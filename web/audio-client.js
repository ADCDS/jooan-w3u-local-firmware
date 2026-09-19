import {
  AUDIO_PACKET_SAMPLES,
  AUDIO_WS_AUTH_PREFIX,
  AUDIO_WS_PROTOCOL,
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
    if (!/^[A-Za-z0-9_-]{16,512}$/.test(authToken || '')) {
      throw new TypeError('authToken must be a base64url one-time token');
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
    this.talking = false;
    this.closed = false;
    this.releaseHandlers = [];
  }

  async connect() {
    if (this.closed || this.socket) throw new Error('audio client is not reusable');
    this.context = new AudioContext({ latencyHint: 'interactive' });
    await this.context.audioWorklet.addModule(this.workletUrl);
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
    socket.onclose = () => this.failSafeRelease('disconnect', false);
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
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return false;
    this.socket.send(buildAudioFrame(type, this.nextSequence(), payload));
    return true;
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
      this.talking = true;
      this.node.port.postMessage({ type: 'ptt', active: true });
      this.dispatchEvent(new CustomEvent('talkstate', { detail: { active: true } }));
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
    const wasActive = this.wantTalk || this.talking;
    this.wantTalk = false;
    this.talkGeneration += 1;
    if (this.node) this.node.port.postMessage({ type: 'ptt', active: false });
    if (this.talking && notifyServer) this.send(AudioWireType.PTT_RELEASE);
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
      } else if (frame.type === AudioWireType.STATE || frame.type === AudioWireType.ERROR) {
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
