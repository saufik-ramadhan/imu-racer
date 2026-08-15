#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit addresses present on a GY-87 once MPU6050 bypass mode is enabled */
#define MPU6050_ADDR    0x68  /* accel + gyro (0x69 if AD0 is pulled high) */
#define HMC5883L_ADDR   0x1E  /* magnetometer, original Honeywell part      */
#define QMC5883L_ADDR   0x0D  /* magnetometer, clone part on newer boards   */
#define BMP180_ADDR     0x77  /* barometer                                  */

typedef enum {
    MAG_NONE = 0,
    MAG_HMC5883L,
    MAG_QMC5883L,
} gy87_mag_type_t;

typedef struct {
    float ax, ay, az;     /* g       */
    float gx, gy, gz;     /* deg/s   */
    float mx, my, mz;     /* microtesla (approx, uncalibrated) */
    float temp_mpu;       /* degC, from the MPU6050 die sensor  */
    float temp_baro;      /* degC, from the BMP180              */
    float pressure;       /* Pa                                 */
    float altitude;       /* m above sea level, ISA model       */
} gy87_data_t;

typedef struct {
    i2c_master_dev_handle_t mpu;
    i2c_master_dev_handle_t mag;
    i2c_master_dev_handle_t baro;
    gy87_mag_type_t mag_type;
    bool baro_present;
    /* BMP180 factory calibration */
    int16_t  ac1, ac2, ac3, b1, b2, mb, mc, md;
    uint16_t ac4, ac5, ac6;
} gy87_t;

/**
 * @brief Bring up every chip on the GY-87 module.
 *
 * Wakes the MPU6050, turns off its aux-I2C master and enables bypass so the
 * magnetometer and barometer appear directly on the main bus, then probes and
 * configures whichever mag/baro parts this board variant carries.
 */
esp_err_t gy87_init(i2c_master_bus_handle_t bus, gy87_t *dev);

/**
 * @brief Read every sensor into @p out. Fields for absent chips are left at 0.
 *
 * Takes ~35 ms because the BMP180 blocks while it converts, so it is not
 * suitable for a fast control loop -- use gy87_read_motion() for that.
 */
esp_err_t gy87_read(gy87_t *dev, gy87_data_t *out);

/**
 * @brief Read only the MPU6050 (accel, gyro, die temperature).
 *
 * One ~1 ms I2C burst, safe to call at hundreds of hertz. Leaves the
 * magnetometer and barometer fields of @p out untouched.
 */
esp_err_t gy87_read_motion(gy87_t *dev, gy87_data_t *out);

#ifdef __cplusplus
}
#endif
