#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE game-controller peripheral.
 *
 * Advertises a custom 128-bit service carrying the board's tilt, which the
 * IMU Racer web page subscribes to through the Web Bluetooth API. The UUIDs
 * and the packet layout must stay in sync with imu-racer/src/ble.js.
 *
 *   service   4f1a0000-8b2c-4f5e-9d3a-1c7e6b9f0a01
 *   telemetry 4f1a0001-...  notify, 8 bytes
 *   command   4f1a0002-...  write, 1 byte
 *
 * Telemetry, little-endian:
 *   int16  steer    -1000..1000
 *   int16  pitch    -1000..1000
 *   uint8  buttons
 *   uint8  flags    bit0 = zero reference captured
 *   uint16 seq
 */

#define BLE_CMD_ZERO 0x01

esp_err_t ble_controller_init(const char *device_name);

/**
 * Call periodically (a few times a second is plenty) from the application
 * task. Restarts advertising if it is not running and nobody is connected.
 *
 * ble_gap_adv_start() often fails when called straight from the disconnect
 * event, because the controller is still tearing the link down. Without a
 * retry the board goes quiet until it is reset.
 */
void ble_controller_maintain(void);

/** True once a central is connected. */
bool ble_controller_is_connected(void);

/** True once that central has actually subscribed to notifications. */
bool ble_controller_is_streaming(void);

/** Push one telemetry frame. Cheap no-op when nobody is subscribed. */
void ble_controller_publish(float steer, float pitch, uint8_t buttons, bool zeroed);

/**
 * Returns true once after the central asks for the level reference to be
 * recaptured, clearing the request.
 */
bool ble_controller_take_zero_request(void);

#ifdef __cplusplus
}
#endif
