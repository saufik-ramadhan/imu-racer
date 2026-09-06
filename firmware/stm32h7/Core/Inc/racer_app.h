#ifndef INC_RACER_APP_H
#define INC_RACER_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * IMU Racer controller application.
 *
 * The STM32H7 port of the ESP32-S3 firmware in imu-racer/firmware: a GY-87 on
 * I2C4 provides the tilt, an SSD1306 on the same bus shows it, and the steering
 * goes to the browser over the ST-LINK virtual COM port instead of BLE.
 */

/** Bring up the bus, the sensors, the display and the USB link. */
void racer_app_init(void);

/**
 * Run one pass of the control loop. Returns immediately when the 20 ms tick
 * is not due yet, so it can be called straight from the main while(1).
 */
void racer_app_run(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_RACER_APP_H */
