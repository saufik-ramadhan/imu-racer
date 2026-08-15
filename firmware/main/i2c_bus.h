#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*                                 Pin mapping                                */
/* -------------------------------------------------------------------------- */
/* Bus 0: GY-87 multi-sensor module (MPU6050 + magnetometer + barometer) */
#define I2C_SENSOR_PORT      0
#define I2C_SENSOR_SDA_PIN   GPIO_NUM_5
#define I2C_SENSOR_SCL_PIN   GPIO_NUM_4
#define I2C_SENSOR_FREQ_HZ   400000

/* Bus 1: SSD1306 OLED display */
#define I2C_OLED_PORT        1
#define I2C_OLED_SDA_PIN     GPIO_NUM_7
#define I2C_OLED_SCL_PIN     GPIO_NUM_6
#define I2C_OLED_FREQ_HZ     400000

/* Default per-transaction timeout, in milliseconds (new driver takes ms, not ticks) */
#define I2C_XFER_TIMEOUT_MS  100

/**
 * @brief Allocate both I2C master buses.
 *
 * @param[out] sensor_bus Handle for the GY-87 bus (may be NULL if not needed)
 * @param[out] oled_bus   Handle for the SSD1306 bus (may be NULL if not needed)
 */
esp_err_t i2c_buses_init(i2c_master_bus_handle_t *sensor_bus,
                         i2c_master_bus_handle_t *oled_bus);

/**
 * @brief Probe every 7-bit address on a bus and log the ones that ACK.
 */
void i2c_bus_scan(i2c_master_bus_handle_t bus, const char *bus_name);

/**
 * @brief Write a single 8-bit register on a device.
 */
esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);

/**
 * @brief Read @p len bytes starting at register @p reg (write-then-read, repeated START).
 */
esp_err_t i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
