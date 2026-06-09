/**
 * @file ble_treadmill.h
 * @brief BLE GATT client for the NordicTrack/iFit "I_TL" treadmill.
 *
 * Scans for, connects to, and subscribes to the treadmill's telemetry
 * characteristic. Notifications are copied (bytes only) into a FreeRTOS
 * queue inside the GATTC callback and parsed on a worker task — NEVER
 * parse in the notify callback. The latest decoded sample is cached and
 * read out via ble_treadmill_get().
 */

#ifndef BLE_TREADMILL_H
#define BLE_TREADMILL_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Latest decoded treadmill telemetry sample.
 */
typedef struct {
    float    speed_kmh;    /* Belt speed in km/h */
    float    incline_pct;  /* Incline grade as a percentage */
    uint32_t elapsed_s;    /* Elapsed workout time in seconds */
    bool     moving;       /* true if the belt is currently moving */
    bool     connected;    /* true if the BLE link is up */
} treadmill_telemetry_t;

/**
 * Initialize the BLE GATTC stack, start scanning, and begin the worker
 * task that parses notifications. Assumes Bluedroid is already enabled.
 *
 * @return ESP_OK on success.
 */
esp_err_t ble_treadmill_init(void);

/**
 * Copy the latest cached telemetry sample into *out.
 *
 * @param out  destination struct (must be non-NULL).
 * @return true if a valid sample was copied, false if none available yet.
 */
bool ble_treadmill_get(treadmill_telemetry_t *out);

/**
 * @return true if the GATT client is currently connected to the treadmill.
 */
bool ble_treadmill_is_connected(void);

#endif /* BLE_TREADMILL_H */
