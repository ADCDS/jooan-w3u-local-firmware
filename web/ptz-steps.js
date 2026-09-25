'use strict';

/* One in-flight nudge per browser. Stop is a separate, prioritized action;
 * the camera also serializes Move/Stop and owns the final short deadman. */
export class PtzSteps {
  constructor({ request, video, state, settled, inputs, canMove = () => true }) {
    this.request = request;
    this.video = video;
    this.state = state;
    this.settled = settled;
    this.inputs = inputs;
    this.canMove = canMove;
    this.mode = 'fine';
    this.generation = 0;
    this.busy = false;
    this.lease = '';
    this.stopTask = null;
    this.uncertain = false;
    this.stopFailed = false;
  }
  setMode(mode) { if (!this.busy && (mode === 'fine' || mode === 'coarse')) this.mode = mode; }
  async stop() {
    ++this.generation;
    if (this.stopTask) return this.stopTask;
    const lease = this.lease;
    if (!lease) return;
    this.state('Stopping…');
    this.stopTask = this.request('/api/v1/ptz/stop', { lease }).then(() => {
      if (this.lease === lease) this.lease = '';
      this.uncertain = false;
      this.state('Stopped');
    }).catch(error => {
      if (error.code === 'invalid_lease' && !this.stopFailed) {
        if (this.lease === lease) this.lease = '';
        this.state('Move expired · check live picture before another nudge');
        return;
      }
      this.stopFailed = this.uncertain = true;
      this.state('Stop not confirmed · check the camera');
      throw error;
    }).finally(() => { this.stopTask = null; });
    return this.stopTask;
  }
  async nudge(command) {
    if (this.busy || this.stopTask || this.uncertain || !this.canMove() || document.hidden) return false;
    this.stopFailed = false;
    const step = this.mode === 'fine' ? { duration_ms: 100, speed: 1 } : { duration_ms: 350, speed: 3 };
    const generation = ++this.generation;
    this.busy = true;
    this.inputs(true);
    this.state(`Sending ${this.mode} ${command} nudge…`);
    let lease = '';
    try {
      lease = (await this.request('/api/v1/ptz/lease')).lease;
      this.lease = lease;
      if (generation !== this.generation || document.hidden) return false;
      const video = this.video();
      const available = video?.getVideoPlaybackQuality?.().totalVideoFrames ?? null;
      if (!video || video.readyState < 2 || available == null || video.paused) {
        this.state('Live video unavailable');
        return false;
      }
      const result = await this.request('/api/v1/ptz/move', { lease, command, ...step });
      if (!result?.stopped) { this.uncertain = this.stopFailed = true; throw new Error('Camera stop was not confirmed'); }
      if (this.lease === lease) this.lease = '';
      lease = '';
      if (generation !== this.generation || document.hidden) return false;
      const afterStop = video.getVideoPlaybackQuality?.().totalVideoFrames ?? null;
      this.state('Waiting for updated live picture…');
      const updated = await this.settled(video, afterStop, () => generation === this.generation);
      if (generation === this.generation) this.state(updated
        ? 'Live video advanced · confirm position visually' : 'Picture not confirmed · check live view');
      return updated;
    } catch (error) {
      if (generation === this.generation)
        this.state(error.code === 'ptz_stop_uncertain' ? 'Stop not confirmed · check the camera'
          : error.message || 'Camera did not take that nudge');
      return false;
    } finally {
      if (lease && this.lease === lease && !this.stopFailed) {
        try { if (this.stopTask) await this.stopTask; else await this.stop(); }
        catch (_) { /* stop() already reported uncertainty */ }
      }
      this.busy = false;
      this.inputs(false);
    }
  }
}
