// frame-selection.js — browser-side frame selection for the llama.cpp webcam agent.
//
// Ports mlx-vlm-private:continuous-analyzer  watchdawg/adaptive.py to JS/WebCodecs:
//   - PixelDiffFilter  : MSE on a 64x64 RGB thumbnail   (always available; canvas only)
//   - codec heuristic  : EncodedVideoChunk.type/byteLength  (WebCodecs; optional enhancement)
//   - selection policy : keyframe-always / perceptual-gate / motion-tag + interval governors
//
// See ../02-frame-selection-perceptual-and-codec.md. No build step; pure ES module.

export const DEFAULTS = {
  thumb: 64,            // MLX PixelDiffFilter thumb_size
  mseThreshold: 0.005,  // MLX skip threshold (normalized 0..1)
  thresholdFactor: 2.0, // MLX ChangeDetector: P-frame size >= 2x rolling avg => process
  window: 30,           // rolling-average window (frames)
  warmup: 15,           // always-process warmup frames (MLX window_size/2)
  maxFps: 3,            // never send faster than this regardless of motion
  idleHeartbeatS: 10,   // force one frame this often even if static ("still quiet")
};

// ---- Layer 1: perceptual MSE on a downscaled thumbnail (decode-domain) ----
export class PixelDiffFilter {
  constructor({ thumb = DEFAULTS.thumb, mseThreshold = DEFAULTS.mseThreshold } = {}) {
    this.thumb = thumb; this.mseThreshold = mseThreshold; this.prev = null;
    this.canvas = new OffscreenCanvas(thumb, thumb);
    this.ctx = this.canvas.getContext('2d', { willReadFrequently: true });
  }
  // src: VideoFrame | HTMLVideoElement | canvas-drawable. Returns {changed, mse}.
  test(src) {
    this.ctx.drawImage(src, 0, 0, this.thumb, this.thumb);
    const { data } = this.ctx.getImageData(0, 0, this.thumb, this.thumb); // RGBA u8
    if (!this.prev) { this.prev = Float32Array.from(data, v => v / 255); return { changed: true, mse: 1 }; }
    let sum = 0;
    for (let i = 0; i < data.length; i++) { const d = data[i] / 255 - this.prev[i]; sum += d * d; this.prev[i] = data[i] / 255; }
    const mse = sum / data.length;
    return { changed: mse >= this.mseThreshold, mse };
  }
}

// ---- Layer 2: codec-level change detector (mirrors ChangeDetector) ----
// Fed EncodedVideoChunk objects from a WebCodecs VideoEncoder running over the stream.
export class CodecChangeDetector {
  constructor({ thresholdFactor = DEFAULTS.thresholdFactor, window = DEFAULTS.window, warmup = DEFAULTS.warmup } = {}) {
    this.thresholdFactor = thresholdFactor; this.window = window; this.warmup = warmup;
    this.rollingAvg = 0; this.n = 0; this.last = { process: true, isKeyframe: false, motion: 0.5 };
  }
  feed(chunk /* EncodedVideoChunk */) {
    const size = chunk.byteLength;
    this.rollingAvg = this.n === 0 ? size : this.rollingAvg + (size - this.rollingAvg) / Math.min(this.n + 1, this.window);
    this.n++;
    if (chunk.type === 'key') return (this.last = { process: true, isKeyframe: true, motion: 1.0 });
    if (this.n < this.warmup) return (this.last = { process: true, isKeyframe: false, motion: 0.5 });
    const ratio = size / (this.rollingAvg || 1);
    return (this.last = { process: ratio >= this.thresholdFactor, isKeyframe: false, motion: Math.min(ratio / this.thresholdFactor, 1) });
  }
  latest() { return this.last; }
}

// ---- Selection policy: combine layers + interval governors ----
export class FrameSelector {
  constructor(opts = {}) {
    this.o = { ...DEFAULTS, ...opts };
    this.pixel = new PixelDiffFilter(this.o);
    this.codec = ('VideoEncoder' in globalThis) ? new CodecChangeDetector(this.o) : null;
    this.codecAvailable = !!this.codec;
    this.lastSentMs = -1e12; this.lastAnyMs = -1e12;
  }
  feedCodecChunk(chunk) { if (this.codec) this.codec.feed(chunk); }
  // Call per sampled candidate frame. nowMs default performance.now().
  // Returns { send, motion, isKeyframe, reason, mse }.
  consider(src, nowMs = performance.now()) {
    const minGap = 1000 / this.o.maxFps;
    const { changed, mse } = this.pixel.test(src);
    const cv = this.codec ? this.codec.latest() : null;
    const heartbeat = (nowMs - this.lastSentMs) >= this.o.idleHeartbeatS * 1000;
    let send = false, reason = 'static', motion = cv ? cv.motion : (changed ? 0.5 : 0), isKeyframe = !!(cv && cv.isKeyframe);

    if (nowMs - this.lastSentMs < minGap && !isKeyframe) { reason = 'rate-limited'; }
    else if (isKeyframe)         { send = true; reason = 'keyframe'; motion = 1.0; }
    else if (changed)            { send = true; reason = 'perceptual'; }
    else if (heartbeat)          { send = true; reason = 'heartbeat'; motion = 0; }

    this.lastAnyMs = nowMs;
    if (send) this.lastSentMs = nowMs;
    return { send, motion, isKeyframe, reason, mse };
  }
}

// ---- Optional: wire a VideoEncoder over a MediaStreamTrack to feed codec hints ----
// Returns a stop() fn. Silently no-ops if WebCodecs is unavailable.
export function attachCodecHints(track, selector, { width, height, framerate = 30 } = {}) {
  if (!('VideoEncoder' in globalThis) || !('MediaStreamTrackProcessor' in globalThis)) return () => {};
  let stopped = false;
  const encoder = new VideoEncoder({ output: (chunk) => selector.feedCodecChunk(chunk), error: (e) => console.warn('VideoEncoder', e) });
  try {
    encoder.configure({ codec: 'avc1.42001f', width, height, framerate, latencyMode: 'realtime', bitrateMode: 'variable' });
  } catch (e) { console.warn('VideoEncoder.configure failed; codec hints off', e); return () => {}; }
  (async () => {
    const reader = new MediaStreamTrackProcessor({ track }).readable.getReader();
    while (!stopped) {
      const { value: frame, done } = await reader.read(); if (done) break;
      try { if (encoder.encodeQueueSize < 2) encoder.encode(frame); } catch {} finally { frame.close(); }
    }
  })();
  return () => { stopped = true; try { encoder.close(); } catch {} };
}
