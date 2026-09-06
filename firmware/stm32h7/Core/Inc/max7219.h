#ifndef INC_MAX7219_H
#define INC_MAX7219_H

#include "main.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MAX7219 8x8 LED matrix on SPI4 (PE2 SCK, PE6 MOSI). MISO is unused -- the
 * chip has no readback at all, which is worth knowing up front: nothing here
 * can tell a working panel from a disconnected one.
 *
 * A full refresh is 8 registers of two bytes each, ~16 us at 8 MHz. That is
 * nothing next to the 20 ms control tick, so unlike the OLED this panel is
 * redrawn in one go every pass.
 */

/* Chip select. The .ioc leaves NSS in software mode and defines no output for
 * it, so the driver claims this pin itself -- PE4 is free and sits next to the
 * SPI4 signals on the header. */
#define MAX7219_CS_PORT       GPIOE
#define MAX7219_CS_PIN        GPIO_PIN_4
#define MAX7219_CS_CLK_ENABLE() __HAL_RCC_GPIOE_CLK_ENABLE()

/*
 * Modules differ in how the digit and segment lines are wired to the panel,
 * and the panel itself can be mounted any way round. Correct both here rather
 * than in the drawing code, which stays in plain top-left-origin coordinates.
 *
 * These three are applied in order: transpose, then flip X, then flip Y.
 * Transpose with one flip is a 90 degree rotation -- which one depends on
 * which flip, so swapping FLIP_X for FLIP_Y turns the image the other way.
 */
#define MAX7219_FLIP_X        1
#define MAX7219_FLIP_Y        0
#define MAX7219_TRANSPOSE     1

#define MAX7219_SIZE          8
#define MAX7219_MAX_INTENSITY 15

typedef struct {
  uint8_t row[MAX7219_SIZE];
  uint8_t intensity;
  bool    present;
} max7219_t;

/**
 * Reconfigure SPI4 for this chip and bring the panel up dark and cleared.
 *
 * Returns HAL_ERROR only when the SPI transfers themselves fail; a panel that
 * is simply not plugged in still returns HAL_OK.
 */
HAL_StatusTypeDef max7219_init(max7219_t *m, uint8_t intensity);

/** Clear the in-RAM frame (call max7219_flush() to push it out). */
void max7219_clear(max7219_t *m);

/** Set/clear one pixel, origin top-left. Out-of-range coordinates are ignored. */
void max7219_pixel(max7219_t *m, int x, int y, bool on);

/** Push all eight rows. */
HAL_StatusTypeDef max7219_flush(max7219_t *m);

/** 0..15. Cheap no-op when the value has not changed. */
HAL_StatusTypeDef max7219_set_intensity(max7219_t *m, uint8_t intensity);

#ifdef __cplusplus
}
#endif

#endif /* INC_MAX7219_H */
