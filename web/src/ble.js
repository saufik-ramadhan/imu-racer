/**
 * Web Bluetooth client for the ESP32-S3 + GY-87 controller.
 *
 * The UUIDs and packet layout here must stay in sync with
 * blink/main/ble_controller.c on the firmware side.
 *
 * Telemetry packet, 8 bytes little-endian:
 *   int16  steer    -1000..1000   roll angle, normalised
 *   int16  pitch    -1000..1000   reserved for throttle
 *   uint8  buttons  bit0 = user button
 *   uint8  flags    bit0 = zero offset captured
 *   uint16 seq      wraps, used to spot dropped notifications
 */
export const SERVICE_UUID = '4f1a0000-8b2c-4f5e-9d3a-1c7e6b9f0a01';
export const TELEMETRY_UUID = '4f1a0001-8b2c-4f5e-9d3a-1c7e6b9f0a01';
export const COMMAND_UUID = '4f1a0002-8b2c-4f5e-9d3a-1c7e6b9f0a01';

export const CMD_ZERO = 0x01; // re-capture the level reference

export function isSupported() {
  return typeof navigator !== 'undefined' && !!navigator.bluetooth;
}

/**
 * Shown when the chooser comes back empty or cancelled. An empty list almost
 * always means something else is holding the connection, since a peripheral
 * stops advertising while connected.
 */
export const notFoundHint =
  'No controller picked. If the list was empty: the board only talks to one '
  + 'thing at a time, so disconnect it from any other Bluetooth page or the OS '
  + 'settings, and check it is still advertising.';

/**
 * Why a connection can't be offered, or null when it can. Web Bluetooth is
 * gated on a secure context, so a plain-http deployment silently has no
 * navigator.bluetooth at all.
 */
export function unsupportedReason() {
  if (typeof navigator === 'undefined') return 'No browser environment.';
  if (!window.isSecureContext) return 'Needs HTTPS (or localhost).';
  if (!navigator.bluetooth) return 'Needs Chrome, Edge or Opera on desktop/Android.';
  return null;
}

export class Controller extends EventTarget {
  constructor() {
    super();
    this.device = null;
    this.server = null;
    this.command = null;
    this.connected = false;
    this.steer = 0;      // -1..1
    this.pitch = 0;      // -1..1
    this.buttons = 0;
    this.zeroed = false;
    this.lastSeq = -1;
    this.dropped = 0;
    this._onDisconnect = this._onDisconnect.bind(this);
    this._onNotify = this._onNotify.bind(this);
  }

  _emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  /** Must be called from a user gesture -- the chooser will not open otherwise. */
  async connect() {
    const reason = unsupportedReason();
    if (reason) throw new Error(reason);

    // Entries in `filters` are OR-ed. Matching on the name as well as the
    // service UUID means the device still shows up if the 128-bit UUID does
    // not make it into the advertisement -- it is 18 of the 31 available
    // bytes, and some stacks quietly drop it. optionalServices is what
    // actually grants access to the service after connecting, so the GATT
    // side works either way.
    this.device = await navigator.bluetooth.requestDevice({
      filters: [
        { services: [SERVICE_UUID] },
        { namePrefix: 'IMU' },
      ],
      optionalServices: [SERVICE_UUID],
    });

    this.device.addEventListener('gattserverdisconnected', this._onDisconnect);

    this.server = await this.device.gatt.connect();
    const service = await this.server.getPrimaryService(SERVICE_UUID);
    const telemetry = await service.getCharacteristic(TELEMETRY_UUID);

    // The command characteristic is optional so an older firmware still pairs.
    try {
      this.command = await service.getCharacteristic(COMMAND_UUID);
    } catch {
      this.command = null;
    }

    telemetry.addEventListener('characteristicvaluechanged', this._onNotify);
    await telemetry.startNotifications();

    this.connected = true;
    this._emit('connected', { name: this.device.name || 'controller' });
    return this.device.name;
  }

  async disconnect() {
    if (this.device?.gatt?.connected) this.device.gatt.disconnect();
    else this._onDisconnect();
  }

  /** Ask the firmware to treat the current attitude as level. */
  async zero() {
    if (!this.command) return false;
    await this.command.writeValue(Uint8Array.of(CMD_ZERO));
    return true;
  }

  _onNotify(event) {
    const v = event.target.value;
    if (v.byteLength < 8) return;

    this.steer = clamp(v.getInt16(0, true) / 1000, -1, 1);
    this.pitch = clamp(v.getInt16(2, true) / 1000, -1, 1);
    this.buttons = v.getUint8(4);
    this.zeroed = (v.getUint8(5) & 0x01) !== 0;

    const seq = v.getUint16(6, true);
    if (this.lastSeq >= 0) {
      const gap = (seq - this.lastSeq) & 0xffff;
      if (gap > 1) this.dropped += gap - 1;
    }
    this.lastSeq = seq;

    this._emit('input', { steer: this.steer, pitch: this.pitch });
  }

  _onDisconnect() {
    this.connected = false;
    this.server = null;
    this.command = null;
    this.steer = 0;
    this.lastSeq = -1;
    this._emit('disconnected', {});
  }
}

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
