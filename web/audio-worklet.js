import {
  AUDIO_PACKET_SAMPLES,
  AUDIO_SAMPLE_RATE,
  alawDecodeSample,
  alawEncodeSample,
} from './audio-codec.js';

const PLAYBACK_PREROLL_SAMPLES = AUDIO_PACKET_SAMPLES * 3;

class SampleRing {
  constructor(capacity) {
    this.samples = new Float32Array(capacity);
    this.readIndex = 0;
    this.length = 0;
  }

  clear() {
    this.readIndex = 0;
    this.length = 0;
  }

  push(value) {
    if (this.length === this.samples.length) {
      this.readIndex = (this.readIndex + 1) % this.samples.length;
      this.length -= 1;
    }
    const index = (this.readIndex + this.length) % this.samples.length;
    this.samples[index] = value;
    this.length += 1;
  }

  peek(offset = 0) {
    if (offset >= this.length) return 0;
    return this.samples[(this.readIndex + offset) % this.samples.length];
  }

  shift() {
    if (this.length === 0) return 0;
    const value = this.samples[this.readIndex];
    this.readIndex = (this.readIndex + 1) % this.samples.length;
    this.length -= 1;
    return value;
  }
}

class JooanAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.talking = false;
    this.playback = new SampleRing(AUDIO_SAMPLE_RATE * 2);
    this.playbackPhase = 0;
    this.playbackStarted = false;
    this.capturePacket = new Uint8Array(AUDIO_PACKET_SAMPLES);
    this.captureLength = 0;
    this.captureInputIndex = -1;
    this.captureNextOutput = 0;
    this.capturePrevious = 0;
    this.captureFilter = 0;
    this.port.onmessage = ({ data }) => this.onMessage(data);
  }

  onMessage(message) {
    if (!message || typeof message.type !== 'string') return;
    if (message.type === 'ptt') {
      this.talking = Boolean(message.active);
      this.resetCapture();
      if (this.talking) {
        this.playback.clear();
        this.playbackPhase = 0;
        this.playbackStarted = false;
      }
      return;
    }
    if (message.type === 'pcma' && !this.talking && message.data) {
      const bytes = new Uint8Array(message.data);
      for (let i = 0; i < bytes.length; i += 1) {
        this.playback.push(alawDecodeSample(bytes[i]) / 32768);
      }
    }
  }

  resetCapture() {
    this.captureLength = 0;
    this.captureInputIndex = -1;
    this.captureNextOutput = 0;
    this.capturePrevious = 0;
    this.captureFilter = 0;
  }

  emitCaptureSample(value) {
    const clipped = Math.max(-1, Math.min(1, value));
    const pcm = clipped < 0 ? Math.round(clipped * 32768) : Math.round(clipped * 32767);
    this.capturePacket[this.captureLength] = alawEncodeSample(pcm);
    this.captureLength += 1;
    if (this.captureLength === this.capturePacket.length) {
      const packet = this.capturePacket.buffer;
      this.port.postMessage({ type: 'pcma', data: packet }, [packet]);
      this.capturePacket = new Uint8Array(AUDIO_PACKET_SAMPLES);
      this.captureLength = 0;
    }
  }

  capture(input) {
    const step = sampleRate / AUDIO_SAMPLE_RATE;
    const filterAlpha = 1 - Math.exp(-2 * Math.PI * 7000 / sampleRate);
    for (let i = 0; i < input.length; i += 1) {
      if (this.captureInputIndex < 0) this.captureFilter = input[i];
      else this.captureFilter += filterAlpha * (input[i] - this.captureFilter);
      const current = this.captureFilter;
      this.captureInputIndex += 1;
      if (this.captureInputIndex === 0) {
        this.capturePrevious = current;
        this.emitCaptureSample(current);
        this.captureNextOutput = step;
        continue;
      }
      while (this.captureNextOutput <= this.captureInputIndex) {
        const fraction = this.captureNextOutput - (this.captureInputIndex - 1);
        this.emitCaptureSample(this.capturePrevious +
          (current - this.capturePrevious) * fraction);
        this.captureNextOutput += step;
      }
      this.capturePrevious = current;
    }
  }

  renderPlayback(output) {
    const step = AUDIO_SAMPLE_RATE / sampleRate;
    if (!this.playbackStarted) {
      if (this.playback.length < PLAYBACK_PREROLL_SAMPLES) {
        output.fill(0);
        return;
      }
      this.playbackStarted = true;
    }
    for (let i = 0; i < output.length; i += 1) {
      if (this.playback.length < 2) {
        output[i] = 0;
        this.playbackPhase = 0;
        this.playbackStarted = false;
        continue;
      }
      const first = this.playback.peek(0);
      const second = this.playback.peek(1);
      output[i] = first + (second - first) * this.playbackPhase;
      this.playbackPhase += step;
      while (this.playbackPhase >= 1 && this.playback.length) {
        this.playback.shift();
        this.playbackPhase -= 1;
      }
    }
  }

  process(inputs, outputs) {
    const input = inputs[0]?.[0];
    const channels = outputs[0] || [];
    if (this.talking && input) this.capture(input);
    for (let channel = 0; channel < channels.length; channel += 1) {
      if (this.talking) channels[channel].fill(0);
      else this.renderPlayback(channels[channel]);
    }
    return true;
  }
}

registerProcessor('jooan-audio', JooanAudioProcessor);
