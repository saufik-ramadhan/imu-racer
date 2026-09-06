#include "max7219.h"
#include "spi.h"

#include <stdio.h>
#include <string.h>

/* Register map, datasheet table 2. */
#define REG_NOOP          0x00
#define REG_DIGIT0        0x01
#define REG_DECODE_MODE   0x09
#define REG_INTENSITY     0x0A
#define REG_SCAN_LIMIT    0x0B
#define REG_SHUTDOWN      0x0C
#define REG_DISPLAY_TEST  0x0F

#define SPI_TIMEOUT_MS    10U

static HAL_StatusTypeDef spi_reconfigure(void)
{
  /*
   * CubeMX generates 4-bit words at 32 Mbit/s, neither of which this chip can
   * use: it wants 16-bit frames (sent as two bytes) and tops out at 10 MHz.
   * SPI4 carries nothing else, so the driver just sets what the panel needs
   * instead of making the .ioc the place you have to get right.
   */
  hspi4.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi4.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;   /* 64 MHz / 8 = 8 MHz */
  hspi4.Init.CLKPolarity = SPI_POLARITY_LOW;                /* mode 0 */
  hspi4.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi4.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi4.Init.NSS = SPI_NSS_SOFT;

  return HAL_SPI_Init(&hspi4);
}

static void cs_init(void)
{
  GPIO_InitTypeDef gpio = { 0 };

  MAX7219_CS_CLK_ENABLE();

  /* Idle high: the MAX7219 latches on the rising edge, so the pin must never
   * float low while the panel is being configured. */
  HAL_GPIO_WritePin(MAX7219_CS_PORT, MAX7219_CS_PIN, GPIO_PIN_SET);

  gpio.Pin = MAX7219_CS_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(MAX7219_CS_PORT, &gpio);
}

/** One 16-bit frame: address then data, latched when CS goes back high. */
static HAL_StatusTypeDef send(uint8_t reg, uint8_t value)
{
  uint8_t frame[2] = { reg, value };
  HAL_StatusTypeDef ret;

  HAL_GPIO_WritePin(MAX7219_CS_PORT, MAX7219_CS_PIN, GPIO_PIN_RESET);
  ret = HAL_SPI_Transmit(&hspi4, frame, sizeof(frame), SPI_TIMEOUT_MS);
  HAL_GPIO_WritePin(MAX7219_CS_PORT, MAX7219_CS_PIN, GPIO_PIN_SET);

  return ret;
}

HAL_StatusTypeDef max7219_init(max7219_t *m, uint8_t intensity)
{
  memset(m, 0, sizeof(*m));

  if (spi_reconfigure() != HAL_OK)
  {
    printf("max7219: SPI4 reconfigure failed\r\n");
    return HAL_ERROR;
  }
  cs_init();

  if (intensity > MAX7219_MAX_INTENSITY)
  {
    intensity = MAX7219_MAX_INTENSITY;
  }
  m->intensity = intensity;

  const uint8_t setup[][2] = {
    { REG_DISPLAY_TEST, 0x00 },              /* leave the all-on test mode    */
    { REG_SHUTDOWN,     0x00 },              /* dark while we configure       */
    { REG_DECODE_MODE,  0x00 },              /* raw segments, not BCD digits  */
    { REG_SCAN_LIMIT,   MAX7219_SIZE - 1 },  /* drive all eight rows          */
    { REG_INTENSITY,    intensity },
  };

  for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++)
  {
    if (send(setup[i][0], setup[i][1]) != HAL_OK)
    {
      printf("max7219: setup write failed\r\n");
      return HAL_ERROR;
    }
  }

  m->present = true;
  if (max7219_flush(m) != HAL_OK || send(REG_SHUTDOWN, 0x01) != HAL_OK)
  {
    m->present = false;
    return HAL_ERROR;
  }

  /* Deliberately not called "detected": this chip cannot be read back, so all
   * we know is that the writes left the STM32. */
  printf("max7219: 8x8 matrix configured on SPI4, CS on PE4 (no readback)\r\n");
  return HAL_OK;
}

void max7219_clear(max7219_t *m)
{
  memset(m->row, 0x00, sizeof(m->row));
}

void max7219_pixel(max7219_t *m, int x, int y, bool on)
{
  if (x < 0 || x >= MAX7219_SIZE || y < 0 || y >= MAX7219_SIZE)
  {
    return;
  }

#if MAX7219_TRANSPOSE
  int t = x;
  x = y;
  y = t;
#endif
#if MAX7219_FLIP_X
  x = MAX7219_SIZE - 1 - x;
#endif
#if MAX7219_FLIP_Y
  y = MAX7219_SIZE - 1 - y;
#endif

  /* Segment D7 is the leftmost column on the usual module wiring. */
  uint8_t mask = (uint8_t)(1U << (MAX7219_SIZE - 1 - x));

  if (on)
  {
    m->row[y] |= mask;
  }
  else
  {
    m->row[y] = (uint8_t)(m->row[y] & ~mask);
  }
}

HAL_StatusTypeDef max7219_flush(max7219_t *m)
{
  if (!m->present)
  {
    return HAL_ERROR;
  }
  for (uint8_t i = 0; i < MAX7219_SIZE; i++)
  {
    if (send((uint8_t)(REG_DIGIT0 + i), m->row[i]) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }
  return HAL_OK;
}

HAL_StatusTypeDef max7219_set_intensity(max7219_t *m, uint8_t intensity)
{
  if (intensity > MAX7219_MAX_INTENSITY)
  {
    intensity = MAX7219_MAX_INTENSITY;
  }
  if (!m->present || intensity == m->intensity)
  {
    return HAL_OK;
  }
  m->intensity = intensity;
  return send(REG_INTENSITY, intensity);
}
