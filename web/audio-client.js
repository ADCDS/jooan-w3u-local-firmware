import {
  AUDIO_PACKET_SAMPLES,
  AUDIO_BACKPRESSURE_BYTES,
  AUDIO_MAX_TALK_MS,
  AUDIO_WS_AUTH_PREFIX,
  AUDIO_WS_PROTOCOL,
  AudioState,
  AudioWireType,
  buildAudioFrame,
  parseAudioFrame,
} from './audio-codec.js';

export class JooanAudioClient extends EventTarget {
  constructor({ url, authToken, workletUrl = './audio-worklet.js', allowInsecure = false }) {
    super();
    const parsed = new URL(url, window.location.href);
    if (parsed.protocol !== 'wss:' && !allowInsecure) {
      throw new TypeError('audio WebSocket must use wss:');
    }
    if (parsed.search || parsed.hash) {
      throw new TypeError('do not place audio credentials in the WebSocket URL');
    }
    if (!/^[0-9a-f]{64}$/.test(authToken || '')) {
      throw new TypeError('authToken must be the current 64-character CSRF token');
    }
    this.url = parsed.href;
    this.authProtocol = `${AUDIO_WS_AUTH_PREFIX}${authToken}`;
    this.workletUrl = workletUrl;
    this.socket = null;
    this.context = null;
    this.node = null;
    this.mediaStream = null;
    this.mediaSource = null;
    this.sendSequence = 0;
    this.receiveSequence = 0;
    this.talkGeneration = 0;
    this.wantTalk = false;
    this.acquirePending = false;
    this.talking = false;
    this.acquireTimer = null;
    this.leaseTimer = null;
    this.closed = false;
    this.releaseHandlers = [];
  }

  async connect() {
    if (this.closed || this.socket) throw new Error('audio client is not reusable');
    this.context = new AudioContext({ latencyHint: 'interactive' });
    const resume = this.context.resume();
    await this.context.audioWorklet.addModule(this.workletUrl);
    await resume;
    this.node = new AudioWorkletNode(this.context, 'jooan-audio', {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [1],
    });
    this.node.port.onmessage = ({ data }) => this.onWorkletMessage(data);
    this.node.connect(this.context.destination);

    const socket = new WebSocket(this.url, [AUDIO_WS_PROTOCOL, this.authProtocol]);
    socket.binaryType = 'arraybuffer';
    this.socket = socket;
    socket.onmessage = (event) => this.onSocketMessage(event);
    socket.onclose = () => {
      this.failSafeRelease('disconnect', false);
      this.dispatchEvent(new Event('close'));
    };
    socket.onerror = () => this.failSafeRelease('socket-error', false);
    await new Promise((resolve, reject) => {
      socket.onopen = () => {
        if (socket.protocol !== AUDIO_WS_PROTOCOL) {
          socket.close(1002, 'protocol not authenticated');
          reject(new Error('server did not select authenticated audio protocol'));
          return;
        }
        this.installGlobalReleaseHandlers();
        this.dispatchEvent(new Event('open'));
        resolve();
      };
      socket.addEventListener('error', () => reject(new Error('audio WebSocket failed')), { once: true });
    });
  }

  nextSequence() {
    this.sendSequence = this.sendSequence === 0xffffffff ? 1 : this.sendSequence + 1;
    return this.sendSequence;
  }

  send(type, payload) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN ||
        this.socket.bufferedAmount > AUDIO_BACKPRESSURE_BYTES) return false;
    try {
      this.socket.send(buildAudioFrame(type, this.nextSequence(), payload));
      return true;
    } catch (_) {
      return false;
    }
  }

  async ensureMicrophone(generation) {
    if (this.mediaStream) return true;
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        channelCount: 1,
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: true,
      },
      video: false,
    });
    if (!this.wantTalk || generation !== this.talkGeneration || this.closed) {
      stream.getTracks().forEach((track) => track.stop());
      return false;
    }
    this.mediaStream = stream;
    this.mediaSource = this.context.createMediaStreamSource(stream);
    this.mediaSource.connect(this.node);
    return true;
  }

  async pressToTalk() {
    if (this.closed || this.wantTalk || this.talking ||
        !this.socket || this.socket.readyState !== WebSocket.OPEN) return;
    this.wantTalk = true;
    const generation = ++this.talkGeneration;
    try {
      await this.context.resume();
      if (!await this.ensureMicrophone(generation) || !this.wantTalk) return;
      if (!this.send(AudioWireType.PTT_ACQUIRE)) throw new Error('audio socket closed');
      this.acquirePending = true;
      this.acquireTimer = setTimeout(() => {
        this.failSafeRelease('acquire-timeout', true);
        this.socket?.close(1011, 'audio acquire timeout');
      }, 2000);
    } catch (error) {
      this.failSafeRelease('microphone-error', true);
      this.dispatchEvent(new CustomEvent('audioerror', { detail: error }));
    }
  }

  releaseTalk(reason = 'release') {
    this.failSafeRelease(reason, true);
  }

  stopMicrophone() {
    if (this.mediaSource) {
      try { this.mediaSource.disconnect(); } catch (_) { /* already disconnected */ }
    }
    if (this.mediaStream) this.mediaStream.getTracks().forEach((track) => track.stop());
    this.mediaSource = null;
    this.mediaStream = null;
  }

  failSafeRelease(reason, notifyServer) {
    const wasActive = this.wantTalk || this.acquirePending || this.talking;
    this.wantTalk = false;
    this.talkGeneration += 1;
    clearTimeout(this.acquireTimer);
    clearTimeout(this.leaseTimer);
    this.acquireTimer = null;
    this.leaseTimer = null;
    if (this.node) this.node.port.postMessage({ type: 'ptt', active: false });
    if ((this.talking || this.acquirePending) && notifyServer &&
        !this.send(AudioWireType.PTT_RELEASE)) {
      this.socket?.close(1011, 'audio release backpressure');
    }
    this.acquirePending = false;
    this.talking = false;
    this.stopMicrophone();
    if (wasActive) {
      this.dispatchEvent(new CustomEvent('talkstate', {
        detail: { active: false, reason },
      }));
    }
  }

  onWorkletMessage(message) {
    if (!message || message.type !== 'pcma' || !this.talking) return;
    const payload = new Uint8Array(message.data);
    if (payload.byteLength !== AUDIO_PACKET_SAMPLES ||
        !this.send(AudioWireType.PTT_PCMA, payload)) {
      this.failSafeRelease('send-failed', true);
    }
  }

  onSocketMessage(event) {
    try {
      if (!(event.data instanceof ArrayBuffer)) throw new TypeError('text audio frame');
      const frame = parseAudioFrame(event.data);
      const expected = this.receiveSequence === 0xffffffff ? 1 : this.receiveSequence + 1;
      if (frame.sequence !== expected) throw new TypeError('audio sequence gap');
      this.receiveSequence = frame.sequence;
      if (frame.type === AudioWireType.MIC_PCMA) {
        if (!this.talking) {
          const copy = frame.payload.slice().buffer;
          this.node.port.postMessage({ type: 'pcma', data: copy }, [copy]);
        }
      } else if (frame.type === AudioWireType.STATE) {
        if (frame.payload.byteLength !== 1) throw new TypeError('invalid audio state');
        const state = frame.payload[0];
        if (state === AudioState.ACQUIRED) {
          clearTimeout(this.acquireTimer);
          this.acquireTimer = null;
          this.acquirePending = false;
          if (this.wantTalk) {
            this.talking = true;
            this.node.port.postMessage({ type: 'ptt', active: true });
            this.leaseTimer = setTimeout(
              () => this.releaseTalk('lease-expired'), AUDIO_MAX_TALK_MS);
            this.dispatchEvent(new CustomEvent('talkstate', { detail: { active: true } }));
          }
        } else if (state === AudioState.RELEASED ||
                   state === AudioState.LEASE_EXPIRED) {
          this.failSafeRelease(
            state === AudioState.LEASE_EXPIRED ? 'lease-expired' : 'released', false);
        } else {
          throw new TypeError('unknown audio state');
        }
        this.dispatchEvent(new CustomEvent('servermessage', { detail: frame }));
      } else if (frame.type === AudioWireType.ERROR) {
        this.dispatchEvent(new CustomEvent('servermessage', { detail: frame }));
      } else {
        throw new TypeError('unexpected server audio frame');
      }
    } catch (error) {
      this.failSafeRelease('protocol-error', true);
      this.socket?.close(1002, 'invalid audio frame');
      this.dispatchEvent(new CustomEvent('audioerror', { detail: error }));
    }
  }

  bindPressToTalk(element) {
    const down = (event) => {
      event.preventDefault();
      if (event.pointerId !== undefined) element.setPointerCapture?.(event.pointerId);
      void this.pressToTalk();
    };
    const up = (event) => {
      event.preventDefault();
      this.releaseTalk(event.type);
    };
    const keyDown = (event) => {
      if ((event.code === 'Space' || event.code === 'Enter') && !event.repeat) down(event);
    };
    const keyUp = (event) => {
      if (event.code === 'Space' || event.code === 'Enter') up(event);
    };
    element.addEventListener('pointerdown', down);
    for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) {
      element.addEventListener(name, up);
    }
    element.addEventListener('keydown', keyDown);
    element.addEventListener('keyup', keyUp);
    element.addEventListener('blur', up);
    return () => {
      element.removeEventListener('pointerdown', down);
      for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) {
        element.removeEventListener(name, up);
      }
      element.removeEventListener('keydown', keyDown);
      element.removeEventListener('keyup', keyUp);
      element.removeEventListener('blur', up);
      this.releaseTalk('unbind');
    };
  }

  installGlobalReleaseHandlers() {
    const hidden = () => {
      if (document.visibilityState !== 'visible') this.releaseTalk('hidden');
    };
    const leaving = () => this.releaseTalk('pagehide');
    document.addEventListener('visibilitychange', hidden);
    window.addEventListener('pagehide', leaving);
    window.addEventListener('beforeunload', leaving);
    this.releaseHandlers.push(
      () => document.removeEventListener('visibilitychange', hidden),
      () => window.removeEventListener('pagehide', leaving),
      () => window.removeEventListener('beforeunload', leaving),
    );
  }

  async close() {
    if (this.closed) return;
    this.failSafeRelease('close', true);
    this.closed = true;
    this.releaseHandlers.splice(0).forEach((remove) => remove());
    if (this.socket && this.socket.readyState < WebSocket.CLOSING) this.socket.close(1000);
    this.socket = null;
    if (this.node) this.node.disconnect();
    this.node = null;
    if (this.context) await this.context.close();
    this.context = null;
  }
}
