#include "ssd1306.h"
#include "i2c_bus.h"
#include "ssd1306_font.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CTRL_CMD    0x00   /* Co=0, D/C=0: the rest of the frame is commands */
#define CTRL_DATA   0x40   /* Co=0, D/C=1: the rest of the frame is GDDRAM   */

static HAL_StatusTypeDef send_cmds(const uint8_t *cmds, uint16_t len)
{
  uint8_t buf[16];

  if ((uint16_t)(len + 1) > sizeof(buf))
  {
    return HAL_ERROR;
  }
  buf[0] = CTRL_CMD;
  memcpy(&buf[1], cmds, len);
  return i2c_write_raw(SSD1306_ADDR, buf, (uint16_t)(len + 1), I2C_XFER_TIMEOUT_MS);
}

HAL_StatusTypeDef ssd1306_init(ssd1306_t *disp)
{
  memset(disp, 0, sizeof(*disp));
  for (int p = 0; p < SSD1306_PAGES; p++)
  {
    disp->page[p][0] = CTRL_DATA;
  }

  if (!i2c_bus_probe(SSD1306_ADDR))
  {
    printf("oled: no SSD1306 at 0x%02X\r\n", SSD1306_ADDR);
    return HAL_ERROR;
  }

  static const uint8_t init_seq[] = {
    0xAE,               /* display off                                  */
    0xD5, 0x80,         /* clock divide ratio / osc frequency           */
    0xA8, SSD1306_HEIGHT - 1,  /* multiplex ratio                       */
    0xD3, 0x00,         /* display offset                               */
    0x40,               /* start line = 0                               */
    0x8D, 0x14,         /* charge pump on (0x10 for external VCC)       */
    0x20, 0x02,         /* page addressing mode -- see push_page()      */
#if SSD1306_FLIP_180
    0xA0,               /* segment remap: column 0 -> SEG0             */
    0xC0,               /* COM scan direction normal                   */
#else
    0xA1,               /* segment remap: column 127 -> SEG0           */
    0xC8,               /* COM scan direction remapped                 */
#endif
#if SSD1306_HEIGHT == 32
    0xDA, 0x02,         /* COM pins: sequential, no remap               */
#else
    0xDA, 0x12,         /* COM pins: alternative, no remap              */
#endif
    0x81, 0x7F,         /* contrast                                     */
    0xD9, 0xF1,         /* pre-charge period                            */
    0xDB, 0x40,         /* VCOMH deselect level                         */
    0xA4,               /* resume from RAM (not all-pixels-on)          */
    0xA6,               /* normal, not inverted                         */
    0x2E,               /* deactivate scroll                            */
  };

  /* Split into chunks so each fits the small command buffer. */
  for (size_t i = 0; i < sizeof(init_seq); i += 8)
  {
    size_t chunk = (sizeof(init_seq) - i) < 8 ? (sizeof(init_seq) - i) : 8;
    if (send_cmds(&init_seq[i], (uint16_t)chunk) != HAL_OK)
    {
      printf("oled: init sequence failed\r\n");
      return HAL_ERROR;
    }
  }

  disp->present = true;
  ssd1306_clear(disp);
  if (ssd1306_flush(disp) != HAL_OK || ssd1306_display_on(disp, true) != HAL_OK)
  {
    disp->present = false;
    return HAL_ERROR;
  }

  printf("oled: SSD1306 %dx%d ready at 0x%02X\r\n",
         SSD1306_WIDTH, SSD1306_HEIGHT, SSD1306_ADDR);
  return HAL_OK;
}

HAL_StatusTypeDef ssd1306_display_on(ssd1306_t *disp, bool on)
{
  uint8_t cmd = on ? 0xAF : 0xAE;

  (void)disp;
  return send_cmds(&cmd, 1);
}

void ssd1306_clear(ssd1306_t *disp)
{
  for (int p = 0; p < SSD1306_PAGES; p++)
  {
    memset(&disp->page[p][1], 0x00, SSD1306_WIDTH);
  }
}

void ssd1306_pixel(ssd1306_t *disp, int x, int y, bool on)
{
  if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT)
  {
    return;
  }
  /* +1 skips the control byte at the head of each page. */
  uint8_t *byte = &disp->page[y / 8][1 + x];
  uint8_t mask = (uint8_t)(1U << (y % 8));

  if (on)
  {
    *byte |= mask;
  }
  else
  {
    *byte = (uint8_t)(*byte & ~mask);
  }
}

void ssd1306_char(ssd1306_t *disp, int page, int col_px, char c)
{
  if (page < 0 || page >= SSD1306_PAGES)
  {
    return;
  }
  if (c < 0x20 || (uint8_t)c > 0x7F)
  {
    c = '?';
  }

  const uint8_t *glyph = ssd1306_font5x7[c - 0x20];

  for (int i = 0; i < 5; i++)
  {
    int x = col_px + i;
    if (x < 0 || x >= SSD1306_WIDTH)
    {
      continue;
    }
    disp->page[page][1 + x] = glyph[i];
  }

  /* One blank column of inter-character spacing. */
  int x = col_px + 5;
  if (x >= 0 && x < SSD1306_WIDTH)
  {
    disp->page[page][1 + x] = 0x00;
  }
}

void ssd1306_text(ssd1306_t *disp, int page, int col_px, const char *str)
{
  for (const char *p = str; *p != 0; p++, col_px += 6)
  {
    if (col_px >= SSD1306_WIDTH)
    {
      break;
    }
    ssd1306_char(disp, page, col_px, *p);
  }
}

void ssd1306_printf(ssd1306_t *disp, int page, int col_px, const char *fmt, ...)
{
  char line[32];   /* 128 px / 6 px per char = 21 visible chars */
  va_list args;

  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  ssd1306_text(disp, page, col_px, line);
}

/* Point the panel write cursor at column 0 of @p page. */
static HAL_StatusTypeDef set_page(uint8_t page)
{
  const uint8_t cmds[] = { (uint8_t)(0xB0 | page), 0x00, 0x10 };

  return send_cmds(cmds, sizeof(cmds));
}

static HAL_StatusTypeDef push_page(ssd1306_t *disp, uint8_t page)
{
  if (!disp->present)
  {
    return HAL_ERROR;
  }
  if (set_page(page) != HAL_OK)
  {
    return HAL_ERROR;
  }
  return i2c_write_raw(SSD1306_ADDR, disp->page[page], 1 + SSD1306_WIDTH,
                       I2C_XFER_TIMEOUT_MS);
}

HAL_StatusTypeDef ssd1306_flush(ssd1306_t *disp)
{
  for (uint8_t p = 0; p < SSD1306_PAGES; p++)
  {
    if (push_page(disp, p) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }
  disp->next_page = 0;
  return HAL_OK;
}

HAL_StatusTypeDef ssd1306_flush_step(ssd1306_t *disp)
{
  HAL_StatusTypeDef ret = push_page(disp, disp->next_page);

  disp->next_page = (uint8_t)((disp->next_page + 1) % SSD1306_PAGES);
  return ret;
}
