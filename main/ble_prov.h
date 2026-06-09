/**
 * @file ble_prov.h
 * @brief BLE GATT server for treadmill bridge provisioning (PROV mode).
 *
 * Advertises during PROV mode. A client (phone app or generic BLE tool)
 * connects to read device info and write WiFi + MQTT configuration as a
 * JSON blob. On a valid config the values are stored in NVS, op_mode is
 * set to OP_MODE_RUN, and the device reboots into RUN mode.
 *
 * Runs as a GATTS app profile alongside any GATTC use.
 */

#ifndef BLE_PROV_H
#define BLE_PROV_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* Custom 16-bit UUIDs in the Bluetooth Base UUID range */
#define BLE_PROV_SVC_UUID           0xCE00
#define BLE_PROV_CHAR_INFO_UUID     0xCE01
#define BLE_PROV_CHAR_FW_UUID       0xCE02
#define BLE_PROV_CHAR_CONFIG_UUID   0xCE03
#define BLE_PROV_CHAR_STATUS_UUID   0xCE04

/* GATTS app profile ID */
#define BLE_PROV_APP_ID             1

/* Config status notification values */
#define BLE_PROV_STATUS_RECEIVED        0x01
#define BLE_PROV_STATUS_WIFI_CONNECTING 0x02
#define BLE_PROV_STATUS_WIFI_CONNECTED  0x03
#define BLE_PROV_STATUS_WIFI_FAILED     0x04
#define BLE_PROV_STATUS_READY           0x05

/**
 * Initialize the BLE provisioning GATT server.
 * Starts Bluedroid if not already running, registers the GATTS profile,
 * creates the service and characteristics, and begins advertising.
 *
 * @param bluedroid_already_init  true if the BT/Bluedroid stack was
 *                                already enabled by another module.
 * @return ESP_OK on success.
 */
esp_err_t ble_prov_init(bool bluedroid_already_init);

/**
 * Stop advertising and unregister the GATTS app.
 * Safe to call if not initialized.
 */
void ble_prov_stop(void);

/**
 * @return true if the provisioning server is advertising.
 */
bool ble_prov_is_active(void);

/**
 * Parse a config JSON string and store the values in NVS.
 * Exposed for unit testing.
 *
 * @param json_str  JSON string (length is explicit, no NUL required).
 * @param len       length of json_str.
 * @return true on success.
 */
bool ble_prov_parse_config(const char *json_str, size_t len);

#endif /* BLE_PROV_H */
