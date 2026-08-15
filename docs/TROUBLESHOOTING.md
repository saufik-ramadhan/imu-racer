# Troubleshooting

## The controller connects, then immediately disconnects

You are almost certainly pairing from the OS Bluetooth panel. Don't.

Web Bluetooth does not use OS pairing at all — the browser opens its own GATT
connection. Windows "Add device" connects, looks for a profile it recognises,
finds only a custom service that means nothing to it, and hangs up.

Leave the Bluetooth adapter **on**, but do not pair the device. Connect from the
page's connect button instead. If the controller is already listed under
Settings → Bluetooth & devices, remove it — Windows silently auto-reconnects to
remembered devices and will keep stealing the link.

## The browser's device chooser is empty

A BLE peripheral serves one central at a time and **stops advertising while
connected**. The chooser only lists devices it can currently see advertising, so
anything holding the link makes it appear empty.

Check, in order:

1. Is the OS connected to it? Remove the pairing.
2. Is another browser tab connected? Close it.
3. Is `chrome://bluetooth-internals` (or `brave://…`) connected? That page has a
   **Disconnect** button — a very easy one to leave holding the link.
4. Is the board advertising at all? The OLED should read `BLE: advertising`, and
   the log should show `advertising as "IMU Racer"`.

`chrome://bluetooth-internals` → Devices → Start Scan tells you whether the
board is visible to the browser at all, independently of the game page.

## It shows up in bluetooth-internals but not in the chooser

That means the advertisement does not carry what your filter matches on. The
client filters on the service UUID **or** a name prefix, so at least one has to
be in the advertisement or scan response.

The firmware logs the exact bytes it advertises at startup:

```
I (1234) ble_ctrl: adv payload 21/31 bytes: 02 01 06 11 07 01 0a 9f ...
```

Reading it: `02 01 06` is the flags structure, then `11 07` opens a 17-byte
"complete list of 128-bit service UUIDs" followed by the UUID in reverse byte
order. If that second structure is missing, the service filter cannot match and
the name filter is what saves you.

## It goes quiet after a disconnect and only a reset brings it back

Fixed, but worth understanding. `ble_gap_adv_start()` frequently returns
`BLE_HS_EBUSY` when called straight from the disconnect event, because the
controller is still tearing the link down. The original code logged that and
gave up, so nothing ever restarted advertising.

`ble_controller_maintain()` now runs on the slow tick and restarts advertising
whenever nothing is connected and nothing is advertising, so a one-off failure
heals within about a quarter of a second.

## Disconnect reason codes

NimBLE reports HCI reasons as `0x200 | code`. The firmware decodes the common
ones:

| Reason | Meaning |
| --- | --- |
| `0x208` | supervision timeout — out of range, or the central stopped responding |
| `0x213` | the central terminated the connection |
| `0x216` | terminated locally |
| `0x205` | authentication failure — the central expected pairing |
| `0x206` | PIN or key missing — a stale pairing is cached on the central |
| `0x23e` | failed to establish the connection |

## No I²C devices found

The boot scan prints every address that answers on both buses:

```
I (330) i2c_bus: Scanning sensor bus ...
I (340) i2c_bus:   found device at 0x68
```

An empty bus is wiring, power or pull-ups. Internal pull-ups are enabled but
weak; add 4.7 kΩ to 3V3 on SDA and SCL. Check the module is on 3.3 V, and that
SDA and SCL are not swapped.

## The magnetometer or barometer is missing

If `0x68` answers but `0x1E`, `0x0D` and `0x77` do not, the MPU6050's bypass is
not open. The firmware clears `USER_CTRL` and sets `I2C_BYPASS_EN`, in that
order, which is what makes the auxiliary devices visible.

If the barometer answers but reports chip ID `0x58`, it is a BMP280 rather than
a BMP180 — a different register map entirely. The driver detects this and warns
instead of producing nonsense.

Plenty of boards sold as GY-87 simply do not populate the magnetometer. Steering
only uses the accelerometer, so this does not affect the game.

## The connect button is disabled

The page tells you why in the status line under it:

- *Needs HTTPS (or localhost)* — Web Bluetooth only exists in a secure context
- *Needs Chrome, Edge or Opera* — Firefox and every iOS browser lack the API

In an iframe, the parent page must pass `allow="bluetooth"`.

## The game renders but the canvas is the wrong size

Should not happen — the canvas is driven by a `ResizeObserver` on the play area,
which fires on initial observation as well as on changes. If you see a 300×150
canvas, the observer is not being delivered, which happens in a page that is
never composited (a hidden tab, or an automated browser without a display).
