#ifndef NETPLAY_WEBRTC_TRANSPORT_H
#define NETPLAY_WEBRTC_TRANSPORT_H

#include <libultraship/libultraship.h>

/**
 * WebRTC data channels, one per other member of a room, for WebRtcPeers.h. The browser build gets them from the
 * browser (src/port/web/WebRtcLink.js), the desktop game from libdatachannel (WebRtcTransportDesktop.cpp).
 *
 * Nothing calls back: what happens (a message, a channel opening or closing, a setup message to pass to the other
 * member through the relay) waits in a queue for sk_rtc_poll(), called from the game thread. Channels are unordered
 * and never retransmit, like UDP. Setup messages are JSON: {"type":"offer"|"answer","sdp":...} or
 * {"type":"candidate","candidate":{"candidate":...,"sdpMid":...}}, as a browser's RTCPeerConnection makes them.
 */

#ifdef __cplusplus
extern "C" {
#endif

// Kinds of event sk_rtc_poll() returns
#define SK_RTC_EVENT_DATA 0
#define SK_RTC_EVENT_SIGNAL 1
#define SK_RTC_EVENT_OPEN 2
#define SK_RTC_EVENT_CLOSED 3

/**
 * Starts a connection to the member in this slot, replacing any earlier one. The initiator makes the offer and the
 * channel; the other side waits for the offer. iceServers: JSON, as RTCPeerConnection takes it.
 */
void sk_rtc_open(int slot, int initiator, const char* iceServersJson);
/** Ends the connection to this slot and drops its events still waiting. */
void sk_rtc_close(int slot);
/** A setup message from the member in this slot, which came through the relay. */
void sk_rtc_signal(int slot, const u8* data, int size);
/** @returns 1 if the message went out on an open channel. */
int sk_rtc_send(int slot, const u8* data, int size);
/**
 * Takes the oldest event: writes its slot and kind, copies up to `capacity` bytes of its data to `buffer`.
 * @returns the data's size, or -1 when nothing is waiting.
 */
int sk_rtc_poll(int* slot, int* kind, u8* buffer, int capacity);

#ifdef __cplusplus
}
#endif

#endif // NETPLAY_WEBRTC_TRANSPORT_H
