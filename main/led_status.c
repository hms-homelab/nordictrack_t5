/**
 * @file led_status.c
 * @brief Status LED state machine on CONFIG_LED_GPIO.
 *
 * A single periodic esp_timer ticks at a fixed base rate; each state defines
 * a short on/off pattern table that is replayed cyclically. This lets us do
 * slow blink (PROV), fast blink (CONNECTING), a steady heartbeat (STREAMING),
 * and a double-blink (ERROR) without per-state timers.
 */

#include "led_status.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define LED_PIN          CONFIG_LED_GPIO

/* Base tick: each pattern frame is this long. 100 ms gives us enough
 * resolution for fast blink while keeping the slow patterns reasonable. */
#define LED_TICK_US      (100 * 1000)

static esp_timer_handle_t s_timer = NULL;
static led_state_t        s_state = LED_PROV;
static uint8_t            s_step  = 0;

/* Each pattern is a frame table of on(1)/off(0) levels, one frame per tick.
 * Patterns repeat once the step index wraps past the table length. */
typedef struct {
    const uint8_t *frames;
    uint8_t        len;
} led_pattern_t;

/* PROV: slow blink (1 s on, 1 s off) */
static const uint8_t pat_prov[]       = { 1,1,1,1,1, 0,0,0,0,0 };
/* CONNECTING: fast blink (200 ms on, 200 ms off) */
static const uint8_t pat_connecting[] = { 1,1, 0,0 };
/* STREAMING: solid with a brief heartbeat dip (mostly on) */
static const uint8_t pat_streaming[]  = { 1,1,1,1,1,1,1,1,1, 0 };
/* ERROR: double-blink then a longer pause */
static const uint8_t pat_error[]      = { 1, 0, 1, 0, 0,0,0,0,0,0 };

static led_pattern_t pattern_for(led_state_t state)
{
    switch (state) {
        case LED_CONNECTING:
            return (led_pattern_t){ pat_connecting, sizeof(pat_connecting) };
        case LED_STREAMING:
            return (led_pattern_t){ pat_streaming, sizeof(pat_streaming) };
        case LED_ERROR:
            return (led_pattern_t){ pat_error, sizeof(pat_error) };
        case LED_PROV:
        default:
            return (led_pattern_t){ pat_prov, sizeof(pat_prov) };
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    led_pattern_t p = pattern_for(s_state);
    if (s_step >= p.len) {
        s_step = 0;
    }
    gpio_set_level(LED_PIN, p.frames[s_step]);
    s_step++;
    if (s_step >= p.len) {
        s_step = 0;
    }
}

void led_status_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_PIN, 0);

    if (s_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = tick_cb,
            .name     = "led_status",
        };
        esp_timer_create(&timer_args, &s_timer);
    }

    s_state = LED_PROV;
    s_step  = 0;
    esp_timer_start_periodic(s_timer, LED_TICK_US);
}

void led_status_set(led_state_t state)
{
    if (state == s_state) {
        return;
    }
    s_state = state;
    s_step  = 0;
    /* Apply the first frame immediately so the change is visible without
     * waiting for the next tick. */
    led_pattern_t p = pattern_for(s_state);
    if (p.len > 0) {
        gpio_set_level(LED_PIN, p.frames[0]);
        s_step = 1;
        if (s_step >= p.len) {
            s_step = 0;
        }
    }
}
