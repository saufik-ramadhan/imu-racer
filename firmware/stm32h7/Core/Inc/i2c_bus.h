#ifndef INC_I2C_BUS_H
#define INC_I2C_BUS_H

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thin register-level helpers over the HAL, so the GY-87 and SSD1306 drivers
 * read the same way they do in the ESP32 firmware.
 *
 * Unlike the ESP32 build -- which gave each chip its own bus -- everything here
 * shares I2C4 (PF14 = SCL, PF15 = SDA). No address collides: 0x3C display,
 * 0x68 IMU, 0x1E/0x0D magnetometer, 0x77 barometer.
 */

/*
 * The .ioc asks for Fast Mode, so MX_I2C4_Init() already brings the bus up at
 * 400 kHz and i2c_bus_init() has nothing to do. The fallback below exists
 * because 100 kHz is what CubeMX picks by default, and at that rate a single
 * full display frame takes ~92 ms -- four whole control periods. Losing Fast
 * Mode in a regeneration should slow the display down, not break the steering.
 *
 * 0x00923266 is PRESC=0, SCLDEL=9, SDADEL=2, SCLH=0x32, SCLL=0x66 against the
 * 64 MHz kernel clock, i.e. ~390 kHz.
 */
#define I2C_BUS_TIMING_100KHZ   0x10707DBCU   /* the CubeMX default */
#define I2C_BUS_TIMING_400KHZ   0x00923266U

#define I2C_XFER_TIMEOUT_MS     50U    /* register-sized transfers */
#define I2C_FRAME_TIMEOUT_MS    200U   /* whole-framebuffer transfers */

/** Check the generated bus speed, forcing fast mode if it came out at 100 kHz. */
HAL_StatusTypeDef i2c_bus_init(void);

/** True when a device ACKs its address. */
bool i2c_bus_probe(uint8_t addr7);

/** Probe every 7-bit address and print the ones that answer. */
void i2c_bus_scan(void);

/** Write one 8-bit register. */
HAL_StatusTypeDef i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t val);

/** Read @p len bytes from consecutive registers (write-then-read, repeated START). */
HAL_StatusTypeDef i2c_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len);

/** Raw write, for devices like the SSD1306 that take a control byte, not a register. */
HAL_StatusTypeDef i2c_write_raw(uint8_t addr7, const uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/**
 * HAL_I2C_ERROR_* from the last failed transfer, and a short name for it.
 * AF means the device did not acknowledge its address (absent, unpowered, or
 * the wrong address); BERR/ARLO point at the wiring or the pull-ups instead.
 */
uint32_t    i2c_bus_last_error(void);
const char *i2c_bus_error_name(uint32_t err);

#ifdef __cplusplus
}
#endif

#endif /* INC_I2C_BUS_H */
