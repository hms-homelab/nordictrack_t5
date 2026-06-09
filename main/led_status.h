/**
 * @file led_status.h
 * @brief Status LED state machine on CONFIG_LED_GPIO.
 *
 * Maps a coarse device state to an LED pattern (driven by a timer/task
 * inside the implementation): solid, blink, fast-blink, etc.
 */

#ifndef LED_STATUS_H
#define LED_STATUS_H

/**
 * Coarse device states reflected on the status LED.
 */
typedef enum {
    LED_PROV,        /* Provisioning / waiting for config */
    LED_CONNECTING,  /* Connecting WiFi / MQTT / treadmill BLE */
    LED_STREAMING,   /* Connected and publishing telemetry */
    LED_ERROR        /* Fault condition */
} led_state_t;

/**
 * Initialize the status LED GPIO and the pattern driver.
 */
void led_status_init(void);

/**
 * Set the current LED state (changes the displayed pattern).
 * @param state  new state.
 */
void led_status_set(led_state_t state);

#endif /* LED_STATUS_H */
