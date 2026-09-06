#include "racer_app.h"

#include "gy87.h"
#include "i2c_bus.h"
#include "link_serial.h"
#include "max7219.h"
#include "ssd1306.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The control loop runs fast so steering feels direct over the USB link. The
 * barometer, magnetometer, display and serial log only need a few hertz, so
 * they run on a slow sub-tick. */
#define CONTROL_PERIOD_MS 20                     /* 50 Hz steering            */
#define SLOW_EVERY        12                     /* ~240 ms redraw + baro     */
#define LOG_EVERY         (SLOW_EVERY * 10)      /* ~2.4 s console line       */

/* Tilt beyond this many degrees is full lock. Comfortable for a hand-held
 * board; lower it for a twitchier feel. */
#define STEER_RANGE_DEG   35.0f
#define PITCH_RANGE_DEG   35.0f
#define STEER_DEADZONE    0.06f

/* Complementary low-pass on the accelerometer-derived angle. The GY-87 picks
 * up plenty of hand tremor and engine-order vibration at 50 Hz. */
#define TILT_ALPHA        0.25f

/* User button: a short press rotates the mounting frame, holding it recaptures
 * level. 800 ms is long enough that a deliberate rotation never triggers it. */
#define BUTTON_DEBOUNCE_TICKS  2     /*  40 ms */
#define BUTTON_LONG_TICKS      40    /* 800 ms */

/* The LED matrix is a spirit level, so its full scale is tighter than the
 * steering range: 20 degrees over 8 pixels is 5 degrees per pixel, fine enough
 * to actually level the board by. */
#define BUBBLE_RANGE_DEG       20.0f
#define BUBBLE_LEVEL_DEG       2.0f   /* inside this, show the centre block */
#define BUBBLE_BLINK_TICKS     12     /* fault border, ~240 ms per phase    */
#define BUBBLE_DIM             1      /* no host listening */
#define BUBBLE_BRIGHT          7      /* streaming         */

#define DEVICE_NAME       "IMU Racer STM32H7"

typedef struct {
  float roll_deg;
  float pitch_deg;
  float roll_zero;
  float pitch_zero;
  bool  zeroed;
} tilt_t;

static ssd1306_t   s_oled;
static max7219_t   s_matrix;
static gy87_t      s_gy87;
static gy87_data_t s_data;
static tilt_t      s_tilt;

static bool     s_oled_ok;
static bool     s_matrix_ok;
static bool     s_imu_ok;
static uint32_t s_imu_err_streak;   /* consecutive failed reads, 0 when healthy */
static uint32_t s_imu_err_total;
static float    s_steer;
static float    s_pitch;
static uint32_t s_tick;
static uint32_t s_next_tick;
static bool     s_zero_request;

/* Which way the GY-87 ended up mounted, in 90-degree steps around Z. Cycled
 * from the user button, so the same firmware handles any of the four ways the
 * module can sit without re-flashing. */
static uint8_t s_orientation;
static bool    s_tilt_snap;   /* next sample replaces the filter outright */

static const uint16_t ORIENTATION_DEG[4] = { 0, 90, 180, 270 };

/* Debounced button state, sampled on the control tick. */
static uint8_t  s_btn_level;
static uint8_t  s_btn_samples;
static uint32_t s_btn_held_ticks;
static bool     s_btn_long_fired;

/* -------------------------------------------------------------------------- */
/*                                   Helpers                                  */
/* -------------------------------------------------------------------------- */

/*
 * newlib-nano leaves the floating point formatter out of printf unless the
 * linker is told to pull it in, so nothing here passes a float to printf or
 * snprintf. Numbers are split into integer and fractional parts by hand.
 */
static void fmt_2dp(char *buf, size_t n, float v)
{
  int32_t t = (int32_t)(v * 100.0f + (v < 0.0f ? -0.5f : 0.5f));
  char sign = (t < 0) ? '-' : '+';

  if (t < 0)
  {
    t = -t;
  }
  snprintf(buf, n, "%c%ld.%02ld", sign, (long)(t / 100), (long)(t % 100));
}

static void fmt_1dp(char *buf, size_t n, float v)
{
  int32_t t = (int32_t)(v * 10.0f + (v < 0.0f ? -0.5f : 0.5f));
  char sign = (t < 0) ? '-' : '+';

  if (t < 0)
  {
    t = -t;
  }
  snprintf(buf, n, "%c%ld.%01ld", sign, (long)(t / 10), (long)(t % 10));
}

static long round_to_long(float v)
{
  return (long)(v + (v < 0.0f ? -0.5f : 0.5f));
}

/*
 * A failed read leaves the previous sample in place, so without this the tilt
 * simply freezes and the display keeps refreshing as if nothing happened. Say
 * so, on the console and on the panel, rather than showing stale numbers as
 * though they were live.
 */
static void note_imu_error(void)
{
  const char *why = i2c_bus_error_name(i2c_bus_last_error());

  s_imu_err_total++;
  /* First failure, then once a second: a dead bus fails every tick and would
   * otherwise bury the log under 50 identical lines per second. */
  if (s_imu_err_streak % 50U == 0U)
  {
    printf("app: MPU6050 read failed (%lu total), I2C %s\r\n",
           (unsigned long)s_imu_err_total, why);
  }
  s_imu_err_streak++;
}

static void note_imu_ok(void)
{
  if (s_imu_err_streak != 0U)
  {
    printf("app: MPU6050 responding again after %lu failed reads\r\n",
           (unsigned long)s_imu_err_streak);
    s_imu_err_streak = 0;
  }
}

/**
 * Turn the accelerometer X/Y pair by @p quadrant * 90 degrees about Z. Only
 * the horizontal pair moves: Z is the rotation axis, so gravity along it is
 * unchanged whichever way the module faces.
 */
static void rotate_xy(uint8_t quadrant, float x, float y, float *ox, float *oy)
{
  switch (quadrant & 3U)
  {
  case 1:  *ox =  y; *oy = -x; break;   /*  90 deg */
  case 2:  *ox = -x; *oy = -y; break;   /* 180 deg */
  case 3:  *ox = -y; *oy =  x; break;   /* 270 deg */
  default: *ox =  x; *oy =  y; break;   /*   0 deg */
  }
}

/**
 * Standard accelerometer tilt. Using the magnitude of the other two axes in
 * the denominator keeps both angles well behaved as the board approaches
 * vertical, which atan2(ax, az) alone does not.
 */
static void tilt_update(tilt_t *t, const gy87_data_t *d, uint8_t orientation)
{
  float ax, ay;

  rotate_xy(orientation, d->ax, d->ay, &ax, &ay);

  const float az = d->az;
  const float roll = atan2f(ax, sqrtf(ay * ay + az * az)) * 57.29578f;
  const float pitch = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.29578f;

  if (s_tilt_snap)
  {
    /* A rotation moves both angles a long way in one step. Filtering that
     * transition would only add a second of visible drift towards a value we
     * already know exactly. */
    t->roll_deg = roll;
    t->pitch_deg = pitch;
    s_tilt_snap = false;
    return;
  }

  t->roll_deg += (roll - t->roll_deg) * TILT_ALPHA;
  t->pitch_deg += (pitch - t->pitch_deg) * TILT_ALPHA;
}

static void tilt_capture_zero(tilt_t *t)
{
  char r[12], p[12];

  t->roll_zero = t->roll_deg;
  t->pitch_zero = t->pitch_deg;
  t->zeroed = true;

  fmt_1dp(r, sizeof(r), t->roll_zero);
  fmt_1dp(p, sizeof(p), t->pitch_zero);
  printf("app: level reference captured: roll %s pitch %s\r\n", r, p);
}

static float axis_value(float deg, float zero, float range)
{
  float v = (deg - zero) / range;

  if (v > 1.0f)
  {
    v = 1.0f;
  }
  if (v < -1.0f)
  {
    v = -1.0f;
  }
  if (fabsf(v) < STEER_DEADZONE)
  {
    return 0.0f;
  }
  /* Rescale so the deadzone does not create a step at its edge. */
  return (v - copysignf(STEER_DEADZONE, v)) / (1.0f - STEER_DEADZONE);
}

static void rotate_orientation(void)
{
  s_orientation = (uint8_t)((s_orientation + 1U) & 3U);

  /* The level reference was captured in the old frame, so it describes nothing
   * now. Snap the filter onto the new axes and re-zero from that same sample,
   * otherwise the car would pull hard to one side until the user re-levelled
   * by hand. */
  s_tilt_snap = true;
  s_zero_request = true;

  printf("app: steer orientation now %u deg\r\n",
         (unsigned)ORIENTATION_DEG[s_orientation]);
}

/**
 * Short press rotates the frame 90 degrees, holding it recaptures level.
 *
 * Both need the press *duration*, and the BSP EXTI callback only sees the
 * rising edge, so the button is polled here on the same 20 ms tick as
 * everything else. At 50 Hz a two-sample debounce is already 40 ms, well past
 * the contact bounce on this switch.
 */
static void button_poll(void)
{
  uint8_t raw = (BSP_PB_GetState(BUTTON_USER) != 0) ? 1U : 0U;

  if (raw != s_btn_level)
  {
    s_btn_samples++;
    if (s_btn_samples < BUTTON_DEBOUNCE_TICKS)
    {
      return;
    }
    s_btn_level = raw;
    s_btn_samples = 0;

    if (raw != 0U)
    {
      s_btn_held_ticks = 0;
      s_btn_long_fired = false;
    }
    else if (!s_btn_long_fired)
    {
      /* Released before the hold timeout, so it was a tap. */
      rotate_orientation();
    }
    return;
  }

  s_btn_samples = 0;

  if (s_btn_level != 0U)
  {
    s_btn_held_ticks++;
    /* Fire while the button is still down: the OLED updates immediately, which
     * is the feedback that tells the user to let go. */
    if (!s_btn_long_fired && s_btn_held_ticks >= BUTTON_LONG_TICKS)
    {
      s_btn_long_fired = true;
      s_zero_request = true;
    }
  }
}

/* -------------------------------------------------------------------------- */
/*                                   Display                                  */
/* -------------------------------------------------------------------------- */

static void draw_status(void)
{
  char num[16];

  ssd1306_clear(&s_oled);
  ssd1306_text(&s_oled, 0, 0, "IMU RACER STM32");
  ssd1306_text(&s_oled, 1, 0,
               link_serial_host_present() ? "USB: streaming" : "USB: waiting");

  if (!s_imu_ok)
  {
    ssd1306_text(&s_oled, 2, 0, "IMU: absent");
  }
  else if (s_imu_err_streak != 0U)
  {
    ssd1306_printf(&s_oled, 2, 0, "IMU: read err %lu", (unsigned long)s_imu_err_streak);
  }
  else if (s_imu_err_total != 0U)
  {
    ssd1306_printf(&s_oled, 2, 0, "IMU: ok (%lu errs)", (unsigned long)s_imu_err_total);
  }
  else
  {
    ssd1306_text(&s_oled, 2, 0, "IMU: ok");
  }

  fmt_2dp(num, sizeof(num), s_steer);
  ssd1306_printf(&s_oled, 3, 0, "STEER %s", num);
  ssd1306_printf(&s_oled, 3, 78, "ROT%3u", (unsigned)ORIENTATION_DEG[s_orientation]);

  /* A little bar so you can see the steering without the game running. */
  const int cx = 64;
  const int half = 58;

  for (int x = cx - half; x <= cx + half; x++)
  {
    ssd1306_pixel(&s_oled, x, 38, true);
  }

  const int px = cx + (int)(s_steer * (float)half);

  for (int y = 34; y <= 42; y++)
  {
    ssd1306_pixel(&s_oled, px, y, true);
    ssd1306_pixel(&s_oled, px - 1, y, true);
  }

  ssd1306_printf(&s_oled, 6, 0, "R%+4ld P%+4ld%s",
                 round_to_long(s_tilt.roll_deg - s_tilt.roll_zero),
                 round_to_long(s_tilt.pitch_deg - s_tilt.pitch_zero),
                 s_tilt.zeroed ? "" : " ?");

  fmt_1dp(num, sizeof(num), s_data.temp_mpu);
  if (s_gy87.baro_present)
  {
    ssd1306_printf(&s_oled, 7, 0, "%7ldPa %sC",
                   round_to_long(s_data.pressure), num);
  }
  else
  {
    ssd1306_printf(&s_oled, 7, 0, "T %sC", num);
  }
}

/* -------------------------------------------------------------------------- */
/*                                 Bubble level                               */
/* -------------------------------------------------------------------------- */

/** Map one axis, in degrees away from level, onto a 0..7 pixel coordinate. */
static int bubble_axis(float rel_deg)
{
  float v = rel_deg / BUBBLE_RANGE_DEG;

  if (v < -1.0f)
  {
    v = -1.0f;
  }
  if (v > 1.0f)
  {
    v = 1.0f;
  }

  int p = (int)(3.5f + v * 3.5f + 0.5f);

  if (p < 0)
  {
    p = 0;
  }
  if (p > MAX7219_SIZE - 1)
  {
    p = MAX7219_SIZE - 1;
  }
  return p;
}

static void bubble_border(void)
{
  for (int i = 0; i < MAX7219_SIZE; i++)
  {
    max7219_pixel(&s_matrix, i, 0, true);
    max7219_pixel(&s_matrix, i, MAX7219_SIZE - 1, true);
    max7219_pixel(&s_matrix, 0, i, true);
    max7219_pixel(&s_matrix, MAX7219_SIZE - 1, i, true);
  }
}

/**
 * A spirit level: one lit pixel showing where the board is relative to its
 * captured level reference.
 *
 * The angles come from the same filter the steering uses, so the frame the
 * user button rotates applies here too -- tilt left, the dot goes left, which
 * is how you can tell at a glance whether ROT is set correctly.
 *
 * The raw angles are used rather than the steering values, because those carry
 * a deadzone: feeding them here would blank out the middle of the grid, which
 * is exactly the part you need while levelling.
 */
static void draw_bubble(void)
{
  max7219_clear(&s_matrix);

  /* Brightness carries the link state, so no pixel is spent on it and the
   * dot stays legible at full deflection. */
  max7219_set_intensity(&s_matrix,
                        link_serial_host_present() ? BUBBLE_BRIGHT : BUBBLE_DIM);

  if (!s_imu_ok || s_imu_err_streak != 0U)
  {
    /* Stale angles. Blink the border and show no dot at all: a dot sitting
     * still would read as "the board is level", which is a lie. */
    if (((s_tick / BUBBLE_BLINK_TICKS) & 1U) != 0U)
    {
      bubble_border();
    }
    max7219_flush(&s_matrix);
    return;
  }

  const float roll_rel = s_tilt.roll_deg - s_tilt.roll_zero;
  const float pitch_rel = s_tilt.pitch_deg - s_tilt.pitch_zero;

  if (fabsf(roll_rel) < BUBBLE_LEVEL_DEG && fabsf(pitch_rel) < BUBBLE_LEVEL_DEG)
  {
    /* Level. A 2x2 block instead of a single pixel, because with an even
     * number of columns there is no true centre pixel to sit on. */
    max7219_pixel(&s_matrix, 3, 3, true);
    max7219_pixel(&s_matrix, 4, 3, true);
    max7219_pixel(&s_matrix, 3, 4, true);
    max7219_pixel(&s_matrix, 4, 4, true);
  }
  else
  {
    /* Pitch is negated so that nosing the board away from you moves the dot
     * up the panel, the way a real bubble runs uphill. */
    max7219_pixel(&s_matrix, bubble_axis(roll_rel), bubble_axis(-pitch_rel), true);
  }

  max7219_flush(&s_matrix);
}

/* -------------------------------------------------------------------------- */
/*                                    Setup                                   */
/* -------------------------------------------------------------------------- */

void racer_app_init(void)
{
  /* Unbuffered, so a log line reaches the VCP before the next I2C transfer
   * rather than sitting in newlib for a while. */
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("\r\n--- IMU Racer controller, STM32H7A3ZI-Q ---\r\n");

  if (i2c_bus_init() != HAL_OK)
  {
    printf("app: I2C4 re-init at 400 kHz failed\r\n");
    Error_Handler();
  }
  i2c_bus_scan();

  s_oled_ok = (ssd1306_init(&s_oled) == HAL_OK);
  if (s_oled_ok)
  {
    ssd1306_text(&s_oled, 0, 0, "IMU RACER STM32");
    ssd1306_text(&s_oled, 2, 0, "starting...");
    ssd1306_flush(&s_oled);
  }
  else
  {
    printf("app: OLED init failed -- continuing without display\r\n");
  }

  s_matrix_ok = (max7219_init(&s_matrix, BUBBLE_DIM) == HAL_OK);
  if (!s_matrix_ok)
  {
    printf("app: MAX7219 init failed -- continuing without the bubble level\r\n");
  }

  s_imu_ok = (gy87_init(&s_gy87) == HAL_OK);
  if (!s_imu_ok)
  {
    printf("app: GY-87 init failed -- check SDA=PF15 SCL=PF14, 3V3 and pull-ups\r\n");
    BSP_LED_On(LED_RED);
  }

  link_serial_init(DEVICE_NAME);

  s_next_tick = HAL_GetTick();
}

/* -------------------------------------------------------------------------- */
/*                                 Control loop                               */
/* -------------------------------------------------------------------------- */

void racer_app_run(void)
{
  uint32_t now = HAL_GetTick();

  if ((int32_t)(now - s_next_tick) < 0)
  {
    return;
  }
  s_next_tick += CONTROL_PERIOD_MS;
  /* If something blocked long enough to miss a whole tick, resynchronise
   * instead of trying to catch up with a burst of back-to-back passes. */
  if ((int32_t)(HAL_GetTick() - s_next_tick) > 0)
  {
    s_next_tick = HAL_GetTick() + CONTROL_PERIOD_MS;
  }

  button_poll();

  if (s_imu_ok)
  {
    if (gy87_read_motion(&s_gy87, &s_data) != HAL_OK)
    {
      /* Steering holds its last value rather than snapping to centre: one
       * dropped read on a shared bus should not jerk the car. */
      note_imu_error();
    }
    else
    {
      note_imu_ok();
      tilt_update(&s_tilt, &s_data, s_orientation);

      /* Give the filter a moment to settle before latching level, so a board
       * still being put down does not become the reference. */
      if (!s_tilt.zeroed && s_tick > 40)
      {
        tilt_capture_zero(&s_tilt);
      }
      /* The request latches, so one made while the bus was down is honoured
       * on the first good sample instead of being lost. */
      if (link_serial_take_zero_request() || s_zero_request)
      {
        s_zero_request = false;
        tilt_capture_zero(&s_tilt);
      }

      s_steer = axis_value(s_tilt.roll_deg, s_tilt.roll_zero, STEER_RANGE_DEG);
      s_pitch = axis_value(s_tilt.pitch_deg, s_tilt.pitch_zero, PITCH_RANGE_DEG);
    }
  }

  link_serial_publish(s_steer, s_pitch, s_btn_level, s_tilt.zeroed);
  link_serial_poll();

  /* LD1 green: a browser tab is listening. LD2 yellow: level captured.
   * LD3 red: no IMU, lit once at startup. */
  if (link_serial_host_present())
  {
    BSP_LED_On(LED_GREEN);
  }
  else
  {
    BSP_LED_Off(LED_GREEN);
  }
  if (s_tilt.zeroed)
  {
    BSP_LED_On(LED_YELLOW);
  }

  /* One page of the display per tick: a full refresh costs ~24 ms on this
   * bus, which is more than a whole control period. */
  if (s_oled_ok)
  {
    ssd1306_flush_step(&s_oled);
  }

  /* The matrix is redrawn whole every tick instead: 16 bytes on a bus of its
   * own is ~16 us, and a bubble that lags is worse than useless. */
  if (s_matrix_ok)
  {
    draw_bubble();
  }

  if (s_imu_ok)
  {
    gy87_baro_step(&s_gy87, &s_data);
  }

  if (s_tick % SLOW_EVERY == 0)
  {
    if (s_imu_ok && s_gy87.mag_type != MAG_NONE)
    {
      gy87_read_mag(&s_gy87, &s_data);
    }
    if (s_oled_ok)
    {
      draw_status();
    }
  }

  if (s_tick % LOG_EVERY == 0)
  {
    char st[12], pi[12], ax[12], ay[12], az[12];

    fmt_2dp(st, sizeof(st), s_steer);
    fmt_2dp(pi, sizeof(pi), s_pitch);
    fmt_2dp(ax, sizeof(ax), s_data.ax);
    fmt_2dp(ay, sizeof(ay), s_data.ay);
    fmt_2dp(az, sizeof(az), s_data.az);
    printf("app: steer %s pitch %s | accel %s %s %s | rot %u | host %s\r\n",
           st, pi, ax, ay, az,
           (unsigned)ORIENTATION_DEG[s_orientation],
           link_serial_host_present() ? "present" : "absent");
  }

  s_tick++;
}
