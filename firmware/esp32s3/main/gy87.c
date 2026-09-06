#include "gy87.h"
#include "i2c_bus.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gy87";

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

/* ±2 g and ±250 deg/s full-scale => these sensitivities (datasheet §6.2/6.3) */
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

static esp_err_t add_device(i2c_master_bus_handle_t bus, uint16_t addr,
                            i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_SENSOR_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

/* -------------------------------------------------------------------------- */
/*                                    Init                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t mpu6050_init(gy87_t *dev)
{
    uint8_t who = 0;
    esp_err_t ret = i2c_read_regs(dev->mpu, MPU_WHO_AM_I, &who, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 not responding at 0x%02X: %s", MPU6050_ADDR, esp_err_to_name(ret));
        return ret;
    }
    /* Genuine parts return 0x68; several clones report 0x70/0x72 but work fine. */
    ESP_LOGI(TAG, "MPU6050 WHO_AM_I = 0x%02X", who);

    /* Device reset, then wait for the internal registers to settle. */
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_PWR_MGMT_1, 0x80), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Wake up and clock off the gyro X PLL -- more stable than the internal RC. */
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_PWR_MGMT_1, 0x01), TAG, "wake failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_CONFIG, 0x03), TAG, "dlpf failed");        /* DLPF 44 Hz */
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_SMPLRT_DIV, 0x04), TAG, "rate failed");    /* 1 kHz/5 = 200 Hz */
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_GYRO_CONFIG, 0x00), TAG, "gyro fs failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_ACCEL_CONFIG, 0x00), TAG, "accel fs failed");

    /* The mag and baro sit on the MPU's *auxiliary* bus. Disabling the aux-I2C
     * master and setting I2C_BYPASS_EN wires them straight through to the ESP32,
     * which is what makes 0x1E/0x0D/0x77 visible to us at all. */
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_USER_CTRL, 0x00), TAG, "user_ctrl failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->mpu, MPU_INT_PIN_CFG, 0x02), TAG, "bypass failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "MPU6050 ready, aux-I2C bypass enabled");
    return ESP_OK;
}

static esp_err_t mag_init(i2c_master_bus_handle_t bus, gy87_t *dev)
{
    dev->mag_type = MAG_NONE;

    if (i2c_master_probe(bus, HMC5883L_ADDR, 50) == ESP_OK) {
        ESP_RETURN_ON_ERROR(add_device(bus, HMC5883L_ADDR, &dev->mag), TAG, "mag add failed");
        uint8_t id[3] = {0};
        i2c_read_regs(dev->mag, HMC_ID_A, id, 3);
        if (id[0] == 'H' && id[1] == '4' && id[2] == '3') {
            ESP_LOGI(TAG, "HMC5883L detected (ID \"H43\")");
        } else {
            ESP_LOGW(TAG, "Device at 0x1E but ID is %02X %02X %02X -- treating as HMC5883L",
                     id[0], id[1], id[2]);
        }
        i2c_write_reg(dev->mag, HMC_CONFIG_A, 0x70);  /* 8-sample average, 15 Hz, normal bias */
        i2c_write_reg(dev->mag, HMC_CONFIG_B, 0x20);  /* gain 1090 LSB/Gauss (+-1.3 Ga)       */
        i2c_write_reg(dev->mag, HMC_MODE, 0x00);      /* continuous measurement               */
        dev->mag_type = MAG_HMC5883L;
        vTaskDelay(pdMS_TO_TICKS(10));
        return ESP_OK;
    }

    if (i2c_master_probe(bus, QMC5883L_ADDR, 50) == ESP_OK) {
        ESP_RETURN_ON_ERROR(add_device(bus, QMC5883L_ADDR, &dev->mag), TAG, "mag add failed");
        i2c_write_reg(dev->mag, QMC_SET_RESET, 0x01);  /* recommended set/reset period */
        i2c_write_reg(dev->mag, QMC_CONTROL_1, 0x1D);  /* OSR512, 8 Ga, 200 Hz, continuous */
        dev->mag_type = MAG_QMC5883L;
        ESP_LOGI(TAG, "QMC5883L detected at 0x0D");
        vTaskDelay(pdMS_TO_TICKS(10));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "No magnetometer found -- bypass may have failed, or this board has none");
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t bmp180_init(i2c_master_bus_handle_t bus, gy87_t *dev)
{
    dev->baro_present = false;

    if (i2c_master_probe(bus, BMP180_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "No barometer at 0x%02X", BMP180_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(add_device(bus, BMP180_ADDR, &dev->baro), TAG, "baro add failed");

    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(i2c_read_regs(dev->baro, BMP_CHIP_ID, &chip_id, 1), TAG, "baro id failed");
    if (chip_id != 0x55) {
        /* 0x58 is a BMP280, which speaks a completely different register map. */
        ESP_LOGW(TAG, "Chip at 0x77 reports ID 0x%02X, not BMP180 (0x55) -- skipping barometer%s",
                 chip_id, chip_id == 0x58 ? " (looks like a BMP280)" : "");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 11 big-endian 16-bit calibration words in EEPROM at 0xAA..0xBF */
    uint8_t cal[22];
    ESP_RETURN_ON_ERROR(i2c_read_regs(dev->baro, BMP_CAL_START, cal, sizeof(cal)), TAG, "cal read failed");

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
    if (dev->ac1 == 0 || dev->ac1 == -1 || dev->ac4 == 0 || dev->ac4 == 0xFFFF) {
        ESP_LOGE(TAG, "BMP180 calibration data looks invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }

    dev->baro_present = true;
    ESP_LOGI(TAG, "BMP180 ready (AC1=%d AC4=%u MC=%d)", dev->ac1, dev->ac4, dev->mc);
    return ESP_OK;
}

esp_err_t gy87_init(i2c_master_bus_handle_t bus, gy87_t *dev)
{
    memset(dev, 0, sizeof(*dev));

    ESP_RETURN_ON_ERROR(add_device(bus, MPU6050_ADDR, &dev->mpu), TAG, "mpu add failed");
    ESP_RETURN_ON_ERROR(mpu6050_init(dev), TAG, "mpu init failed");

    /* The mag/baro are optional: a board missing one should still report accel. */
    mag_init(bus, dev);
    bmp180_init(bus, dev);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                                    Read                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t mpu6050_read(gy87_t *dev, gy87_data_t *out)
{
    /* ACCEL_XOUT_H..GYRO_ZOUT_L is contiguous: 6 accel + 2 temp + 6 gyro */
    uint8_t d[14];
    ESP_RETURN_ON_ERROR(i2c_read_regs(dev->mpu, MPU_ACCEL_XOUT_H, d, sizeof(d)), TAG, "mpu read failed");

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
    out->temp_mpu = rt / 340.0f + 36.53f;   /* datasheet §4.19 */

    return ESP_OK;
}

static esp_err_t mag_read(gy87_t *dev, gy87_data_t *out)
{
    uint8_t d[6];

    if (dev->mag_type == MAG_HMC5883L) {
        ESP_RETURN_ON_ERROR(i2c_read_regs(dev->mag, HMC_DATA_X_MSB, d, sizeof(d)), TAG, "mag read failed");
        /* Note the register order is X, Z, Y -- big-endian. */
        int16_t x = (int16_t)((d[0] << 8) | d[1]);
        int16_t z = (int16_t)((d[2] << 8) | d[3]);
        int16_t y = (int16_t)((d[4] << 8) | d[5]);
        out->mx = x / HMC_LSB_PER_GAUSS * 100.0f;
        out->my = y / HMC_LSB_PER_GAUSS * 100.0f;
        out->mz = z / HMC_LSB_PER_GAUSS * 100.0f;
        return ESP_OK;
    }

    if (dev->mag_type == MAG_QMC5883L) {
        ESP_RETURN_ON_ERROR(i2c_read_regs(dev->mag, QMC_DATA_X_LSB, d, sizeof(d)), TAG, "mag read failed");
        /* X, Y, Z -- little-endian on this part. */
        int16_t x = (int16_t)((d[1] << 8) | d[0]);
        int16_t y = (int16_t)((d[3] << 8) | d[2]);
        int16_t z = (int16_t)((d[5] << 8) | d[4]);
        out->mx = x / QMC_LSB_PER_GAUSS * 100.0f;
        out->my = y / QMC_LSB_PER_GAUSS * 100.0f;
        out->mz = z / QMC_LSB_PER_GAUSS * 100.0f;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t bmp180_read(gy87_t *dev, gy87_data_t *out)
{
    if (!dev->baro_present) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t d[3];

    /* --- uncompensated temperature --- */
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->baro, BMP_CTRL_MEAS, BMP_CMD_READ_TEMP), TAG, "temp cmd failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(i2c_read_regs(dev->baro, BMP_DATA_MSB, d, 2), TAG, "temp read failed");
    int32_t ut = (d[0] << 8) | d[1];

    /* --- uncompensated pressure --- */
    ESP_RETURN_ON_ERROR(i2c_write_reg(dev->baro, BMP_CTRL_MEAS, BMP_CMD_READ_PRESS | (BMP_OSS << 6)),
                        TAG, "press cmd failed");
    vTaskDelay(pdMS_TO_TICKS(30));   /* 25.5 ms at OSS=3, rounded up to a tick boundary */
    ESP_RETURN_ON_ERROR(i2c_read_regs(dev->baro, BMP_DATA_MSB, d, 3), TAG, "press read failed");
    int32_t up = (((int32_t)d[0] << 16) | ((int32_t)d[1] << 8) | d[2]) >> (8 - BMP_OSS);

    /* --- Bosch fixed-point compensation (BMP180 datasheet §3.5) --- */
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
    /* International Standard Atmosphere, referenced to 1013.25 hPa */
    out->altitude = 44330.0f * (1.0f - powf((float)p / 101325.0f, 0.1902949f));

    return ESP_OK;
}

esp_err_t gy87_read_motion(gy87_t *dev, gy87_data_t *out)
{
    return mpu6050_read(dev, out);
}

esp_err_t gy87_read(gy87_t *dev, gy87_data_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_err_t ret = mpu6050_read(dev, out);
    mag_read(dev, out);      /* best effort -- absent chip simply leaves zeros */
    bmp180_read(dev, out);

    return ret;
}
