// The browser build's voice chat sound (src/port/netplay/VoiceDeviceWeb.cpp), an AudioWorklet on a sound context
// that runs at the voice's own rate: it writes what the microphone records into one ring in the game's shared memory,
// and plays the other players' voices from another, which the game fills once per frame. Samples are 16 bit, one
// channel, both ways.

class VoiceIo extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { memory, capturedWrite, capturedRead, captured, playbackWrite, playbackRead, playback, capacity } =
        options.processorOptions;
    this.capturedWrite = new Uint32Array(memory, capturedWrite, 1);
    this.capturedRead = new Uint32Array(memory, capturedRead, 1);
    this.captured = new Int16Array(memory, captured, capacity);
    this.playbackWrite = new Uint32Array(memory, playbackWrite, 1);
    this.playbackRead = new Uint32Array(memory, playbackRead, 1);
    this.playback = new Int16Array(memory, playback, capacity);
    this.capacity = capacity;
    this.mask = capacity - 1;
    this.stopped = false;
    this.port.onmessage = () => { this.stopped = true; };
    // Whatever an earlier microphone left queued is not played now.
    Atomics.store(this.playbackRead, 0, Atomics.load(this.playbackWrite, 0));
  }

  process(inputs, outputs) {
    if (this.stopped) {
      return false;
    }
    // No input while the microphone is not connected yet.
    const recorded = inputs[0][0];
    if (recorded) {
      const write = Atomics.load(this.capturedWrite, 0);
      const space = this.capacity - ((write - Atomics.load(this.capturedRead, 0)) >>> 0);
      // What does not fit is dropped: the game has not read for over a second.
      const count = Math.min(recorded.length, space);
      for (let i = 0; i < count; i++) {
        this.captured[(write + i) & this.mask] = Math.max(-32768, Math.min(32767, recorded[i] * 32768));
      }
      Atomics.store(this.capturedWrite, 0, (write + count) >>> 0);
    }
    const played = outputs[0][0];
    const read = Atomics.load(this.playbackRead, 0);
    const queued = (Atomics.load(this.playbackWrite, 0) - read) >>> 0;
    const count = Math.min(played.length, queued);
    for (let i = 0; i < count; i++) {
      played[i] = this.playback[(read + i) & this.mask] / 32768;
    }
    // Silence when the game has queued too little.
    played.fill(0, count);
    Atomics.store(this.playbackRead, 0, (read + count) >>> 0);
    return true;
  }
}

registerProcessor('voice-io', VoiceIo);
