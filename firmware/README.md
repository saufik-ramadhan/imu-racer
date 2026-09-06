# Firmware

Two boards, one game. Both read a GY-87, drive an SSD1306, and send the same
8-byte telemetry packet; they differ only in how that packet reaches the
browser.

| Target | Transport | Browser API |
| --- | --- | --- |
| [`esp32s3/`](esp32s3) | BLE GATT notifications | Web Bluetooth |
| [`stm32h7/`](stm32h7) | framed stream on the ST-LINK virtual COM port | Web Serial |

The STM32H7A3 has no radio, which is the whole reason for the second transport.
Keeping the telemetry payload byte-for-byte identical means the web client
parses both the same way — only the plumbing around it differs, and the page
offers a connect button for each.

The protocol is documented in [`docs/PROTOCOL.md`](../docs/PROTOCOL.md), and
each target has its own README covering wiring, build and board-specific
detail.
