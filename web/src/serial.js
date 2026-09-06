/**
 * Web Serial client for the STM32H7 + GY-87 controller.
 *
 * The STM32H7A3 has no radio, so it streams over the ST-LINK virtual COM port
 * -- the same USB cable that powers and flashes the board. The framing here
 * must stay in sync with Core/Src/link_serial.c in the STM32 project.
 *
 *   0xA5 | TYPE | LEN | payload[LEN] | CRC8      (poly 0x07, init 0x00,
 *                                                 over TYPE, LEN, payload)
 *
 * Anything outside a frame is console text from the firmware: no byte of
 * 7-bit ASCII can be 0xA5, so logs and telemetry share one stream safely.
 *
 * The telemetry payload is byte-for-byte the BLE notification in ble.js:
 *   int16  steer    -1000..1000
 *   int16  pitch    -1000..1000
 *   uint8  buttons  bit0 = user button
 *   uint8  flags    bit0 = zero offset captured
 *   uint16 seq      wraps, used to spot dropped frames
 */
const SOF = 0xa5;

const MSG_TELEMETRY = 0x01;
const MSG_INFO = 0x02;
const MSG_CMD = 0x10;
const MSG_HELLO = 0x11;

export const CMD_ZERO = 0x01; // re-capture the level reference

const BAUD_RATE = 115200;
const HELLO_INTERVAL_MS = 1000;

export function isSupported() {
  return typeof navigator !== 'undefined' && !!navigator.serial;
}

/**
 * Why a connection can't be offered, or null when it can. Web Serial is gated
 * on a secure context, exactly like Web Bluetooth.
 */
export function unsupportedReason() {
  if (typeof navigator === 'undefined') return 'No browser environment.';
  if (!window.isSecureContext) return 'Needs HTTPS (or localhost).';
  if (!navigator.serial) return 'Needs Chrome, Edge or Opera on desktop.';
  return null;
}

/** Shown when the port chooser comes back empty or cancelled. */
export const notFoundHint =
  'No port picked. The board shows up as a USB serial device once the '
  + 'ST-LINK driver has it -- close any terminal that already holds the port, '
  + 'then try again.';

function crc8(bytes) {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b;
    for (let i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
    }
  }
  return crc;
}

function frame(type, payload = []) {
  const body = Uint8Array.from([type, payload.length, ...payload]);
  return Uint8Array.from([SOF, ...body, crc8(body)]);
}

export class Controller extends EventTarget {
  constructor() {
    super();
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.connected = false;
    this.name = 'IMU Racer (USB)';
    this.steer = 0;      // -1..1
    this.pitch = 0;      // -1..1
    this.buttons = 0;
    this.zeroed = false;
    this.lastSeq = -1;
    this.dropped = 0;
    this._hello = null;
    this._text = '';
    this._buf = new Uint8Array(0);
  }

  _emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  /** Must be called from a user gesture -- the port chooser will not open otherwise. */
  async connect() {
    const reason = unsupportedReason();
    if (reason) throw new Error(reason);

    // No vendor filter: the chooser should still list the board when it is
    // behind a plain USB-serial adapter rather than the on-board ST-LINK
    // (which is 0x0483). An empty list is worse than a long one here.
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: BAUD_RATE, bufferSize: 4096 });

    this.writer = this.port.writable.getWriter();
    this.connected = true;
    this.lastSeq = -1;

    this._readLoop();                       // runs until the port closes
    await this._send(frame(MSG_HELLO));     // asks the board to identify itself

    // The firmware treats hello as a keepalive: it drives the "USB: streaming"
    // line on the OLED, so the board can tell a live tab from a stale port.
    this._hello = setInterval(() => {
      this._send(frame(MSG_HELLO)).catch(() => {});
    }, HELLO_INTERVAL_MS);

    this._emit('connected', { name: this.name });
    return this.name;
  }

  async disconnect() {
    if (!this.port) {
      this._onDisconnect();
      return;
    }
    try {
      await this.reader?.cancel();
    } catch {
      /* already gone */
    }
  }

  /** Ask the firmware to treat the current attitude as level. */
  async zero() {
    if (!this.connected) return false;
    await this._send(frame(MSG_CMD, [CMD_ZERO]));
    return true;
  }

  async _send(bytes) {
    if (!this.writer) return;
    await this.writer.write(bytes);
  }

  async _readLoop() {
    try {
      while (this.port?.readable) {
        this.reader = this.port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) break;
            this._feed(value);
          }
        } finally {
          this.reader.releaseLock();
        }
      }
    } catch {
      /* unplugged mid-read: fall through to the cleanup below */
    }
    await this._close();
  }

  async _close() {
    clearInterval(this._hello);
    this._hello = null;
    try {
      this.writer?.releaseLock();
      await this.port?.close();
    } catch {
      /* already closed */
    }
    this._onDisconnect();
  }

  /**
   * Byte-stream parser. Bytes outside a frame are firmware log lines, which
   * are worth keeping visible -- they are the only diagnostics the board has.
   */
  _feed(chunk) {
    const buf = new Uint8Array(this._buf.length + chunk.length);
    buf.set(this._buf);
    buf.set(chunk, this._buf.length);

    let i = 0;
    while (i < buf.length) {
      if (buf[i] !== SOF) {
        this._logByte(buf[i]);
        i++;
        continue;
      }
      if (buf.length - i < 4) break;              // header + crc not here yet
      const type = buf[i + 1];
      const len = buf[i + 2];
      if (buf.length - i < 4 + len) break;        // payload still arriving

      const body = buf.subarray(i + 1, i + 3 + len);
      if (crc8(body) === buf[i + 3 + len]) {
        this._onFrame(type, buf.subarray(i + 3, i + 3 + len));
        i += 4 + len;
      } else {
        i++;                                      // false start, resynchronise
      }
    }
    this._buf = buf.slice(i);
  }

  _logByte(byte) {
    if (byte === 0x0a) {
      const line = this._text.trim();
      if (line) console.debug('[board]', line);
      this._text = '';
    } else if (byte !== 0x0d) {
      this._text += String.fromCharCode(byte);
      if (this._text.length > 200) this._text = '';
    }
  }

  _onFrame(type, payload) {
    if (type === MSG_INFO) {
      this.name = new TextDecoder().decode(payload) || this.name;
      return;
    }
    if (type !== MSG_TELEMETRY || payload.length < 8) return;

    const v = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
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
    if (!this.connected && !this.port) return;
    this.connected = false;
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.steer = 0;
    this.lastSeq = -1;
    this._buf = new Uint8Array(0);
    this._emit('disconnected', {});
  }
}

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
