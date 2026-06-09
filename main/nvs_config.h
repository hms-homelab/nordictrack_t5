/**
 * @file nvs_config.h
 * @brief NVS persistent configuration for the ESP32-S3 treadmill bridge.
 *
 * Stores WiFi credentials, MQTT broker settings, topic prefix, and the
 * operating mode (PROV vs RUN). Written during provisioning (BLE GATTS or
 * captive portal), read on every boot to decide the boot path.
 */

#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Operating mode persisted in NVS.
 * Determines the boot path in main.c.
 */
typedef enum {
    OP_MODE_PROV = 0,  /* Unprovisioned: GATT server + SoftAP captive portal */
    OP_MODE_RUN  = 1   /* Provisioned: GATT client + WiFi + MQTT */
} op_mode_t;

/**
 * Initialize NVS flash and open the "treadmill" namespace.
 * Must be called once at boot before any other nvs_config_* call.
 */
void nvs_config_init(void);

/* WiFi credentials (key: wifi_ssid, wifi_pass) */
bool nvs_config_get_wifi_ssid(char *buf, size_t buf_size);
bool nvs_config_get_wifi_pass(char *buf, size_t buf_size);
void nvs_config_set_wifi(const char *ssid, const char *pass);
bool nvs_config_has_wifi(void);
void nvs_config_clear_wifi(void);

/* MQTT broker host (key: mqtt_host) */
bool nvs_config_get_mqtt_host(char *buf, size_t buf_size);
void nvs_config_set_mqtt_host(const char *host);
bool nvs_config_has_mqtt_host(void);

/* MQTT broker port (key: mqtt_port). Returns default_port if unset. */
int  nvs_config_get_mqtt_port(int default_port);
void nvs_config_set_mqtt_port(int port);

/* MQTT credentials (key: mqtt_user, mqtt_pass) */
bool nvs_config_get_mqtt_user(char *buf, size_t buf_size);
bool nvs_config_get_mqtt_pass(char *buf, size_t buf_size);
void nvs_config_set_mqtt_creds(const char *user, const char *pass);

/* MQTT topic prefix (key: topic_prefix). Returns default if unset. */
bool nvs_config_get_topic_prefix(char *buf, size_t buf_size);
void nvs_config_set_topic_prefix(const char *prefix);

/* Operating mode (key: op_mode) */
op_mode_t nvs_config_get_op_mode(void);
void nvs_config_set_op_mode(op_mode_t mode);

/* Erase all keys in the namespace (factory reset -> back to PROV). */
void nvs_config_clear(void);

#endif /* NVS_CONFIG_H */
