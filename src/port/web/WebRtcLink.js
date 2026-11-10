/**
 * WebRTC data channels for the browser build's netplay (src/port/netplay/RelayLinkWeb.cpp): one connection per other
 * member of the room, the browser's way to reach it without going through the relay. Linked with --js-library
 * (cmake/web/WebTarget.cmake).
 *
 * The game polls instead of being called back: everything that happens (a message, a connection opening or closing, a
 * setup message to pass to the other member) waits in a queue for sk_rtc_poll(), which the game calls from its own
 * frame. Each channel is unordered and never retransmits, like UDP: rollback would rather lose a late packet than wait
 * for it.
 */
addToLibrary({
  $SkRtc: {
    peers: {},  // slot -> { pc, channel, open }
    events: [], // { slot, kind, data: Uint8Array }
    kData: 0,
    kSignal: 1,
    kOpen: 2,
    kClosed: 3,

    push(slot, kind, data) {
      SkRtc.events.push({ slot, kind, data: data || new Uint8Array(0) });
    },

    signal(slot, message) {
      console.debug(`[WebRTC] slot ${slot}: sending ${message.type}`);
      SkRtc.push(slot, SkRtc.kSignal, new TextEncoder().encode(JSON.stringify(message)));
    },

    attach(slot, peer, channel) {
      peer.channel = channel;
      channel.binaryType = 'arraybuffer';
      channel.onopen = () => {
        peer.open = true;
        SkRtc.push(slot, SkRtc.kOpen);
      };
      channel.onclose = () => SkRtc.lost(slot, peer);
      channel.onmessage = (event) => {
        if (event.data instanceof ArrayBuffer) {
          SkRtc.push(slot, SkRtc.kData, new Uint8Array(event.data));
        }
      };
    },

    lost(slot, peer) {
      if (SkRtc.peers[slot] === peer && !peer.lost) {
        peer.lost = true;
        peer.open = false;
        SkRtc.push(slot, SkRtc.kClosed);
      }
    },

    async describe(slot, peer, description) {
      try {
        await peer.pc.setLocalDescription(description);
        SkRtc.signal(slot, { type: peer.pc.localDescription.type, sdp: peer.pc.localDescription.sdp });
      } catch (error) {
        console.warn(`[WebRTC] slot ${slot}: ${error}`);
        SkRtc.lost(slot, peer);
      }
    },
  },

  /**
   * Starts a connection to the member in this slot, replacing any earlier one. The initiator makes the offer; the
   * other side waits for it. iceServers: JSON, as RTCPeerConnection takes it.
   */
  sk_rtc_open__deps: ['$SkRtc'],
  sk_rtc_open: function(slot, initiator, iceServersJson) {
    _sk_rtc_close(slot);
    const pc = new RTCPeerConnection({ iceServers: JSON.parse(UTF8ToString(iceServersJson)) });
    const peer = { pc, channel: null, open: false, lost: false, pendingCandidates: [] };
    SkRtc.peers[slot] = peer;
    pc.onicecandidate = (event) => {
      if (event.candidate) {
        SkRtc.signal(slot, { type: 'candidate', candidate: event.candidate.toJSON() });
      }
    };
    console.info(`[WebRTC] slot ${slot}: connecting${initiator ? ', offering' : ''}`);
    pc.onconnectionstatechange = () => {
      console.info(`[WebRTC] slot ${slot}: ${pc.connectionState}`);
      if (pc.connectionState === 'failed' || pc.connectionState === 'closed') {
        SkRtc.lost(slot, peer);
      }
    };
    if (initiator) {
      SkRtc.attach(slot, peer, pc.createDataChannel('sk', { ordered: false, maxRetransmits: 0 }));
      pc.createOffer().then((offer) => SkRtc.describe(slot, peer, offer)).catch(() => SkRtc.lost(slot, peer));
    } else {
      pc.ondatachannel = (event) => SkRtc.attach(slot, peer, event.channel);
    }
  },

  sk_rtc_close__deps: ['$SkRtc'],
  sk_rtc_close: function(slot) {
    const peer = SkRtc.peers[slot];
    if (peer) {
      delete SkRtc.peers[slot];
      peer.pc.close();
    }
    SkRtc.events = SkRtc.events.filter((event) => event.slot !== slot);
  },

  /** A setup message from the member in this slot (JSON, as SkRtc.signal made it), which came through the relay. */
  sk_rtc_signal__deps: ['$SkRtc'],
  sk_rtc_signal: function(slot, data, size) {
    const peer = SkRtc.peers[slot];
    if (!peer) {
      return;
    }
    let message;
    try {
      message = JSON.parse(new TextDecoder().decode(HEAPU8.slice(data, data + size)));
    } catch (error) {
      return;
    }
    console.debug(`[WebRTC] slot ${slot}: received ${message.type}`);
    const pc = peer.pc;
    const addCandidates = () => {
      for (const candidate of peer.pendingCandidates) {
        pc.addIceCandidate(candidate).catch(() => {});
      }
      peer.pendingCandidates = [];
    };
    if (message.type === 'offer') {
      pc.setRemoteDescription(message)
        .then(() => { addCandidates(); return pc.createAnswer(); })
        .then((answer) => SkRtc.describe(slot, peer, answer))
        .catch(() => SkRtc.lost(slot, peer));
    } else if (message.type === 'answer') {
      pc.setRemoteDescription(message).then(addCandidates).catch(() => SkRtc.lost(slot, peer));
    } else if (message.type === 'candidate') {
      // Candidates can arrive before the description they belong to.
      if (pc.remoteDescription) {
        pc.addIceCandidate(message.candidate).catch(() => {});
      } else {
        peer.pendingCandidates.push(message.candidate);
      }
    }
  },

  /** @returns 1 if the message went out on an open channel. */
  sk_rtc_send__deps: ['$SkRtc'],
  sk_rtc_send: function(slot, data, size) {
    const peer = SkRtc.peers[slot];
    if (!peer || !peer.open || peer.channel.readyState !== 'open') {
      return 0;
    }
    try {
      peer.channel.send(HEAPU8.slice(data, data + size));
      return 1;
    } catch (error) {
      return 0;
    }
  },

  /**
   * Takes the oldest event: writes its slot and kind, copies up to `capacity` bytes of its data to `buffer`.
   * @returns the data's size, or -1 when nothing is waiting.
   */
  sk_rtc_poll__deps: ['$SkRtc'],
  sk_rtc_poll: function(slotOut, kindOut, buffer, capacity) {
    const event = SkRtc.events.shift();
    if (!event) {
      return -1;
    }
    HEAP32[slotOut >> 2] = event.slot;
    HEAP32[kindOut >> 2] = event.kind;
    const size = Math.min(event.data.length, capacity);
    HEAPU8.set(event.data.subarray(0, size), buffer);
    return size;
  },

  /** @returns 1 when the page was served over HTTPS, where it may only open secure WebSockets (wss://). */
  sk_page_is_secure: function() {
    return location.protocol === 'https:' ? 1 : 0;
  },
});
