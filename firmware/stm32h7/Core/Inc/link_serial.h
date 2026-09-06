#ifndef INC_LINK_SERIAL_H
#define INC_LINK_SERIAL_H

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB link to the IMU Racer web page.
 *
 * The STM32H7A3 has no radio, so the BLE GATT service of the ESP32 build is
 * replaced by a framed byte stream over the ST-LINK virtual COM port -- the
 * same USB cable that powers and flashes the board (USART3, PD8/PD9, 115200
 * 8N1). The browser reads it with the Web Serial API instead of Web Bluetooth.
 *
 * Frame layout:
 *
 *   0xA5 | TYPE | LEN | payload[LEN] | CRC8
 *
 * CRC8 is the classic poly 0x07, init 0x00, taken over TYPE, LEN and payload.
 * Plain ASCII log lines share the same stream: no byte of 7-bit text can be
 * 0xA5, so a receiver simply treats anything outside a frame as console
 * output. That keeps a dumb serial terminal useful for debugging.
 *
 * Telemetry payload is byte-for-byte the BLE notification of the ESP32 build,
 * so the browser-side parser did not have to change:
 *
 *   int16  steer    -1000..1000
 *   int16  pitch    -1000..1000
 *   uint8  buttons  bit0 = user button
 *   uint8  flags    bit0 = zero reference captured
 *   uint16 seq      wraps, used to spot dropped frames
 */

#define LINK_SOF            0xA5

/* device -> host */
#define LINK_MSG_TELEMETRY  0x01
#define LINK_MSG_INFO       0x02

/* host -> device */
#define LINK_MSG_CMD        0x10
#define LINK_MSG_HELLO      0x11

#define LINK_CMD_ZERO       0x01

/** Take over USART3 receive and announce ourselves. */
void link_serial_init(const char *device_name);

/** Push one telemetry frame. ~1 ms at 115200. */
void link_serial_publish(float steer, float pitch, uint8_t buttons, bool zeroed);

/** Drain the receive ring and act on whatever the host sent. */
void link_serial_poll(void);

/** Re-send the identification frame (the host asks for it by saying hello). */
void link_serial_send_info(void);

/**
 * Returns true once after the host asks for the level reference to be
 * recaptured, clearing the request. Mirrors ble_controller_take_zero_request().
 */
bool link_serial_take_zero_request(void);

/** True while the host has said hello within the last couple of seconds. */
bool link_serial_host_present(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_LINK_SERIAL_H */
