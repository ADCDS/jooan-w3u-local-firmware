'use strict';

export class Fmp4Player {
  constructor(video, stream) {
    this.video = video;
    this.stream = stream;
    this.media = null;
    this.buffer = null;
    this.sequence = 0;
    this.failures = 0;
    this.generation = 0;
    this.closed = false;
    this.objectUrl = '';
  }

  async start() {
    if (!('MediaSource' in window)) throw new Error('Media Source Extensions unavailable');
    await this.initialize(++this.generation);
    void this.pump(this.generation);
  }

  async initialize(generation) {
    const mime = this.stream.mime || 'video/mp4; codecs="avc1.640032"';
    if (!MediaSource.isTypeSupported(mime)) throw new Error(`Unsupported codec ${mime}`);
    this.disposeMedia();
    this.sequence = 0;
    this.failures = 0;
    const media = new MediaSource();
    this.media = media;
    this.objectUrl = URL.createObjectURL(media);
    this.video.src = this.objectUrl;
    await new Promise((resolve, reject) => {
      media.addEventListener('sourceopen', resolve, { once: true });
      media.addEventListener('error', () => reject(new Error('MediaSource failed')), { once: true });
    });
    if (this.closed || generation !== this.generation) return;
    this.buffer = media.addSourceBuffer(mime);
    this.buffer.mode = 'segments';
    await this.append((await this.fetch(this.stream.init)).bytes, generation);
    // Browsers may keep play() pending until the first decodable media sample
    // arrives.  Start the fragment pump without waiting for that promise, or
    // an init-only MediaSource deadlocks before its first fragment request.
    void this.video.play().catch(() => {});
  }

  async fetch(url) {
    const response = await window.fetch(url, { credentials: 'same-origin', cache: 'no-store' });
    if (response.status === 401) {
      window.dispatchEvent(new CustomEvent('joan-auth-required'));
      throw new Error('authentication required');
    }
    if (!response.ok) throw new Error(`video HTTP ${response.status}`);
    return {
      bytes: await response.arrayBuffer(),
      sequence: Number(response.headers.get('X-Joan-Sequence') || 0),
    };
  }

  append(bytes, generation) {
    if (this.closed || generation !== this.generation || !this.buffer)
      return Promise.reject(new Error('player generation changed'));
    return new Promise((resolve, reject) => {
      const cleanup = () => {
        this.buffer?.removeEventListener('updateend', done);
        this.buffer?.removeEventListener('error', failed);
      };
      const done = () => { cleanup(); resolve(); };
      const failed = () => { cleanup(); reject(new Error('fragment append failed')); };
      this.buffer.addEventListener('updateend', done, { once: true });
      this.buffer.addEventListener('error', failed, { once: true });
      this.buffer.appendBuffer(bytes);
    });
  }

  async pump(generation) {
    while (!this.closed && generation === this.generation) {
      try {
        const separator = this.stream.fragment.includes('?') ? '&' : '?';
        const part = await this.fetch(`${this.stream.fragment}${separator}after=${this.sequence}`);
        if (!part.sequence || part.sequence <= this.sequence) throw new Error('stale fragment');
        await this.append(part.bytes, generation);
        this.sequence = part.sequence;
        this.failures = 0;
        if (this.buffer.buffered.length) {
          const end = this.buffer.buffered.end(this.buffer.buffered.length - 1);
          if (end - this.video.currentTime > 8) this.video.currentTime = Math.max(0, end - 2);
          if (!this.buffer.updating && this.buffer.buffered.start(0) < end - 30) {
            this.buffer.remove(0, end - 10);
            await new Promise(resolve => this.buffer.addEventListener('updateend', resolve, { once: true }));
          }
        }
      } catch (_) {
        if (this.closed || generation !== this.generation) return;
        if (++this.failures >= 3) {
          const next = ++this.generation;
          try {
            await this.initialize(next);
            void this.pump(next);
            return;
          } catch (_) {
            generation = this.generation;
            this.failures = 0;
          }
        }
        await new Promise(resolve => setTimeout(resolve, 1000));
      }
    }
  }

  disposeMedia() {
    try { if (this.media?.readyState === 'open') this.media.endOfStream(); } catch (_) {}
    this.buffer = null;
    this.media = null;
    this.video.removeAttribute('src');
    this.video.load();
    if (this.objectUrl) URL.revokeObjectURL(this.objectUrl);
    this.objectUrl = '';
  }

  close() {
    this.closed = true;
    ++this.generation;
    this.disposeMedia();
  }
}
