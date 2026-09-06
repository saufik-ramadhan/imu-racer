#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

esp_err_t i2c_buses_init(i2c_master_bus_handle_t *sensor_bus,
                         i2c_master_bus_handle_t *oled_bus)
{
    if (sensor_bus) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_SENSOR_PORT,
            .sda_io_num = I2C_SENSOR_SDA_PIN,
            .scl_io_num = I2C_SENSOR_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,        /* let the driver pick 1/2/3 */
            .trans_queue_depth = 0,    /* 0 = synchronous transactions only */
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, sensor_bus), TAG, "sensor bus alloc failed");
        ESP_LOGI(TAG, "Sensor bus  (port %d) up on SDA=%d SCL=%d",
                 I2C_SENSOR_PORT, I2C_SENSOR_SDA_PIN, I2C_SENSOR_SCL_PIN);
    }

    if (oled_bus) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_OLED_PORT,
            .sda_io_num = I2C_OLED_SDA_PIN,
            .scl_io_num = I2C_OLED_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, oled_bus), TAG, "oled bus alloc failed");
        ESP_LOGI(TAG, "Display bus (port %d) up on SDA=%d SCL=%d",
                 I2C_OLED_PORT, I2C_OLED_SDA_PIN, I2C_OLED_SCL_PIN);
    }

    return ESP_OK;
}

void i2c_bus_scan(i2c_master_bus_handle_t bus, const char *bus_name)
{
    ESP_LOGI(TAG, "Scanning %s ...", bus_name);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  no devices on %s -- check wiring / pull-ups / 3V3", bus_name);
    }
}

esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
}

esp_err_t i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_XFER_TIMEOUT_MS);
}
