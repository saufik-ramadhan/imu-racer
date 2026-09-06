#include "i2c_bus.h"
#include "i2c.h"

#include <stdio.h>

HAL_StatusTypeDef i2c_bus_init(void)
{
  if (hi2c4.Init.Timing != I2C_BUS_TIMING_100KHZ)
  {
    return HAL_OK;      /* the generated code already asked for fast mode */
  }

  printf("i2c: generated timing is 100 kHz, re-opening the bus at ~390 kHz\r\n");
  hi2c4.Init.Timing = I2C_BUS_TIMING_400KHZ;
  if (HAL_I2C_Init(&hi2c4) != HAL_OK)
  {
    return HAL_ERROR;
  }
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c4, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    return HAL_ERROR;
  }
  return HAL_I2CEx_ConfigDigitalFilter(&hi2c4, 0);
}

bool i2c_bus_probe(uint8_t addr7)
{
  /* One short attempt: a present device ACKs immediately, and the startup scan
   * walks 112 addresses -- retries there would cost seconds of boot time. */
  return HAL_I2C_IsDeviceReady(&hi2c4, (uint16_t)(addr7 << 1), 1, 5) == HAL_OK;
}

void i2c_bus_scan(void)
{
  int found = 0;

  printf("i2c: scanning I2C4 (PF14=SCL PF15=SDA)\r\n");
  for (uint8_t addr = 0x08; addr < 0x78; addr++)
  {
    if (i2c_bus_probe(addr))
    {
      printf("i2c:   found device at 0x%02X\r\n", addr);
      found++;
    }
  }
  if (found == 0)
  {
    printf("i2c:   nothing answered -- check wiring, pull-ups and 3V3\r\n");
  }
}

HAL_StatusTypeDef i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t val)
{
  return HAL_I2C_Mem_Write(&hi2c4, (uint16_t)(addr7 << 1), reg,
                           I2C_MEMADD_SIZE_8BIT, &val, 1, I2C_XFER_TIMEOUT_MS);
}

HAL_StatusTypeDef i2c_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len)
{
  return HAL_I2C_Mem_Read(&hi2c4, (uint16_t)(addr7 << 1), reg,
                          I2C_MEMADD_SIZE_8BIT, buf, len, I2C_XFER_TIMEOUT_MS);
}

HAL_StatusTypeDef i2c_write_raw(uint8_t addr7, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
  return HAL_I2C_Master_Transmit(&hi2c4, (uint16_t)(addr7 << 1), (uint8_t *)buf, len, timeout_ms);
}

uint32_t i2c_bus_last_error(void)
{
  return HAL_I2C_GetError(&hi2c4);
}

const char *i2c_bus_error_name(uint32_t err)
{
  /* Checked most-specific first: a stuck bus usually raises several at once,
   * and the first one named is the one worth chasing. */
  if ((err & HAL_I2C_ERROR_AF) != 0U)      { return "AF (no ACK -- device absent or wrong address)"; }
  if ((err & HAL_I2C_ERROR_ARLO) != 0U)    { return "ARLO (arbitration lost -- SDA driven by something else)"; }
  if ((err & HAL_I2C_ERROR_BERR) != 0U)    { return "BERR (misplaced START/STOP -- wiring or pull-ups)"; }
  if ((err & HAL_I2C_ERROR_TIMEOUT) != 0U) { return "TIMEOUT (SCL held low -- bus hung)"; }
  if ((err & HAL_I2C_ERROR_OVR) != 0U)     { return "OVR"; }
  if ((err & HAL_I2C_ERROR_DMA) != 0U)     { return "DMA"; }
  if (err == HAL_I2C_ERROR_NONE)           { return "none"; }
  return "unknown";
}
