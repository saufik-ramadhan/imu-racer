/**
 * Merges the three steering sources into one value in -1..1.
 *
 * The controller wins whenever it is connected and being moved; otherwise
 * keyboard and pointer stay live. Visitors without the hardware get a fully
 * playable game, which matters for a page on a public site.
 */
export class Input {
  constructor(stage) {
    this.stage = stage;
    this.controller = null;
    this.keyAxis = 0;       // -1, 0, +1 from the arrow keys
    this.pointerAxis = null; // -1..1 while dragging, else null
    this.smoothed = 0;
    this.source = 'keys';
    this._keys = new Set();

    addEventListener('keydown', (e) => this._key(e, true));
    addEventListener('keyup', (e) => this._key(e, false));

    stage.addEventListener('pointerdown', (e) => this._pointer(e, true));
    stage.addEventListener('pointermove', (e) => this._pointer(e, false));
    stage.addEventListener('pointerup', () => { this.pointerAxis = null; });
    stage.addEventListener('pointercancel', () => { this.pointerAxis = null; });
    stage.addEventListener('pointerleave', () => { this.pointerAxis = null; });
  }

  attachController(controller) {
    this.controller = controller;
  }

  _key(e, down) {
    const k = e.key;
    if (k === 'ArrowLeft' || k === 'a' || k === 'A') {
      down ? this._keys.add('l') : this._keys.delete('l');
    } else if (k === 'ArrowRight' || k === 'd' || k === 'D') {
      down ? this._keys.add('r') : this._keys.delete('r');
    } else {
      return;
    }
    e.preventDefault();
    this.keyAxis = (this._keys.has('r') ? 1 : 0) - (this._keys.has('l') ? 1 : 0);
    if (this.keyAxis !== 0) this.source = 'keys';
  }

  _pointer(e, isDown) {
    if (!isDown && e.buttons === 0 && e.pointerType === 'mouse') return;
    const r = this.stage.getBoundingClientRect();
    const x = (e.clientX - r.left) / r.width;         // 0..1
    this.pointerAxis = clamp((x - 0.5) * 2.4, -1, 1); // slight gain, edges reachable
    this.source = 'pointer';
  }

  /** Raw target axis before smoothing. */
  target() {
    if (this.controller?.connected && Math.abs(this.controller.steer) > 0.02) {
      this.source = 'ble';
      return this.controller.steer;
    }
    if (this.pointerAxis !== null) return this.pointerAxis;
    if (this.keyAxis !== 0) return this.keyAxis;
    if (this.controller?.connected) {
      this.source = 'ble';
      return this.controller.steer;
    }
    return 0;
  }

  /**
   * Exponential smoothing, frame-rate independent. The controller is already
   * filtered on the firmware side, so it gets a lighter touch than the keys,
   * which are a hard step input.
   */
  update(dt) {
    const t = this.target();
    const tau = this.source === 'ble' ? 0.06 : 0.11;
    const k = 1 - Math.exp(-dt / tau);
    this.smoothed += (t - this.smoothed) * k;
    return this.smoothed;
  }

  reset() {
    this.smoothed = 0;
    this.pointerAxis = null;
    this._keys.clear();
    this.keyAxis = 0;
  }
}

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
