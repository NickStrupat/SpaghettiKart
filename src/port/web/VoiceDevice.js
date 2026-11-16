/**
 * The browser's microphone and speakers for voice chat (src/port/netplay/VoiceDeviceWeb.cpp). Linked with --js-library
 * (cmake/web/WebTarget.cmake).
 *
 * The browser asks the player for the microphone, and takes the page's own sound out of what it records (echo
 * cancellation), so that players on speakers do not send each other's voices back. A sound context of its own, at the
 * voice's rate, runs VoiceWorklet.js, which moves samples through two rings in the game's shared memory.
 *
 * The game polls: sk_voice_state() says how far opening got.
 */
addToLibrary({
  $SkVoice: {
    // As VoiceDevice::State
    kClosed: 0,
    kStarting: 1,
    kOpen: 2,
    kDenied: 3,
    kFailed: 4,

    state: 0,
    opening: 0, // Counts sk_voice_open() calls: an opening that finishes after a close or a newer open gives up
    stream: null,
    context: null,
    node: null,

    release() {
      const voice = SkVoice;
      if (voice.node) {
        voice.node.port.postMessage('stop');
        voice.node.disconnect();
        voice.node = null;
      }
      if (voice.stream) {
        // Ends the recording: the browser's microphone mark goes out.
        voice.stream.getTracks().forEach((track) => track.stop());
        voice.stream = null;
      }
      if (voice.context) {
        voice.context.close().catch(() => {});
        voice.context = null;
        Module.voiceAudioContext = null;
      }
    },
  },

  /**
   * Starts the microphone and the speakers. The addresses are of the two rings' counts and samples in the game's
   * memory; capacity is each ring's size in samples, a power of two.
   */
  sk_voice_open__deps: ['$SkVoice'],
  sk_voice_open: function(capturedWrite, capturedRead, captured, playbackWrite, playbackRead, playback, capacity,
                          sampleRate) {
    const voice = SkVoice;
    voice.release();
    const opening = ++voice.opening;
    voice.state = voice.kStarting;
    const fail = (error) => {
      if (opening !== voice.opening) {
        return;
      }
      console.warn(`[Voice] ${error}`);
      voice.release();
      const refused = error && ((error.name === 'NotAllowedError') || (error.name === 'SecurityError'));
      voice.state = refused ? voice.kDenied : voice.kFailed;
    };
    if (!navigator.mediaDevices || (typeof AudioWorkletNode === 'undefined') || !self.crossOriginIsolated) {
      fail('This browser cannot record for voice chat');
      return;
    }
    (async () => {
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true, channelCount: 1 },
      });
      if (opening !== voice.opening) {
        stream.getTracks().forEach((track) => track.stop());
        return;
      }
      voice.stream = stream;
      const context = new AudioContext({ sampleRate, latencyHint: 'interactive' });
      voice.context = context;
      // The page resumes it with the game's sound, from the player's touches and keys (shell.html).
      Module.voiceAudioContext = context;
      await context.audioWorklet.addModule('VoiceWorklet.js');
      if (opening !== voice.opening) {
        return;
      }
      voice.node = new AudioWorkletNode(context, 'voice-io', {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
        channelCount: 1,
        channelCountMode: 'explicit',
        processorOptions: {
          memory: HEAPU8.buffer, capturedWrite, capturedRead, captured, playbackWrite, playbackRead, playback, capacity,
        },
      });
      context.createMediaStreamSource(stream).connect(voice.node);
      voice.node.connect(context.destination);
      context.resume().catch(() => {});
      voice.state = voice.kOpen;
    })().catch(fail);
  },

  sk_voice_close__deps: ['$SkVoice'],
  sk_voice_close: function() {
    SkVoice.opening++;
    SkVoice.release();
    SkVoice.state = SkVoice.kClosed;
  },

  /** @returns how far sk_voice_open() got, as VoiceDevice::State. */
  sk_voice_state__deps: ['$SkVoice'],
  sk_voice_state: function() {
    return SkVoice.state;
  },
});
