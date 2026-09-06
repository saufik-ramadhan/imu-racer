#ifndef INC_SSD1306_H
#define INC_SSD1306_H

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_ADDR     0x3C   /* 0x3D if the module address jumper is moved */
#define SSD1306_WIDTH    128
#define SSD1306_HEIGHT   64     /* set to 32 for the half-height modules */
#define SSD1306_PAGES    (SSD1306_HEIGHT / 8)

/*
 * Rotate the image 180 degrees, for a panel mounted the other way up. This is
 * done in the panel itself -- segment remap plus COM scan direction -- so the
 * frame buffer, the text origin and the pixel coordinates are all unaffected.
 * Set to 0 to go back.
 */
#define SSD1306_FLIP_180 1

/*
 * The frame buffer is stored one page per row, each row prefixed with the 0x40
 * "data stream" control byte. That way a page ships in a single I2C write with
 * no copying, which is what makes the incremental flush below cheap.
 */
typedef struct {
  uint8_t page[SSD1306_PAGES][1 + SSD1306_WIDTH];
  uint8_t next_page;   /* cursor for ssd1306_flush_step() */
  bool    present;
} ssd1306_t;

HAL_StatusTypeDef ssd1306_init(ssd1306_t *disp);

/** Clear the in-RAM frame buffer (call a flush to push it out). */
void ssd1306_clear(ssd1306_t *disp);

/** Set/clear a single pixel. Out-of-range coordinates are ignored. */
void ssd1306_pixel(ssd1306_t *disp, int x, int y, bool on);

/** Draw one 6x8 character. @p page is the 8-pixel text row (0..PAGES-1). */
void ssd1306_char(ssd1306_t *disp, int page, int col_px, char c);

/** Draw a NUL-terminated string, clipped at the right edge. */
void ssd1306_text(ssd1306_t *disp, int page, int col_px, const char *str);

/** printf-style variant of ssd1306_text(). Do not use %f -- see racer_app.c. */
void ssd1306_printf(ssd1306_t *disp, int page, int col_px, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/**
 * Push the whole frame buffer. Blocks for ~24 ms at 400 kHz, so it is meant for
 * splash screens and error messages, not for the control loop.
 */
HAL_StatusTypeDef ssd1306_flush(ssd1306_t *disp);

/**
 * Push one page and advance the cursor, ~3 ms. Calling this once per control
 * tick spreads a full refresh over 8 ticks instead of stalling one of them,
 * which matters because the IMU shares this bus.
 */
HAL_StatusTypeDef ssd1306_flush_step(ssd1306_t *disp);

/** Turn the panel on or off without losing GDDRAM contents. */
HAL_StatusTypeDef ssd1306_display_on(ssd1306_t *disp, bool on);

#ifdef __cplusplus
}
#endif

#endif /* INC_SSD1306_H */
