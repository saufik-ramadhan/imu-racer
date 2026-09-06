#include "gy87.h"
#include "i2c_bus.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------- MPU6050 ---------------------------------- */
#define MPU_SMPLRT_DIV      0x19
#define MPU_CONFIG          0x1A
#define MPU_GYRO_CONFIG     0x1B
#define MPU_ACCEL_CONFIG    0x1C
#define MPU_INT_PIN_CFG     0x37
#define MPU_ACCEL_XOUT_H    0x3B
#define MPU_USER_CTRL       0x6A
#define MPU_PWR_MGMT_1      0x6B
#define MPU_WHO_AM_I        0x75

/* +-2 g and +-250 deg/s full-scale => these sensitivities (datasheet 6.2/6.3) */
#define ACCEL_LSB_PER_G     16384.0f
#define GYRO_LSB_PER_DPS    131.0f

/* ------------------------------ HMC5883L ---------------------------------- */
#define HMC_CONFIG_A        0x00
#define HMC_CONFIG_B        0x01
#define HMC_MODE            0x02
#define HMC_DATA_X_MSB      0x03
#define HMC_ID_A            0x0A
/* Gain 0x20 => 1090 LSB/Gauss; 1 Gauss = 100 uT */
#define HMC_LSB_PER_GAUSS   1090.0f

/* ------------------------------ QMC5883L ---------------------------------- */
#define QMC_DATA_X_LSB      0x00
#define QMC_CONTROL_1       0x09
#define QMC_SET_RESET       0x0B
/* Range 8 Gauss => 3000 LSB/Gauss */
#define QMC_LSB_PER_GAUSS   3000.0f

/* ------------------------------- BMP180 ----------------------------------- */
#define BMP_CAL_START       0xAA
#define BMP_CHIP_ID         0xD0
#define BMP_CTRL_MEAS       0xF4
#define BMP_DATA_MSB        0xF6
#define BMP_CMD_READ_TEMP   0x2E
#define BMP_CMD_READ_PRESS  0x34
#define BMP_OSS             3       /* ultra high resolution, 25.5 ms conversion */

#define BMP_TEMP_WAIT_MS    5U
#define BMP_PRESS_WAIT_MS   28U

/* -------------------------------------------------------------------------- */
/*                                    Init                                    */
/* -------------------------------------------------------------------------- */

static HAL_StatusTypeDef mpu6050_init(gy87_t *dev)
{
  uint8_t who = 0;

  if (i2c_read_regs(MPU6050_ADDR, MPU_WHO_AM_I, &who, 1) != HAL_OK)
  {
    printf("gy87: MPU6050 not responding at 0x%02X\r\n", MPU6050_ADDR);
    return HAL_ERROR;
  }
  /* Genuine parts return 0x68; several clones report 0x70/0x72 but work fine. */
  printf("gy87: MPU6050 WHO_AM_I = 0x%02X\r\n", who);

  /* Device reset, then wait for the internal registers to settle. */
  if (i2c_write_reg(MPU6050_ADDR, MPU_PWR_MGMT_1, 0x80) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(100);

  /* Wake up and clock off the gyro X PLL -- more stable than the internal RC. */
  if (i2c_write_reg(MPU6050_ADDR, MPU_PWR_MGMT_1, 0x01) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(10);

  if (i2c_write_reg(MPU6050_ADDR, MPU_CONFIG, 0x03) != HAL_OK ||        /* DLPF 44 Hz     */
      i2c_write_reg(MPU6050_ADDR, MPU_SMPLRT_DIV, 0x04) != HAL_OK ||    /* 1 kHz/5 = 200 Hz */
      i2c_write_reg(MPU6050_ADDR, MPU_GYRO_CONFIG, 0x00) != HAL_OK ||
      i2c_write_reg(MPU6050_ADDR, MPU_ACCEL_CONFIG, 0x00) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* The mag and baro sit on the MPU auxiliary bus. Disabling the aux-I2C
   * master and setting I2C_BYPASS_EN wires them straight through to I2C4,
   * which is what makes 0x1E/0x0D/0x77 visible to us at all. */
  if (i2c_write_reg(MPU6050_ADDR, MPU_USER_CTRL, 0x00) != HAL_OK ||
      i2c_write_reg(MPU6050_ADDR, MPU_INT_PIN_CFG, 0x02) != HAL_OK)
  {
    return HAL_ERROR;
  }
  HAL_Delay(10);

  dev->mpu_present = true;
  printf("gy87: MPU6050 ready, aux-I2C bypass enabled\r\n");
  return HAL_OK;
}

static void mag_init(gy87_t *dev)
{
  dev->mag_type = MAG_NONE;

  if (i2c_bus_probe(HMC5883L_ADDR))
  {
    uint8_t id[3] = { 0 };

    i2c_read_regs(HMC5883L_ADDR, HMC_ID_A, id, 3);
    if (id[0] == 'H' && id[1] == '4' && id[2] == '3')
    {
      printf("gy87: HMC5883L detected (ID H43)\r\n");
    }
    else
    {
      printf("gy87: device at 0x1E but ID is %02X %02X %02X -- treating as HMC5883L\r\n",
             id[0], id[1], id[2]);
    }
    i2c_write_reg(HMC5883L_ADDR, HMC_CONFIG_A, 0x70);  /* 8-sample average, 15 Hz  */
    i2c_write_reg(HMC5883L_ADDR, HMC_CONFIG_B, 0x20);  /* gain 1090 LSB/Gauss      */
    i2c_write_reg(HMC5883L_ADDR, HMC_MODE, 0x00);      /* continuous measurement   */
    dev->mag_type = MAG_HMC5883L;
    HAL_Delay(10);
    return;
  }

  if (i2c_bus_probe(QMC5883L_ADDR))
  {
    i2c_write_reg(QMC5883L_ADDR, QMC_SET_RESET, 0x01);  /* recommended set/reset period     */
    i2c_write_reg(QMC5883L_ADDR, QMC_CONTROL_1, 0x1D);  /* OSR512, 8 Ga, 200 Hz, continuous */
    dev->mag_type = MAG_QMC5883L;
    printf("gy87: QMC5883L detected at 0x0D\r\n");
    HAL_Delay(10);
    return;
  }

  printf("gy87: no magnetometer -- bypass may have failed, or this board has none\r\n");
}

static void bmp180_init(gy87_t *dev)
{
  dev->baro_present = false;

  if (!i2c_bus_probe(BMP180_ADDR))
  {
    printf("gy87: no barometer at 0x%02X\r\n", BMP180_ADDR);
    return;
  }

  uint8_t chip_id = 0;
  if (i2c_read_regs(BMP180_ADDR, BMP_CHIP_ID, &chip_id, 1) != HAL_OK)
  {
    return;
  }
  if (chip_id != 0x55)
  {
    /* 0x58 is a BMP280, which speaks a completely different register map. */
    printf("gy87: chip at 0x77 reports ID 0x%02X, not BMP180 (0x55)%s\r\n",
           chip_id, chip_id == 0x58 ? " -- looks like a BMP280" : "");
    return;
  }

  /* 11 big-endian 16-bit calibration words in EEPROM at 0xAA..0xBF */
  uint8_t cal[22];
  if (i2c_read_regs(BMP180_ADDR, BMP_CAL_START, cal, sizeof(cal)) != HAL_OK)
  {
    printf("gy87: BMP180 calibration read failed\r\n");
    return;
  }

  dev->ac1 = (int16_t)((cal[0]  << 8) | cal[1]);
  dev->ac2 = (int16_t)((cal[2]  << 8) | cal[3]);
  dev->ac3 = (int16_t)((cal[4]  << 8) | cal[5]);
  dev->ac4 = (uint16_t)((cal[6]  << 8) | cal[7]);
  dev->ac5 = (uint16_t)((cal[8]  << 8) | cal[9]);
  dev->ac6 = (uint16_t)((cal[10] << 8) | cal[11]);
  dev->b1  = (int16_t)((cal[12] << 8) | cal[13]);
  dev->b2  = (int16_t)((cal[14] << 8) | cal[15]);
  dev->mb  = (int16_t)((cal[16] << 8) | cal[17]);
  dev->mc  = (int16_t)((cal[18] << 8) | cal[19]);
  dev->md  = (int16_t)((cal[20] << 8) | cal[21]);

  /* An all-0x00 or all-0xFF block means the EEPROM read failed. */
  if (dev->ac1 == 0 || dev->ac1 == -1 || dev->ac4 == 0 || dev->ac4 == 0xFFFF)
  {
    printf("gy87: BMP180 calibration data looks invalid\r\n");
    return;
  }

  dev->baro_present = true;
  dev->baro_state = BARO_IDLE;
  printf("gy87: BMP180 ready (AC1=%d AC4=%u MC=%d)\r\n",
         (int)dev->ac1, (unsigned)dev->ac4, (int)dev->mc);
}

HAL_StatusTypeDef gy87_init(gy87_t *dev)
{
  memset(dev, 0, sizeof(*dev));

  if (mpu6050_init(dev) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* The mag/baro are optional: a board missing one should still report accel. */
  mag_init(dev);
  bmp180_init(dev);

  return HAL_OK;
}

/* -------------------------------------------------------------------------- */
/*                                    Read                                    */
/* -------------------------------------------------------------------------- */

HAL_StatusTypeDef gy87_read_motion(gy87_t *dev, gy87_data_t *out)
{
  /* ACCEL_XOUT_H..GYRO_ZOUT_L is contiguous: 6 accel + 2 temp + 6 gyro */
  uint8_t d[14];

  if (!dev->mpu_present)
  {
    return HAL_ERROR;
  }
  if (i2c_read_regs(MPU6050_ADDR, MPU_ACCEL_XOUT_H, d, sizeof(d)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  int16_t rax = (int16_t)((d[0]  << 8) | d[1]);
  int16_t ray = (int16_t)((d[2]  << 8) | d[3]);
  int16_t raz = (int16_t)((d[4]  << 8) | d[5]);
  int16_t rt  = (int16_t)((d[6]  << 8) | d[7]);
  int16_t rgx = (int16_t)((d[8]  << 8) | d[9]);
  int16_t rgy = (int16_t)((d[10] << 8) | d[11]);
  int16_t rgz = (int16_t)((d[12] << 8) | d[13]);

  out->ax = rax / ACCEL_LSB_PER_G;
  out->ay = ray / ACCEL_LSB_PER_G;
  out->az = raz / ACCEL_LSB_PER_G;
  out->gx = rgx / GYRO_LSB_PER_DPS;
  out->gy = rgy / GYRO_LSB_PER_DPS;
  out->gz = rgz / GYRO_LSB_PER_DPS;
  out->temp_mpu = rt / 340.0f + 36.53f;   /* datasheet 4.19 */

  return HAL_OK;
}

HAL_StatusTypeDef gy87_read_mag(gy87_t *dev, gy87_data_t *out)
{
  uint8_t d[6];

  if (dev->mag_type == MAG_HMC5883L)
  {
    if (i2c_read_regs(HMC5883L_ADDR, HMC_DATA_X_MSB, d, sizeof(d)) != HAL_OK)
    {
      return HAL_ERROR;
    }
    /* Note the register order is X, Z, Y -- big-endian. */
    int16_t x = (int16_t)((d[0] << 8) | d[1]);
    int16_t z = (int16_t)((d[2] << 8) | d[3]);
    int16_t y = (int16_t)((d[4] << 8) | d[5]);

    out->mx = x / HMC_LSB_PER_GAUSS * 100.0f;
    out->my = y / HMC_LSB_PER_GAUSS * 100.0f;
    out->mz = z / HMC_LSB_PER_GAUSS * 100.0f;
    return HAL_OK;
  }

  if (dev->mag_type == MAG_QMC5883L)
  {
    if (i2c_read_regs(QMC5883L_ADDR, QMC_DATA_X_LSB, d, sizeof(d)) != HAL_OK)
    {
      return HAL_ERROR;
    }
    /* X, Y, Z -- little-endian on this part. */
    int16_t x = (int16_t)((d[1] << 8) | d[0]);
    int16_t y = (int16_t)((d[3] << 8) | d[2]);
    int16_t z = (int16_t)((d[5] << 8) | d[4]);

    out->mx = x / QMC_LSB_PER_GAUSS * 100.0f;
    out->my = y / QMC_LSB_PER_GAUSS * 100.0f;
    out->mz = z / QMC_LSB_PER_GAUSS * 100.0f;
    return HAL_OK;
  }

  return HAL_ERROR;
}

/* Bosch fixed-point compensation, BMP180 datasheet 3.5. */
static void bmp180_compensate(gy87_t *dev, int32_t ut, int32_t up, gy87_data_t *out)
{
  int32_t x1 = ((ut - (int32_t)dev->ac6) * (int32_t)dev->ac5) >> 15;
  int32_t x2 = ((int32_t)dev->mc << 11) / (x1 + dev->md);
  int32_t b5 = x1 + x2;

  out->temp_baro = ((b5 + 8) >> 4) / 10.0f;

  int32_t b6 = b5 - 4000;
  x1 = ((int32_t)dev->b2 * ((b6 * b6) >> 12)) >> 11;
  x2 = ((int32_t)dev->ac2 * b6) >> 11;
  int32_t x3 = x1 + x2;
  int32_t b3 = (((((int32_t)dev->ac1 * 4) + x3) << BMP_OSS) + 2) / 4;

  x1 = ((int32_t)dev->ac3 * b6) >> 13;
  x2 = ((int32_t)dev->b1 * ((b6 * b6) >> 12)) >> 16;
  x3 = ((x1 + x2) + 2) >> 2;

  uint32_t b4 = ((uint32_t)dev->ac4 * (uint32_t)(x3 + 32768)) >> 15;
  uint32_t b7 = ((uint32_t)up - b3) * (50000 >> BMP_OSS);

  int32_t p = (b7 < 0x80000000UL) ? (int32_t)((b7 * 2) / b4)
                                  : (int32_t)((b7 / b4) * 2);
  x1 = (p >> 8) * (p >> 8);
  x1 = (x1 * 3038) >> 16;
  x2 = (-7357 * p) >> 16;
  p += (x1 + x2 + 3791) >> 4;

  out->pressure = (float)p;
}

bool gy87_baro_step(gy87_t *dev, gy87_data_t *out)
{
  uint8_t d[3];

  if (!dev->baro_present)
  {
    return false;
  }

  switch (dev->baro_state)
  {
  case BARO_IDLE:
    if (i2c_write_reg(BMP180_ADDR, BMP_CTRL_MEAS, BMP_CMD_READ_TEMP) != HAL_OK)
    {
      return false;
    }
    dev->baro_deadline = HAL_GetTick() + BMP_TEMP_WAIT_MS;
    dev->baro_state = BARO_WAIT_TEMP;
    return false;

  case BARO_WAIT_TEMP:
    if ((int32_t)(HAL_GetTick() - dev->baro_deadline) < 0)
    {
      return false;
    }
    if (i2c_read_regs(BMP180_ADDR, BMP_DATA_MSB, d, 2) != HAL_OK)
    {
      dev->baro_state = BARO_IDLE;
      return false;
    }
    dev->baro_ut = (d[0] << 8) | d[1];

    if (i2c_write_reg(BMP180_ADDR, BMP_CTRL_MEAS,
                      BMP_CMD_READ_PRESS | (BMP_OSS << 6)) != HAL_OK)
    {
      dev->baro_state = BARO_IDLE;
      return false;
    }
    dev->baro_deadline = HAL_GetTick() + BMP_PRESS_WAIT_MS;
    dev->baro_state = BARO_WAIT_PRESSURE;
    return false;

  case BARO_WAIT_PRESSURE:
  default:
    if ((int32_t)(HAL_GetTick() - dev->baro_deadline) < 0)
    {
      return false;
    }
    dev->baro_state = BARO_IDLE;
    if (i2c_read_regs(BMP180_ADDR, BMP_DATA_MSB, d, 3) != HAL_OK)
    {
      return false;
    }

    int32_t up = (((int32_t)d[0] << 16) | ((int32_t)d[1] << 8) | d[2]) >> (8 - BMP_OSS);
    bmp180_compensate(dev, dev->baro_ut, up, out);
    return true;
  }
}
