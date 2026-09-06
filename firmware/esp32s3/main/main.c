#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "ble_controller.h"
#include "gy87.h"
#include "i2c_bus.h"
#include "ssd1306.h"

static const char *TAG = "main_app";

/* The control loop runs fast so steering feels direct over BLE. The barometer,
 * magnetometer, display and serial log only need a few hertz, so they run on a
 * slow sub-tick -- the BMP180 alone blocks for ~35 ms and would wreck the
 * loop's cadence if it ran every pass. */
#define CONTROL_PERIOD_MS 20                     /* 50 Hz steering            */
#define SLOW_EVERY        12                     /* ~240 ms display + baro    */

/* Tilt beyond this many degrees is full lock. Comfortable for a hand-held
 * board; lower it for a twitchier feel. */
#define STEER_RANGE_DEG   35.0f
#define PITCH_RANGE_DEG   35.0f
#define STEER_DEADZONE    0.06f

/* Complementary low-pass on the accelerometer-derived angle. The GY-87 picks
 * up plenty of hand tremor and engine-order vibration at 50 Hz. */
#define TILT_ALPHA        0.25f

typedef struct {
    float roll_deg;
    float pitch_deg;
    float roll_zero;
    float pitch_zero;
    bool zeroed;
} tilt_t;

/**
 * Standard accelerometer tilt. Using the magnitude of the other two axes in
 * the denominator keeps both angles well behaved as the board approaches
 * vertical, which atan2(ax, az) alone does not.
 */
static void tilt_update(tilt_t *t, const gy87_data_t *d)
{
    const float ax = d->ax, ay = d->ay, az = d->az;
    const float roll = atan2f(ax, sqrtf(ay * ay + az * az)) * 57.29578f;
    const float pitch = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.29578f;

    t->roll_deg += (roll - t->roll_deg) * TILT_ALPHA;
    t->pitch_deg += (pitch - t->pitch_deg) * TILT_ALPHA;
}

static void tilt_capture_zero(tilt_t *t)
{
    t->roll_zero = t->roll_deg;
    t->pitch_zero = t->pitch_deg;
    t->zeroed = true;
    ESP_LOGI(TAG, "Level reference captured: roll %.1f pitch %.1f",
             t->roll_zero, t->pitch_zero);
}

static float axis_value(float deg, float zero, float range)
{
    float v = (deg - zero) / range;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (fabsf(v) < STEER_DEADZONE) return 0.0f;
    /* Rescale so the deadzone does not create a step at its edge. */
    return (v - copysignf(STEER_DEADZONE, v)) / (1.0f - STEER_DEADZONE);
}

static void draw_status(ssd1306_t *oled, const tilt_t *t, float steer,
                        const gy87_data_t *d, const gy87_t *gy87)
{
    ssd1306_clear(oled);
    ssd1306_text(oled, 0, 0, "IMU RACER CTRL");

    if (ble_controller_is_streaming()) {
        ssd1306_text(oled, 1, 0, "BLE: streaming");
    } else if (ble_controller_is_connected()) {
        ssd1306_text(oled, 1, 0, "BLE: connected");
    } else {
        ssd1306_text(oled, 1, 0, "BLE: advertising");
    }

    ssd1306_printf(oled, 3, 0, "STEER %+5.2f", steer);

    /* A little bar so you can see the steering without the game running. */
    const int cx = 64;
    const int half = 58;
    for (int x = cx - half; x <= cx + half; x++) {
        ssd1306_pixel(oled, x, 38, true);
    }
    const int px = cx + (int)(steer * half);
    for (int y = 34; y <= 42; y++) {
        ssd1306_pixel(oled, px, y, true);
        ssd1306_pixel(oled, px - 1, y, true);
    }

    ssd1306_printf(oled, 6, 0, "R%+4.0f P%+4.0f%s",
                   t->roll_deg - t->roll_zero,
                   t->pitch_deg - t->pitch_zero,
                   t->zeroed ? "" : " ?");

    if (gy87->baro_present) {
        ssd1306_printf(oled, 7, 0, "%7.1fPa %4.1fC", d->pressure, d->temp_mpu);
    } else {
        ssd1306_printf(oled, 7, 0, "T %4.1fC", d->temp_mpu);
    }

    ssd1306_flush(oled);
}

void app_main(void)
{
    i2c_master_bus_handle_t sensor_bus = NULL;
    i2c_master_bus_handle_t oled_bus = NULL;
    ESP_ERROR_CHECK(i2c_buses_init(&sensor_bus, &oled_bus));

    i2c_bus_scan(sensor_bus, "sensor bus");
    i2c_bus_scan(oled_bus, "display bus");

    static ssd1306_t oled;
    bool oled_ok = (ssd1306_init(oled_bus, &oled) == ESP_OK);
    if (oled_ok) {
        ssd1306_text(&oled, 0, 0, "IMU RACER CTRL");
        ssd1306_text(&oled, 2, 0, "starting...");
        ssd1306_flush(&oled);
    } else {
        ESP_LOGE(TAG, "OLED init failed -- continuing without display");
    }

    static gy87_t gy87;
    bool imu_ok = (gy87_init(sensor_bus, &gy87) == ESP_OK);
    if (!imu_ok) {
        ESP_LOGE(TAG, "GY-87 init failed -- check wiring on SDA=%d SCL=%d",
                 I2C_SENSOR_SDA_PIN, I2C_SENSOR_SCL_PIN);
    }

    ESP_ERROR_CHECK(ble_controller_init("IMU Racer"));

    gy87_data_t d = { 0 };
    tilt_t tilt = { 0 };
    float steer = 0.0f;
    float pitch = 0.0f;
    uint32_t tick = 0;

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (imu_ok && gy87_read_motion(&gy87, &d) == ESP_OK) {
            tilt_update(&tilt, &d);

            /* Give the filter a moment to settle before latching level, so a
             * board still being put down does not become the reference. */
            if (!tilt.zeroed && tick > 40) {
                tilt_capture_zero(&tilt);
            }
            if (ble_controller_take_zero_request()) {
                tilt_capture_zero(&tilt);
            }

            steer = axis_value(tilt.roll_deg, tilt.roll_zero, STEER_RANGE_DEG);
            pitch = axis_value(tilt.pitch_deg, tilt.pitch_zero, PITCH_RANGE_DEG);
        }

        ble_controller_publish(steer, pitch, 0, tilt.zeroed);

        if (tick % SLOW_EVERY == 0) {
            ble_controller_maintain();
            if (imu_ok && gy87.baro_present) {
                gy87_read(&gy87, &d);   /* full pass, refreshes baro and mag */
            }
            if (oled_ok) {
                draw_status(&oled, &tilt, steer, &d, &gy87);
            }
        }

        if (tick % (SLOW_EVERY * 10) == 0) {
            ESP_LOGI(TAG, "steer %+.2f pitch %+.2f | accel %.2f %.2f %.2f | BLE %s",
                     steer, pitch, d.ax, d.ay, d.az,
                     ble_controller_is_streaming() ? "streaming"
                     : ble_controller_is_connected() ? "connected" : "advertising");
        }

        tick++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}
