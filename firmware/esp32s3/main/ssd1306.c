#include "ssd1306.h"
#include "i2c_bus.h"
#include "ssd1306_font.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ssd1306";

#define CTRL_CMD    0x00   /* Co=0, D/C=0: the rest of the frame is commands */
#define CTRL_DATA   0x40   /* Co=0, D/C=1: the rest of the frame is GDDRAM   */

static esp_err_t send_cmds(ssd1306_t *disp, const uint8_t *cmds, size_t len)
{
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, len);
    return i2c_master_transmit(disp->dev, buf, len + 1, I2C_XFER_TIMEOUT_MS);
}

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, ssd1306_t *disp)
{
    memset(disp, 0, sizeof(*disp));
    disp->fb[0] = CTRL_DATA;

    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, SSD1306_ADDR, 100), TAG,
                        "no SSD1306 at 0x%02X", SSD1306_ADDR);

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SSD1306_ADDR,
        .scl_speed_hz = I2C_OLED_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &disp->dev), TAG, "add device failed");

    static const uint8_t init_seq[] = {
        0xAE,               /* display off                                  */
        0xD5, 0x80,         /* clock divide ratio / osc frequency           */
        0xA8, SSD1306_HEIGHT - 1,  /* multiplex ratio                       */
        0xD3, 0x00,         /* display offset                               */
        0x40,               /* start line = 0                               */
        0x8D, 0x14,         /* charge pump on (0x10 for external VCC)       */
        0x20, 0x00,         /* horizontal addressing mode                   */
        0xA1,               /* segment remap: column 127 -> SEG0            */
        0xC8,               /* COM scan direction remapped (flips vertical) */
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
    for (size_t i = 0; i < sizeof(init_seq); i += 8) {
        size_t chunk = (sizeof(init_seq) - i) < 8 ? (sizeof(init_seq) - i) : 8;
        ESP_RETURN_ON_ERROR(send_cmds(disp, &init_seq[i], chunk), TAG, "init cmd failed");
    }

    ssd1306_clear(disp);
    ESP_RETURN_ON_ERROR(ssd1306_flush(disp), TAG, "initial flush failed");
    ESP_RETURN_ON_ERROR(ssd1306_display_on(disp, true), TAG, "display on failed");

    ESP_LOGI(TAG, "SSD1306 %dx%d ready at 0x%02X", SSD1306_WIDTH, SSD1306_HEIGHT, SSD1306_ADDR);
    return ESP_OK;
}

esp_err_t ssd1306_display_on(ssd1306_t *disp, bool on)
{
    uint8_t cmd = on ? 0xAF : 0xAE;
    return send_cmds(disp, &cmd, 1);
}

void ssd1306_clear(ssd1306_t *disp)
{
    memset(&disp->fb[1], 0x00, SSD1306_BUF_SIZE);
}

void ssd1306_pixel(ssd1306_t *disp, int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    /* +1 skips the control byte at fb[0]. */
    uint8_t *byte = &disp->fb[1 + (y / 8) * SSD1306_WIDTH + x];
    uint8_t mask = 1 << (y % 8);
    if (on) {
        *byte |= mask;
    } else {
        *byte &= ~mask;
    }
}

void ssd1306_char(ssd1306_t *disp, int page, int col_px, char c)
{
    if (page < 0 || page >= SSD1306_PAGES) {
        return;
    }
    if (c < 0x20 || (uint8_t)c > 0x7F) {
        c = '?';
    }
    const uint8_t *glyph = ssd1306_font5x7[c - 0x20];

    for (int i = 0; i < 5; i++) {
        int x = col_px + i;
        if (x < 0 || x >= SSD1306_WIDTH) {
            continue;
        }
        disp->fb[1 + page * SSD1306_WIDTH + x] = glyph[i];
    }
    /* One blank column of inter-character spacing. */
    int x = col_px + 5;
    if (x >= 0 && x < SSD1306_WIDTH) {
        disp->fb[1 + page * SSD1306_WIDTH + x] = 0x00;
    }
}

void ssd1306_text(ssd1306_t *disp, int page, int col_px, const char *str)
{
    for (const char *p = str; *p; p++, col_px += 6) {
        if (col_px >= SSD1306_WIDTH) {
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

esp_err_t ssd1306_flush(ssd1306_t *disp)
{
    /* Reset the addressing window, then stream all pages in one transfer. */
    static const uint8_t window[] = {
        0x21, 0, SSD1306_WIDTH - 1,      /* column address range */
        0x22, 0, SSD1306_PAGES - 1,      /* page address range   */
    };
    ESP_RETURN_ON_ERROR(send_cmds(disp, window, sizeof(window)), TAG, "set window failed");

    return i2c_master_transmit(disp->dev, disp->fb, sizeof(disp->fb), I2C_XFER_TIMEOUT_MS);
}
