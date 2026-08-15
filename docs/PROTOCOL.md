# BLE Protocol

The contract between the firmware and the browser. It is defined in exactly two
places, and they must be changed together:

- `blink/main/ble_controller.c` — the peripheral
- `imu-racer/src/ble.js` — the Web Bluetooth client

## GATT layout

| Role | UUID | Properties |
| --- | --- | --- |
| Service | `4f1a0000-8b2c-4f5e-9d3a-1c7e6b9f0a01` | primary |
| Telemetry | `4f1a0001-8b2c-4f5e-9d3a-1c7e6b9f0a01` | read, notify |
| Command | `4f1a0002-8b2c-4f5e-9d3a-1c7e6b9f0a01` | write, write-without-response |

NimBLE stores 128-bit UUIDs least-significant byte first, so the arrays in the
firmware are the written form reversed:

```c
/* 4f1a0000-8b2c-4f5e-9d3a-1c7e6b9f0a01 */
BLE_UUID128_INIT(0x01, 0x0a, 0x9f, 0x6b, 0x7e, 0x1c, 0x3a, 0x9d,
                 0x5e, 0x4f, 0x2c, 0x8b, 0x00, 0x00, 0x1a, 0x4f)
```

Getting this backwards is a silent failure — the peripheral advertises happily
and the browser simply never matches it. A quick way to check is to reverse the
array and format it as a UUID string; it must equal the value in `ble.js`.

## Telemetry packet

8 bytes, little-endian, notified at 50 Hz while a central is subscribed.

| Offset | Type | Field | Range | Meaning |
| --- | --- | --- | --- | --- |
| 0 | `int16` | `steer` | -1000..1000 | roll angle, normalised to full lock |
| 2 | `int16` | `pitch` | -1000..1000 | pitch angle, reserved for throttle |
| 4 | `uint8` | `buttons` | bitfield | bit0 = user button (unused) |
| 5 | `uint8` | `flags` | bitfield | bit0 = zero reference captured |
| 6 | `uint16` | `seq` | wraps | increments per packet |

`seq` exists so the client can detect dropped notifications:

```js
const gap = (seq - this.lastSeq) & 0xffff;
if (gap > 1) this.dropped += gap - 1;
```

## Commands

Single byte written to the command characteristic.

| Value | Name | Effect |
| --- | --- | --- |
| `0x01` | `CMD_ZERO` | Recapture the current attitude as level |

The firmware exposes this as a latch: `ble_controller_take_zero_request()`
returns `true` once and clears the flag, so the control loop polls it rather
than running work inside the BLE callback.

## Advertising

The 128-bit service UUID takes 18 of the 31 bytes available in a legacy
advertising payload. With the 3-byte flags structure that leaves 10, which is
not enough for the name. So:

- **Advertisement** — flags + complete list of 128-bit service UUIDs
- **Scan response** — complete local name + TX power

Both are visible to a browser doing an active scan, which Chrome does.

The web client filters on *either*, because the `filters` array is OR-ed:

```js
filters: [
  { services: [SERVICE_UUID] },
  { namePrefix: 'IMU' },
],
optionalServices: [SERVICE_UUID],
```

`optionalServices` is what actually grants access to the service after
connecting, so GATT works regardless of which filter matched.

## Connection parameters

After a central subscribes, the peripheral requests a 15–30 ms interval with a
4 s supervision timeout. Centrals are free to refuse; the firmware logs it and
carries on notifying at whatever rate it was granted.

The request deliberately happens on the *subscribe* event, not on connect.
Asking for new parameters the instant a link comes up upsets some centrals, and
before anything has subscribed there is nothing to be responsive about.
