/**
 * @file main.c
 * @brief Treadmill S3 Bridge -- boot orchestration.
 *
 * Generic ESP32-S3 (SuperMini) BLE central to a NordicTrack/iFit "I_TL"
 * treadmill. Publishes telemetry to MQTT with Home Assistant discovery.
 *
 * Two boot paths:
 *   PROV mode -- no WiFi creds, or FORCE_PROV button held low at boot:
 *                LED_PROV, BLE GATTS provisioning + SoftAP captive portal.
 *                captive_portal_start() blocks until a valid config is saved
 *                and the device reboots into RUN mode.
 *   RUN  mode -- provisioned: LED_CONNECTING, WiFi STA connect, MQTT client,
 *                BLE GATTC to the treadmill, then a poll/publish loop.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "nvs_config.h"
#include "led_status.h"
#include "ota_handler.h"
#include "ble_prov.h"
#include "captive_portal.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ble_treadmill.h"

static const char *TAG = "MAIN";

#define WIFI_CONNECT_TIMEOUT_MS 30000

/**
 * @brief Read the force-provisioning button (CONFIG_FORCE_PROV_GPIO).
 * @return true if the button is held low at boot (request PROV mode).
 */
static bool force_prov_requested(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_FORCE_PROV_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        return false;
    }

    /* Let the pull-up settle, then debounce: require a sustained low. */
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int i = 0; i < 5; i++) {
        if (gpio_get_level(CONFIG_FORCE_PROV_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

/**
 * @brief Enter PROV mode: BLE GATTS provisioning + SoftAP captive portal.
 *        captive_portal_start() blocks; on a saved config the device reboots.
 */
static void start_prov_mode(void)
{
    ESP_LOGI(TAG, "Entering PROV mode (BLE provisioning + captive portal)");
    led_status_set(LED_PROV);

    nvs_config_set_op_mode(OP_MODE_PROV);

    esp_err_t ret = ble_prov_init(false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE prov init failed: %s (portal still works)",
                 esp_err_to_name(ret));
    }

    /* Blocks until a valid config is saved and the device reboots. */
    captive_portal_start();
}

/**
 * @brief Enter RUN mode: WiFi STA, MQTT, BLE GATTC, poll/publish loop.
 */
static void start_run_mode(void)
{
    ESP_LOGI(TAG, "Entering RUN mode");
    led_status_set(LED_CONNECTING);

    /* 1. WiFi credentials. */
    char ssid[33] = {0};
    char pass[65] = {0};
    nvs_config_get_wifi_ssid(ssid, sizeof(ssid));
    nvs_config_get_wifi_pass(pass, sizeof(pass));

    esp_err_t ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi manager init failed: %s", esp_err_to_name(ret));
        led_status_set(LED_ERROR);
        return;
    }

    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
    ret = wifi_manager_connect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed -- clearing creds, rebooting to portal");
        nvs_config_set_op_mode(OP_MODE_PROV);
        nvs_config_clear_wifi();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return;
    }
    ESP_LOGI(TAG, "Connected to WiFi: %s", ssid);

    /* WiFi + the OTA partition look healthy -- confirm the OTA boot. */
    ota_handler_confirm_boot();

    /* 2. MQTT broker settings. */
    char mqtt_host[128] = {0};
    char mqtt_user[64]  = {0};
    char mqtt_pass[64]  = {0};
    char prefix[64]     = {0};

    if (!nvs_config_get_mqtt_host(mqtt_host, sizeof(mqtt_host)) ||
        mqtt_host[0] == '\0') {
        ESP_LOGE(TAG, "No MQTT host configured -- rebooting to portal");
        nvs_config_set_op_mode(OP_MODE_PROV);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return;
    }

    int mqtt_port = nvs_config_get_mqtt_port(1883);
    nvs_config_get_mqtt_user(mqtt_user, sizeof(mqtt_user));
    nvs_config_get_mqtt_pass(mqtt_pass, sizeof(mqtt_pass));
    if (!nvs_config_get_topic_prefix(prefix, sizeof(prefix)) ||
        prefix[0] == '\0') {
        strlcpy(prefix, CONFIG_MQTT_TOPIC_PREFIX, sizeof(prefix));
    }

    ESP_LOGI(TAG, "MQTT broker: %s:%d  prefix=%s", mqtt_host, mqtt_port, prefix);
    ret = mqtt_manager_init(mqtt_host, mqtt_port,
                            mqtt_user[0] ? mqtt_user : NULL,
                            mqtt_pass[0] ? mqtt_pass : NULL,
                            prefix);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed: %s", esp_err_to_name(ret));
        led_status_set(LED_ERROR);
        /* Keep going; the BLE side is still useful and MQTT may reconnect. */
    }

    /* 3. BLE GATTC to the treadmill. */
    ret = ble_treadmill_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE treadmill init failed: %s", esp_err_to_name(ret));
        led_status_set(LED_ERROR);
        return;
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "   Treadmill S3 Bridge running (RUN mode)");
    ESP_LOGI(TAG, "==================================================");

    /* 4. Poll/publish loop. */
    bool discovery_published = false;
    bool last_ble_connected  = false;

    const TickType_t poll_ticks = pdMS_TO_TICKS(CONFIG_POLL_INTERVAL_MS);
    TickType_t last_status_log  = xTaskGetTickCount();

    while (1) {
        bool ble_connected  = ble_treadmill_is_connected();
        bool mqtt_connected = mqtt_manager_is_connected();

        /* Publish HA discovery once, the first time MQTT is up. */
        if (mqtt_connected && !discovery_published) {
            mqtt_manager_publish_discovery();
            discovery_published = true;
            ESP_LOGI(TAG, "Published Home Assistant discovery configs");
        }
        if (!mqtt_connected) {
            /* Re-publish discovery on the next reconnect. */
            discovery_published = false;
        }

        /* LED reflects the BLE link state. */
        if (ble_connected != last_ble_connected) {
            led_status_set(ble_connected ? LED_STREAMING : LED_CONNECTING);
            last_ble_connected = ble_connected;
        }

        /* Sample telemetry and publish. */
        treadmill_telemetry_t t;
        if (ble_treadmill_get(&t)) {
            t.connected = ble_connected;
            if (mqtt_connected) {
                mqtt_manager_publish_telemetry(&t);
            }
        }

        /* Periodic status log (every ~10s). */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_log) >= pdMS_TO_TICKS(10000)) {
            last_status_log = now;
            char ip_str[16] = "N/A";
            wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Status | WiFi:%s IP:%s | MQTT:%s | BLE:%s",
                     wifi_manager_is_connected() ? "UP" : "DOWN", ip_str,
                     mqtt_connected ? "UP" : "DOWN",
                     ble_connected ? "UP" : "DOWN");
        }

        vTaskDelay(poll_ticks);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "   Treadmill S3 Bridge");
    ESP_LOGI(TAG, "   HW: ESP32-S3 (SuperMini)  Role: BLE central");
    ESP_LOGI(TAG, "==================================================");

    /* 1. NVS flash + config. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS (%s)", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_config_init();
    ESP_LOGI(TAG, "NVS config initialized");

    /* 2. Status LED. */
    led_status_init();

    /* 3. Decide boot path. */
    bool force_prov = force_prov_requested();
    if (force_prov) {
        ESP_LOGW(TAG, "Force-provisioning button held -- entering PROV mode");
    }

    if (force_prov || !nvs_config_has_wifi()) {
        start_prov_mode();
        return; /* captive_portal_start() blocks; reached only if it returns. */
    }

    start_run_mode();
}
