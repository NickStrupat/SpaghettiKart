// The game's sound output in the browser (src/port/audio/WebAudioOutput.h), an AudioWorklet: it runs on the browser's
// audio thread, so a busy page does not interrupt the sound. The game's sound thread writes 16-bit stereo frames at the
// game's rate into a ring in the game's shared memory; this plays them, converted to the output's rate.

class GameSoundOutput extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { memory, writeIndex, readIndex, samples, capacity, rate, buffered } = options.processorOptions;
    this.write = new Uint32Array(memory, writeIndex, 1);
    this.read = new Uint32Array(memory, readIndex, 1);
    this.samples = new Int16Array(memory, samples, capacity * 2);
    this.mask = capacity - 1;
    // Game frames per output frame: below 1, as the game's rate is the lower one.
    this.step = rate / sampleRate;
    this.buffered = buffered;
    // Where between the frame at the read count and the next one the output is.
    this.position = 0;
    // After running out, it waits for half the usual cushion before playing again, rather than flickering on and off.
    this.starved = true;
    this.underruns = 0;
    this.blocks = 0;
  }

  process(inputs, outputs) {
    const left = outputs[0][0];
    const right = outputs[0][1];
    const write = Atomics.load(this.write, 0);
    let read = Atomics.load(this.read, 0);
    let queued = (write - read) >>> 0;
    if (queued > this.buffered * 3) {
      // Sound piled up while the output could not play (before the player first touched the page): play only the
      // newest, or the sound would stay that far behind the game.
      read = (write - this.buffered) >>> 0;
      queued = this.buffered;
      this.position = 0;
    }
    if (this.starved && queued < this.buffered / 2) {
      left.fill(0);
      right.fill(0);
    } else {
      this.starved = false;
      for (let i = 0; i < left.length; i++) {
        if (queued < 2) {
          left.fill(0, i);
          right.fill(0, i);
          this.starved = true;
          this.underruns++;
          break;
        }
        const a = (read & this.mask) * 2;
        const b = ((read + 1) & this.mask) * 2;
        const t = this.position;
        left[i] = (this.samples[a] + (this.samples[b] - this.samples[a]) * t) / 32768;
        right[i] = (this.samples[a + 1] + (this.samples[b + 1] - this.samples[a + 1]) * t) / 32768;
        this.position += this.step;
        if (this.position >= 1) {
          this.position -= 1;
          read = (read + 1) >>> 0;
          queued--;
        }
      }
    }
    Atomics.store(this.read, 0, read);
    // For the page's ?dbg=1 readout, a few times a second.
    if (++this.blocks % 64 === 0) {
      this.port.postMessage({ queued, underruns: this.underruns });
    }
    return true;
  }
}

registerProcessor('game-sound-output', GameSoundOutput);
