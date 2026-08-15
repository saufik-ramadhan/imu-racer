#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_ADDR     0x3C   /* 0x3D if the module's address jumper is moved */
#define SSD1306_WIDTH    128
#define SSD1306_HEIGHT   64     /* set to 32 for the half-height modules */
#define SSD1306_PAGES    (SSD1306_HEIGHT / 8)
#define SSD1306_BUF_SIZE (SSD1306_WIDTH * SSD1306_PAGES)

typedef struct {
    i2c_master_dev_handle_t dev;
    /* Byte 0 is the 0x40 "data stream" control byte so the whole frame ships
     * in a single I2C transaction without an extra copy. */
    uint8_t fb[1 + SSD1306_BUF_SIZE];
} ssd1306_t;

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, ssd1306_t *disp);

/** Clear the in-RAM framebuffer (call ssd1306_flush() to push it out). */
void ssd1306_clear(ssd1306_t *disp);

/** Set/clear a single pixel. Out-of-range coordinates are ignored. */
void ssd1306_pixel(ssd1306_t *disp, int x, int y, bool on);

/** Draw one 6x8 character. @p page is the 8-pixel text row (0..PAGES-1). */
void ssd1306_char(ssd1306_t *disp, int page, int col_px, char c);

/** Draw a NUL-terminated string, clipped at the right edge. */
void ssd1306_text(ssd1306_t *disp, int page, int col_px, const char *str);

/** printf-style variant of ssd1306_text(). */
void ssd1306_printf(ssd1306_t *disp, int page, int col_px, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/** Push the framebuffer to the panel. */
esp_err_t ssd1306_flush(ssd1306_t *disp);

/** Turn the panel on or off without losing GDDRAM contents. */
esp_err_t ssd1306_display_on(ssd1306_t *disp, bool on);

#ifdef __cplusplus
}
#endif
