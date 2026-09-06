#ifndef INC_GY87_H
#define INC_GY87_H

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

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

/*
 * The barometer is read across several control ticks instead of blocking.
 * A BMP180 pressure conversion at OSS=3 takes 25.5 ms, which is longer than
 * the whole 20 ms control period -- waiting for it inline would drop steering
 * frames four times a second.
 */
typedef enum {
  BARO_IDLE = 0,
  BARO_WAIT_TEMP,
  BARO_WAIT_PRESSURE,
} gy87_baro_state_t;

typedef struct {
  float ax, ay, az;     /* g       */
  float gx, gy, gz;     /* deg/s   */
  float mx, my, mz;     /* microtesla (approx, uncalibrated) */
  float temp_mpu;       /* degC, from the MPU6050 die sensor  */
  float temp_baro;      /* degC, from the BMP180              */
  float pressure;       /* Pa                                 */
} gy87_data_t;

typedef struct {
  gy87_mag_type_t mag_type;
  bool     mpu_present;
  bool     baro_present;
  /* BMP180 factory calibration */
  int16_t  ac1, ac2, ac3, b1, b2, mb, mc, md;
  uint16_t ac4, ac5, ac6;
  /* barometer sequencer */
  gy87_baro_state_t baro_state;
  uint32_t baro_deadline;
  int32_t  baro_ut;
} gy87_t;

/**
 * @brief Bring up every chip on the GY-87 module.
 *
 * Wakes the MPU6050, turns off its aux-I2C master and enables bypass so the
 * magnetometer and barometer appear directly on I2C4, then probes and
 * configures whichever mag/baro parts this board variant carries.
 *
 * Returns HAL_OK when at least the MPU6050 came up; the mag and baro are
 * optional and their absence only leaves the matching fields at zero.
 */
HAL_StatusTypeDef gy87_init(gy87_t *dev);

/**
 * @brief Read only the MPU6050 (accel, gyro, die temperature).
 *
 * One ~0.5 ms I2C burst at 400 kHz, safe to call every control tick.
 */
HAL_StatusTypeDef gy87_read_motion(gy87_t *dev, gy87_data_t *out);

/** @brief Read the magnetometer, if one was detected. */
HAL_StatusTypeDef gy87_read_mag(gy87_t *dev, gy87_data_t *out);

/**
 * @brief Advance the barometer sequencer. Call once per control tick.
 *
 * Never blocks: it starts a conversion, comes back later to collect it, and
 * returns true on the tick where @p out gained a fresh pressure/temperature.
 */
bool gy87_baro_step(gy87_t *dev, gy87_data_t *out);

#ifdef __cplusplus
}
#endif

#endif /* INC_GY87_H */
