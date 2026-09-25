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
    this.startedPlayback = false;
    this.requests = new Set();
    this.cancelAppends = new Set();
    this.cancelOpen = null;
    this.cancelBufferWait = null;
    this.cancelDelay = null;
  }

  active(generation) {
    return !this.closed && generation === this.generation;
  }

  cancelPending() {
    for (const request of this.requests) request.abort();
    this.cancelOpen?.();
    for (const cancel of this.cancelAppends) cancel();
    this.cancelBufferWait?.();
    this.cancelDelay?.();
  }

  delay(ms) {
    return new Promise(resolve => {
      let timer;
      const finish = () => {
        clearTimeout(timer);
        if (this.cancelDelay === finish) this.cancelDelay = null;
        resolve();
      };
      this.cancelDelay = finish;
      timer = setTimeout(finish, ms);
    });
  }

  async start() {
    if (this.closed) throw new Error('player closed');
    if (!('MediaSource' in window)) throw new Error('Media Source Extensions unavailable');
    ++this.generation;
    this.cancelPending();
    const generation = this.generation;
    try {
      await this.initialize(generation);
    } catch (error) {
      if (!this.active(generation)) return;
      throw error;
    }
    if (this.active(generation)) void this.pump(generation);
  }

  fragmentUrl(after) {
    const separator = this.stream.fragment.includes('?') ? '&' : '?';
    return `${this.stream.fragment}${separator}after=${after}`;
  }

  validFragment(part, after) {
    return Number.isSafeInteger(part.firstSequence) && part.firstSequence > 0 &&
      Number.isSafeInteger(part.sequence) && part.sequence >= part.firstSequence &&
      (!after || part.firstSequence === after + 1);
  }

  async initialize(generation, initBytes, firstPart) {
    const mime = this.stream.mime || 'video/mp4; codecs="avc1.640032"';
    if (!MediaSource.isTypeSupported(mime)) throw new Error(`Unsupported codec ${mime}`);
    // In recovery, both initBytes and firstPart have already been fetched and
    // checked. Do not touch the visible MSE until that bootstrap is available.
    if (!initBytes) initBytes = (await this.fetch(this.stream.init, generation)).bytes;
    if (!this.active(generation)) return false;

    const media = new MediaSource();
    const objectUrl = URL.createObjectURL(media);
    if (!this.active(generation)) {
      URL.revokeObjectURL(objectUrl);
      return false;
    }
    // Prevent unresolved appends on the previous buffer from advancing its
    // cursor after the new media has taken ownership of this element.
    for (const cancel of this.cancelAppends) cancel();
    this.cancelBufferWait?.();
    this.disposeMedia(false);
    this.media = media;
    this.objectUrl = objectUrl;
    this.sequence = 0;
    this.failures = 0;
    this.startedPlayback = false;
    this.video.src = objectUrl;
    await this.waitForOpen(media, generation);
    if (!this.active(generation)) return false;
    this.buffer = media.addSourceBuffer(mime);
    this.buffer.mode = 'segments';
    await this.append(initBytes, generation);
    if (firstPart) {
      await this.append(firstPart.bytes, generation);
      if (!this.active(generation)) return false;
      this.sequence = firstPart.sequence;
      await this.followLiveEdge(generation);
    }
    if (this.active(generation)) void this.video.play().catch(() => {});
    return this.active(generation);
  }

  waitForOpen(media, generation) {
    return new Promise((resolve, reject) => {
      let settled = false;
      const finish = error => {
        if (settled) return;
        settled = true;
        media.removeEventListener('sourceopen', opened);
        media.removeEventListener('error', failed);
        if (this.cancelOpen === cancel) this.cancelOpen = null;
        if (error) reject(error);
        else resolve();
      };
      const opened = () => finish();
      const failed = () => finish(new Error('MediaSource failed'));
      const cancel = () => finish(new Error('player generation changed'));
      this.cancelOpen = cancel;
      media.addEventListener('sourceopen', opened, { once: true });
      media.addEventListener('error', failed, { once: true });
      if (!this.active(generation)) cancel();
      else if (media.readyState === 'open') opened();
    });
  }

  async fetch(url, generation = this.generation) {
    if (!this.active(generation)) throw new Error('player generation changed');
    const request = new AbortController();
    this.requests.add(request);
    try {
      const response = await window.fetch(url, {
        credentials: 'same-origin', cache: 'no-store', signal: request.signal,
      });
      if (!this.active(generation)) throw new Error('player generation changed');
      if (response.status === 401) {
        window.dispatchEvent(new CustomEvent('joan-auth-required'));
        throw new Error('authentication required');
      }
      if (!response.ok) throw new Error(`video HTTP ${response.status}`);
      const sequence = Number(response.headers.get('X-Joan-Sequence') || 0);
      const firstHeader = response.headers.get('X-Joan-First-Sequence');
      const bytes = await response.arrayBuffer();
      if (!this.active(generation)) throw new Error('player generation changed');
      return {
        bytes,
        sequence,
        // Older daemons return one fragment and only the last-sequence header.
        firstSequence: firstHeader == null ? sequence : Number(firstHeader),
      };
    } finally {
      this.requests.delete(request);
    }
  }

  append(bytes, generation) {
    if (!this.active(generation) || !this.buffer)
      return Promise.reject(new Error('player generation changed'));
    const buffer = this.buffer;
    return new Promise((resolve, reject) => {
      let settled = false;
      const cleanup = () => {
        buffer.removeEventListener('updateend', done);
        buffer.removeEventListener('error', failed);
        this.cancelAppends.delete(cancel);
      };
      const fail = error => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(error);
      };
      const cancel = () => fail(new Error('player generation changed'));
      const done = () => {
        if (settled) return;
        settled = true;
        cleanup();
        if (!this.active(generation) || this.buffer !== buffer)
          reject(new Error('player generation changed'));
        else resolve();
      };
      const failed = () => fail(new Error('fragment append failed'));
      this.cancelAppends.add(cancel);
      buffer.addEventListener('updateend', done, { once: true });
      buffer.addEventListener('error', failed, { once: true });
      try { buffer.appendBuffer(bytes); } catch (error) { fail(error); }
    });
  }

  async followLiveEdge(generation) {
    const buffer = this.buffer;
    if (!this.active(generation) || !buffer?.buffered.length) return;
    const end = buffer.buffered.end(buffer.buffered.length - 1);
    // Start at the live edge and recover if the tab, network or decoder fell
    // behind. Waiting for eight seconds of lag already feels stuck.
    if (!this.startedPlayback || this.video.currentTime < end - 3 ||
        this.video.currentTime < buffer.buffered.start(0)) {
      this.video.currentTime = Math.max(buffer.buffered.start(0), end - 0.7);
      this.startedPlayback = true;
    }
    if (!buffer.updating && buffer.buffered.start(0) < end - 30) {
      buffer.remove(0, end - 10);
      await new Promise(resolve => {
        const finish = () => {
          buffer.removeEventListener('updateend', finish);
          if (this.cancelBufferWait === finish) this.cancelBufferWait = null;
          resolve();
        };
        this.cancelBufferWait = finish;
        buffer.addEventListener('updateend', finish, { once: true });
        if (!this.active(generation)) finish();
      });
    }
  }

  async recover(generation) {
    let failures = 0;
    while (this.active(generation)) {
      try {
        const initBytes = (await this.fetch(this.stream.init, generation)).bytes;
        if (!this.active(generation)) return false;
        const firstPart = await this.fetch(this.fragmentUrl(0), generation);
        if (!this.active(generation)) return false;
        if (!this.validFragment(firstPart, 0)) throw new Error('stale or missing fragment');
        return await this.initialize(generation, initBytes, firstPart);
      } catch (error) {
        if (!this.active(generation) || error.message === 'authentication required') return false;
        // Keep any previously visible media attached while staging retries.
        // Capped exponential delay also applies if after=0 itself returns 409.
        // Retry the entire init/bootstrap pair; never blank the old video here.
        const delay = Math.min(1000 * 2 ** Math.min(failures++, 3), 8000);
        await this.delay(delay);
      }
    }
    return false;
  }

  async pump(generation) {
    while (this.active(generation)) {
      try {
        const part = await this.fetch(this.fragmentUrl(this.sequence), generation);
        if (!this.active(generation)) return;
        if (!this.validFragment(part, this.sequence)) throw new Error('stale or missing fragment');
        await this.append(part.bytes, generation);
        if (!this.active(generation)) return;
        this.sequence = part.sequence;
        this.failures = 0;
        await this.followLiveEdge(generation);
      } catch (error) {
        if (!this.active(generation) || error.message === 'authentication required') return;
        // A 409 or a missing sequence is a different GOP timeline. The payload
        // must never be appended to the current MediaSource.
        const discontinuity = error.message === 'video HTTP 409' ||
          error.message === 'stale or missing fragment';
        if (discontinuity || ++this.failures >= 2) {
          if (!await this.recover(generation)) return;
          continue;
        }
        await this.delay(1000);
      }
    }
  }

  disposeMedia(detachVideo = true) {
    try { if (this.media?.readyState === 'open') this.media.endOfStream(); } catch (_) {}
    this.buffer = null;
    this.media = null;
    if (detachVideo) {
      this.video.removeAttribute('src');
      this.video.load();
    }
    if (this.objectUrl) URL.revokeObjectURL(this.objectUrl);
    this.objectUrl = '';
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    ++this.generation;
    this.cancelPending();
    this.disposeMedia();
  }
}
