# IMU Racer controller — STM32H7 port

The game in this repository and its [ESP32-S3 controller](../esp32s3) are
unchanged.
This project is the same controller on a **NUCLEO-H7A3ZI-Q**: a GY-87 supplies
the tilt, an SSD1306 shows it, and the steering reaches the browser over the
**ST-LINK virtual COM port** — the STM32H7A3 has no radio, so the BLE GATT
service is replaced by a framed byte stream on the USB cable that already
powers and flashes the board.

## Wiring

Everything shares **I2C4**, unlike the ESP32 build which gave the display its
own bus. No address collides.

| Signal | MCU pin |
| --- | --- |
| I2C4_SCL | PF14 |
| I2C4_SDA | PF15 |
| Power | 3V3 |
| Ground | GND |

Both modules go on the same two wires. Find PF14/PF15 on the Zio headers in the
board user manual — the silkscreen only shows the Arduino names, and PF14/PF15
are not among them. The GY-87 and most SSD1306 boards carry their own pull-ups;
with both attached the bus sees a couple of kΩ, which is fine.

The MAX7219 matrix is on **SPI4**, a bus of its own, so it never competes with
the IMU:

| Signal | MCU pin |
| --- | --- |
| SPI4_SCK | PE2 |
| SPI4_MOSI | PE6 |
| CS / LOAD | PE4 |

PE4 is claimed by `max7219.c` itself — the `.ioc` leaves NSS in software mode
and defines no output for it. MISO (PE5) is unused: the MAX7219 cannot be read
back at all.

| Device | Address |
| --- | --- |
| SSD1306 display | 0x3C |
| MPU6050 (accel + gyro) | 0x68 |
| HMC5883L / QMC5883L magnetometer | 0x1E / 0x0D |
| BMP180 barometer | 0x77 |

The magnetometer and barometer only appear once the MPU6050 aux-I2C bypass is
enabled, which `gy87_init()` does.

## Files

| File | Role |
| --- | --- |
| `Core/Src/racer_app.c` | control loop, tilt filter, display, LEDs — the port of `app_main()` |
| `Core/Src/link_serial.c` | USB link: framing, telemetry, commands (replaces `ble_controller.c`) |
| `Core/Src/gy87.c` | MPU6050 + magnetometer + BMP180 on HAL I2C |
| `Core/Src/ssd1306.c` | display driver, incremental flush |
| `Core/Src/max7219.c` | 8x8 LED matrix on SPI4 |
| `Core/Src/i2c_bus.c` | register helpers over `hi2c4` |

`Core/Src/main.c` only gained two lines, both inside `USER CODE` blocks, so
regenerating from `test-imu-racer.ioc` will not lose them.

## Two things worth knowing

**I2C4 must run in Fast Mode.** The `.ioc` asks for it (`Timing = 0x00602173`),
so normally `i2c_bus_init()` does nothing. It exists because 100 kHz is what
CubeMX picks by default, and at that rate a single full display frame takes
~92 ms — four whole control periods. If it ever finds the 100 kHz default
timing it re-opens the bus at ~390 kHz and says so on the console, so losing
Fast Mode in a regeneration slows the display down instead of breaking the
steering.

**`USART3_IRQHandler` lives in `link_serial.c`.** The `.ioc` has no USART3
peripheral — the BSP owns the port — so nothing generates that handler and the
strong definition here wins. If you ever enable USART3 *and* its NVIC entry in
CubeMX, `stm32h7xx_it.c` will define it too and the build will fail with a
duplicate symbol; delete one of the two.

## Timing

| Work | Rate | Cost |
| --- | --- | --- |
| MPU6050 read, tilt filter, telemetry frame | 50 Hz | ~1.5 ms |
| one display page pushed | 50 Hz | ~3 ms |
| whole LED matrix redrawn | 50 Hz | ~16 µs |
| full display refresh | ~6 Hz | spread over 8 ticks |
| barometer | ~16 Hz | sequenced, never blocks |

Two things in the ESP32 firmware would have stalled a 20 ms loop here, and both
are handled rather than accepted: the display is flushed one page per tick
instead of all eight at once, and the BMP180 conversion (25.5 ms at OSS=3) runs
as a state machine across ticks instead of a delay.

## Protocol

```
0xA5 | TYPE | LEN | payload[LEN] | CRC8
```

CRC-8, poly `0x07`, init `0x00`, over `TYPE`, `LEN` and the payload.

| Type | Direction | Payload |
| --- | --- | --- |
| `0x01` telemetry | board → host | 8 bytes, 50 Hz |
| `0x02` info | board → host | device name, sent on hello |
| `0x10` command | host → board | 1 byte, `0x01` = recapture level |
| `0x11` hello | host → board | none, every 1 s — drives “USB: streaming” |

The telemetry payload is byte-for-byte the BLE notification of the ESP32 build
(`int16 steer`, `int16 pitch`, `uint8 buttons`, `uint8 flags`, `uint16 seq`,
little-endian), so the browser parses both boards the same way.

Console logging shares the stream: no byte of 7-bit ASCII can be `0xA5`, so a
receiver treats anything outside a frame as text. A plain serial terminal at
115200 8N1 still shows the startup log and the periodic status line — the
telemetry just looks like occasional binary noise between them.

## Running it

1. Build and flash from STM32CubeIDE. Three LEDs report state: **LD1 green**
   a browser is listening, **LD2 yellow** the level reference is captured,
   **LD3 red** no IMU answered.
2. Serve the web app over `localhost` or HTTPS — Web Serial, like Web
   Bluetooth, needs a secure context:

```bash
cd ../../web && npm install && npm run dev
```

3. Press **CONNECT VIA USB** and pick the ST-LINK port. Close any serial
   terminal first; the port only opens once.
4. Point the steering the right way with the blue user button, then hold the
   board level and hold the button to set the reference.

## The user button

The GY-87 can be glued down facing any of four ways, and which accelerometer
axis means "steer" changes with it. Rather than fixing that in a `#define`, the
button cycles it:

| Press | Effect |
| --- | --- |
| tap (under 800 ms) | rotate the mounting frame 90° — `ROT 0 → 90 → 180 → 270` on line 4 |
| hold (800 ms or more) | recapture level, same as the page's zero command |

Rotating discards the level reference — it was measured in the old frame — and
takes a new one from the same sample, so the car does not pull to one side
while you re-level by hand. The tilt filter is snapped rather than eased onto
the new axes for the same reason.

Tap until tilting left and right moves `STEER` (not the `P` figure on line 7),
then hold to zero. The setting lives in RAM only, so it starts at 0° after
every reset; making it survive would mean an RTC backup register or a flash
sector, neither of which this project enables.

## The LED matrix

A spirit level: one lit pixel showing where the board sits relative to its
captured level reference, ±20° across the panel — 5° per pixel, tight enough to
level by. The OLED bar is one-dimensional and has to be read; this is
two-dimensional and can be seen out of the corner of your eye while you are
looking at the game.

| What you see | Meaning |
| --- | --- |
| single pixel | how far, and which way, the board is off level |
| 2x2 block in the middle | within 2° of level — the moment to hold the button |
| blinking border, no dot | IMU absent or failing; the angles are stale |
| dim vs bright | no host listening vs streaming to a browser |

Brightness carries the link state so that no pixel is spent on it and the dot
stays unambiguous at full deflection.

The angles come from the same filter the steering uses, so the frame the user
button rotates applies here too. That is the quickest way to check `ROT`: tilt
left, and the dot must go left. If it goes up instead, tap the button again.

Raw angles feed the bubble, not the steering values — those carry the 0.06
deadzone, which would blank the middle of the grid, exactly the part you need
while levelling.

Panel orientation is handled entirely in [max7219.h](Core/Inc/max7219.h), never
in the drawing code, which stays in plain top-left-origin coordinates.
`MAX7219_TRANSPOSE`, `MAX7219_FLIP_X` and `MAX7219_FLIP_Y` are applied in that
order; transpose plus one flip is a 90° rotation, and swapping which flip is
set turns the image the other way. It currently ships rotated 90°
(`TRANSPOSE 1`, `FLIP_X 1`).

The OLED is flipped the same way, through `SSD1306_FLIP_180` in
[ssd1306.h](Core/Inc/ssd1306.h). That one is done by the panel itself — segment
remap plus COM scan direction — so the frame buffer and every text coordinate
are untouched by it.

**Two electrical notes.** The MAX7219 wants 4–5.5 V, and its logic-high
threshold is 0.7 × VCC — 3.5 V at a 5 V supply, while the STM32 only swings
3.3 V. It often works anyway, but not reliably. Running the panel at ~4.3 V
(one diode in series from 5 V) drops the threshold to about 3.0 V and settles
it. Also, `max7219_init()` reconfigures SPI4 to 8-bit words at 8 MHz: CubeMX
generates 4-bit at 32 Mbit/s, and this chip needs 16-bit frames and tops out at
10 MHz. SPI4 carries nothing else, so the driver sets what the panel needs
rather than making the `.ioc` the thing you have to get right.

The web app gained [`web/src/serial.js`](../../web/src/serial.js) and a second
connect button. The Bluetooth path is untouched, so the same page still drives
the ESP32 board.
