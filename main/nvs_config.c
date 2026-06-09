/**
 * @file nvs_config.c
 * @brief NVS persistent configuration for the ESP32-S3 treadmill bridge.
 *
 * Stores WiFi credentials, MQTT broker settings, topic prefix, and the
 * operating mode (PROV vs RUN) in the "treadmill" NVS namespace.
 */

#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nvs_cfg";
static nvs_handle_t s_nvs = 0;

/* --------------- helpers --------------- */

static bool read_str(const char *key, char *buf, size_t buf_size)
{
    if (!s_nvs || !buf || buf_size == 0) return false;
    size_t len = buf_size;
    esp_err_t ret = nvs_get_str(s_nvs, key, buf, &len);
    if (ret != ESP_OK) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

static void write_str(const char *key, const char *val)
{
    if (!s_nvs) return;
    esp_err_t ret = nvs_set_str(s_nvs, key, val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write '%s': %s", key, esp_err_to_name(ret));
        return;
    }
    nvs_commit(s_nvs);
}

/* --------------- init --------------- */

void nvs_config_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing and reinitializing");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_open("treadmill", NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "NVS config initialized (op_mode=%s)",
             nvs_config_get_op_mode() == OP_MODE_RUN ? "RUN" : "PROV");
}

/* --------------- WiFi --------------- */

bool nvs_config_get_wifi_ssid(char *buf, size_t buf_size)
{
    return read_str("wifi_ssid", buf, buf_size);
}

bool nvs_config_get_wifi_pass(char *buf, size_t buf_size)
{
    return read_str("wifi_pass", buf, buf_size);
}

void nvs_config_set_wifi(const char *ssid, const char *pass)
{
    write_str("wifi_ssid", ssid);
    write_str("wifi_pass", pass);
    ESP_LOGI(TAG, "WiFi credentials stored (SSID: %s)", ssid);
}

bool nvs_config_has_wifi(void)
{
    if (!s_nvs) return false;
    size_t len = 0;
    /* Pass NULL buffer — just checks if key exists and has non-empty value */
    esp_err_t ret = nvs_get_str(s_nvs, "wifi_ssid", NULL, &len);
    return (ret == ESP_OK && len > 1);  /* len includes null terminator */
}

void nvs_config_clear_wifi(void)
{
    if (!s_nvs) return;
    nvs_erase_key(s_nvs, "wifi_ssid");
    nvs_erase_key(s_nvs, "wifi_pass");
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "WiFi credentials cleared");
}

/* --------------- MQTT broker host --------------- */

bool nvs_config_get_mqtt_host(char *buf, size_t buf_size)
{
    return read_str("mqtt_host", buf, buf_size);
}

void nvs_config_set_mqtt_host(const char *host)
{
    write_str("mqtt_host", host);
    ESP_LOGI(TAG, "MQTT host stored: %s", host);
}

bool nvs_config_has_mqtt_host(void)
{
    if (!s_nvs) return false;
    size_t len = 0;
    esp_err_t ret = nvs_get_str(s_nvs, "mqtt_host", NULL, &len);
    return (ret == ESP_OK && len > 1);
}

/* --------------- MQTT broker port --------------- */

int nvs_config_get_mqtt_port(int default_port)
{
    if (!s_nvs) return default_port;
    uint16_t val = 0;
    esp_err_t ret = nvs_get_u16(s_nvs, "mqtt_port", &val);
    if (ret != ESP_OK || val == 0) return default_port;
    return (int)val;
}

void nvs_config_set_mqtt_port(int port)
{
    if (!s_nvs) return;
    nvs_set_u16(s_nvs, "mqtt_port", (uint16_t)port);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "MQTT port stored: %d", port);
}

/* --------------- MQTT credentials --------------- */

bool nvs_config_get_mqtt_user(char *buf, size_t buf_size)
{
    return read_str("mqtt_user", buf, buf_size);
}

bool nvs_config_get_mqtt_pass(char *buf, size_t buf_size)
{
    return read_str("mqtt_pass", buf, buf_size);
}

void nvs_config_set_mqtt_creds(const char *user, const char *pass)
{
    write_str("mqtt_user", user);
    write_str("mqtt_pass", pass);
    ESP_LOGI(TAG, "MQTT credentials stored (user: %s)", user);
}

/* --------------- Topic prefix --------------- */

bool nvs_config_get_topic_prefix(char *buf, size_t buf_size)
{
    return read_str("topic_prefix", buf, buf_size);
}

void nvs_config_set_topic_prefix(const char *prefix)
{
    write_str("topic_prefix", prefix);
    ESP_LOGI(TAG, "Topic prefix stored: %s", prefix);
}

/* --------------- Operating mode --------------- */

op_mode_t nvs_config_get_op_mode(void)
{
    if (!s_nvs) return OP_MODE_PROV;
    uint8_t val = 0;
    esp_err_t ret = nvs_get_u8(s_nvs, "op_mode", &val);
    if (ret != ESP_OK) {
        /* No explicit mode stored: default to RUN only if WiFi creds exist,
         * otherwise PROV so an unprovisioned device boots into setup. */
        return nvs_config_has_wifi() ? OP_MODE_RUN : OP_MODE_PROV;
    }
    return (val == OP_MODE_RUN) ? OP_MODE_RUN : OP_MODE_PROV;
}

void nvs_config_set_op_mode(op_mode_t mode)
{
    if (!s_nvs) return;
    nvs_set_u8(s_nvs, "op_mode", (uint8_t)mode);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "Operating mode set to %s",
             mode == OP_MODE_RUN ? "RUN" : "PROV");
}

/* --------------- Factory reset --------------- */

void nvs_config_clear(void)
{
    if (!s_nvs) return;
    nvs_erase_all(s_nvs);
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "NVS namespace cleared (factory reset -> PROV)");
}
