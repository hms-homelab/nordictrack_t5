/**
 * @file wifi_manager.h
 * @brief WiFi station manager for RUN mode (connects to home WiFi).
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief WiFi connection status.
 */
typedef enum {
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_ERROR
} wifi_status_t;

/**
 * @brief Initialize the WiFi manager (netif, event loop, STA mode).
 *
 * Also applies CONFIG_TX_POWER_QDBM via esp_wifi_set_max_tx_power()
 * after esp_wifi_start().
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to a WiFi network (blocking up to timeout_ms).
 * @param ssid       WiFi SSID.
 * @param password   WiFi password.
 * @param timeout_ms Connection timeout in milliseconds.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout, error otherwise.
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief Disconnect from the current WiFi network.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Get the current WiFi connection status.
 */
wifi_status_t wifi_manager_get_status(void);

/**
 * @brief Check if WiFi is connected.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Block until connected or timeout.
 * @param timeout_ms Timeout in milliseconds.
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT on timeout, error otherwise.
 */
esp_err_t wifi_manager_wait_connection(uint32_t timeout_ms);

/**
 * @brief Get the current IP address as a string.
 * @param buf      Output buffer (at least 16 bytes).
 * @param buf_size Size of buffer.
 * @return true if an IP was written, false if not connected.
 */
bool wifi_manager_get_ip_str(char *buf, size_t buf_size);

/**
 * @brief Deinitialize the WiFi manager.
 */
void wifi_manager_deinit(void);

#endif /* WIFI_MANAGER_H */
