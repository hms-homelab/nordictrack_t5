/**
 * @file mqtt_manager.h
 * @brief MQTT client with Home Assistant discovery for treadmill telemetry.
 *
 * Connects to a broker, publishes Home Assistant MQTT discovery configs
 * once on connect, then publishes telemetry samples at the poll interval.
 */

#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "esp_err.h"
#include "ble_treadmill.h"  /* treadmill_telemetry_t */
#include <stdbool.h>

/**
 * @brief Initialize and start the MQTT client.
 *
 * @param host   Broker hostname or IP.
 * @param port   Broker port (e.g. 1883).
 * @param user   Username (NULL or "" if none).
 * @param pass   Password (NULL or "" if none).
 * @param prefix Topic prefix (e.g. "treadmill") for telemetry and discovery.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t mqtt_manager_init(const char *host, int port,
                            const char *user, const char *pass,
                            const char *prefix);

/**
 * @brief Publish a telemetry sample to the state topic(s).
 * @param t  Telemetry to publish (must be non-NULL).
 */
void mqtt_manager_publish_telemetry(const treadmill_telemetry_t *t);

/**
 * @brief Publish Home Assistant MQTT discovery configs for all entities.
 *        Call once after the broker connection is established.
 */
void mqtt_manager_publish_discovery(void);

/**
 * @brief Check if the MQTT client is currently connected to the broker.
 */
bool mqtt_manager_is_connected(void);

#endif /* MQTT_MANAGER_H */
