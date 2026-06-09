/**
 * @file ble_prov.c
 * @brief BLE GATT server for treadmill bridge provisioning (PROV mode).
 *
 * Bluedroid GATTS service 0xCE00 with characteristics:
 *   CE01 READ   — Device info string ("Treadmill-Setup")
 *   CE02 READ   — Firmware version string
 *   CE03 WRITE  — Config JSON blob (chunked write, reassembled)
 *   CE04 NOTIFY — Provisioning status code
 *
 * A client writes a JSON config:
 *   {"wifi_ssid","wifi_pass","mqtt_host","mqtt_port","mqtt_user",
 *    "mqtt_pass","topic_prefix"}
 * On a valid config the values are stored in NVS, op_mode is set to
 * OP_MODE_RUN, and the device reboots into RUN mode.
 *
 * Advertises device name "Treadmill-Setup".
 */

#include "ble_prov.h"
#include "nvs_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_prov";

#define PROV_DEVICE_NAME "Treadmill-Setup"

/* ── State ── */

static bool s_active = false;
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id;
static bool s_connected = false;

/* Service and characteristic handles (assigned by stack) */
static uint16_t s_svc_handle;
static uint16_t s_info_handle;
static uint16_t s_fw_handle;
static uint16_t s_config_handle;
static uint16_t s_status_handle;
static uint16_t s_status_ccc_handle;

/* ── Attribute database ── */

enum {
    IDX_SVC,
    IDX_INFO_DECL, IDX_INFO_VAL,
    IDX_FW_DECL, IDX_FW_VAL,
    IDX_CONFIG_DECL, IDX_CONFIG_VAL,
    IDX_STATUS_DECL, IDX_STATUS_VAL, IDX_STATUS_CCC,
    IDX_NB,
};

static uint16_t s_handle_table[IDX_NB];

/* UUIDs */
static const uint16_t primary_svc_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_decl_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t ccc_uuid         = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint16_t svc_uuid    = BLE_PROV_SVC_UUID;
static const uint16_t info_uuid   = BLE_PROV_CHAR_INFO_UUID;
static const uint16_t fw_uuid     = BLE_PROV_CHAR_FW_UUID;
static const uint16_t config_uuid = BLE_PROV_CHAR_CONFIG_UUID;
static const uint16_t status_uuid = BLE_PROV_CHAR_STATUS_UUID;

/* Characteristic properties */
static const uint8_t prop_read   = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t prop_write  = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;

/* Characteristic values (info/fw populated at runtime) */
static uint8_t info_val[24]   = PROV_DEVICE_NAME;
static uint8_t fw_val[24]     = "0.0.0";
static uint8_t config_val[1]  = {0};
static uint8_t status_val[1]  = {0};
static uint8_t status_ccc[2]  = {0, 0};

static const esp_gatts_attr_db_t s_gatt_db[IDX_NB] = {
    /* Service declaration */
    [IDX_SVC] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&primary_svc_uuid, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(svc_uuid), (uint8_t *)&svc_uuid}},

    /* Device info — declaration + value */
    [IDX_INFO_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(prop_read), (uint8_t *)&prop_read}},
    [IDX_INFO_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&info_uuid, ESP_GATT_PERM_READ,
         sizeof(info_val), sizeof(info_val), info_val}},

    /* FW version — declaration + value */
    [IDX_FW_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(prop_read), (uint8_t *)&prop_read}},
    [IDX_FW_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&fw_uuid, ESP_GATT_PERM_READ,
         sizeof(fw_val), sizeof(fw_val), fw_val}},

    /* Config write — declaration + value */
    [IDX_CONFIG_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(prop_write), (uint8_t *)&prop_write}},
    [IDX_CONFIG_VAL] = {{ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_16, (uint8_t *)&config_uuid, ESP_GATT_PERM_WRITE,
         512, sizeof(config_val), config_val}},

    /* Status notify — declaration + value + CCC descriptor */
    [IDX_STATUS_DECL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(prop_notify), (uint8_t *)&prop_notify}},
    [IDX_STATUS_VAL] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&status_uuid, 0,
         sizeof(status_val), sizeof(status_val), status_val}},
    [IDX_STATUS_CCC] = {{ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&ccc_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(status_ccc), sizeof(status_ccc), status_ccc}},
};

/* ── Config write reassembly buffer (BLE may chunk large writes) ── */

#define CONFIG_BUF_SIZE 512
static uint8_t s_config_buf[CONFIG_BUF_SIZE];
static size_t  s_config_len = 0;
static bool    s_status_notify_enabled = false;

static void send_status_notify(uint8_t status_code)
{
    if (!s_connected || !s_status_notify_enabled) return;
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                 s_status_handle, 1, &status_code, false);
}

/**
 * Parse config JSON and store in NVS. Expected format:
 * {"wifi_ssid":"...","wifi_pass":"...","mqtt_host":"...","mqtt_port":1883,
 *  "mqtt_user":"...","mqtt_pass":"...","topic_prefix":"treadmill"}
 * Only wifi_ssid is mandatory. Returns true on success.
 */
bool ble_prov_parse_config(const char *json_str, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) {
        ESP_LOGE(TAG, "Config JSON parse failed");
        return false;
    }

    cJSON *wifi_ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *wifi_pass = cJSON_GetObjectItem(root, "wifi_pass");

    if (!cJSON_IsString(wifi_ssid) || strlen(wifi_ssid->valuestring) == 0) {
        ESP_LOGE(TAG, "Missing or empty wifi_ssid");
        cJSON_Delete(root);
        return false;
    }

    nvs_config_set_wifi(wifi_ssid->valuestring,
                        cJSON_IsString(wifi_pass) ? wifi_pass->valuestring : "");

    cJSON *mqtt_host = cJSON_GetObjectItem(root, "mqtt_host");
    if (cJSON_IsString(mqtt_host) && strlen(mqtt_host->valuestring) > 0) {
        nvs_config_set_mqtt_host(mqtt_host->valuestring);
    }

    cJSON *mqtt_port = cJSON_GetObjectItem(root, "mqtt_port");
    if (cJSON_IsNumber(mqtt_port)) {
        nvs_config_set_mqtt_port(mqtt_port->valueint);
    } else if (cJSON_IsString(mqtt_port) && strlen(mqtt_port->valuestring) > 0) {
        int p = atoi(mqtt_port->valuestring);
        if (p > 0) nvs_config_set_mqtt_port(p);
    }

    cJSON *mqtt_user = cJSON_GetObjectItem(root, "mqtt_user");
    cJSON *mqtt_pass = cJSON_GetObjectItem(root, "mqtt_pass");
    if (cJSON_IsString(mqtt_user) && strlen(mqtt_user->valuestring) > 0) {
        nvs_config_set_mqtt_creds(mqtt_user->valuestring,
                                  cJSON_IsString(mqtt_pass) ? mqtt_pass->valuestring : "");
    }

    cJSON *topic_prefix = cJSON_GetObjectItem(root, "topic_prefix");
    if (cJSON_IsString(topic_prefix) && strlen(topic_prefix->valuestring) > 0) {
        nvs_config_set_topic_prefix(topic_prefix->valuestring);
    }

    ESP_LOGI(TAG, "Config applied — WiFi SSID: %s, MQTT host: %s",
             wifi_ssid->valuestring,
             (cJSON_IsString(mqtt_host) && strlen(mqtt_host->valuestring) > 0)
                 ? mqtt_host->valuestring : "(unset)");

    /* Switch to RUN mode for the next boot. */
    nvs_config_set_op_mode(OP_MODE_RUN);

    cJSON_Delete(root);
    return true;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/* ── Advertising ── */

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min       = 0x40,   /* 40ms */
    .adv_int_max       = 0x80,   /* 80ms */
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void start_advertising(void)
{
    esp_ble_gap_start_advertising(&s_adv_params);
}

/* ── Populate read characteristic values ── */

static void populate_values(void)
{
    memset(info_val, 0, sizeof(info_val));
    strncpy((char *)info_val, PROV_DEVICE_NAME, sizeof(info_val) - 1);

    memset(fw_val, 0, sizeof(fw_val));
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strncpy((char *)fw_val, desc->version, sizeof(fw_val) - 1);
    } else {
        strncpy((char *)fw_val, "0.0.0", sizeof(fw_val) - 1);
    }
}

/* ── GAP event handler ── */

static void gap_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        start_advertising();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_active = true;
            ESP_LOGI(TAG, "Advertising started");
        } else {
            ESP_LOGE(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_active = false;
        ESP_LOGI(TAG, "Advertising stopped");
        break;
    default:
        break;
    }
}

/* ── GATTS event handler ── */

static void gatts_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                           esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTS register failed: %d", param->reg.status);
            return;
        }
        s_gatts_if = gatts_if;
        ESP_LOGI(TAG, "GATTS app registered, if=%d", gatts_if);

        populate_values();

        esp_ble_gap_set_device_name(PROV_DEVICE_NAME);

        /* Build raw advertising data: flags + complete local name + service UUID */
        uint8_t adv_raw[31];
        size_t pos = 0;

        /* AD: Flags (LE General Discoverable + BR/EDR Not Supported) */
        adv_raw[pos++] = 2;     /* length */
        adv_raw[pos++] = 0x01;  /* type: flags */
        adv_raw[pos++] = 0x06;  /* LE General + BR/EDR not supported */

        /* AD: Complete Local Name */
        const char *adv_name = PROV_DEVICE_NAME;
        size_t name_len = strlen(adv_name);
        if (name_len > 20) name_len = 20;
        adv_raw[pos++] = (uint8_t)(name_len + 1);
        adv_raw[pos++] = 0x09;  /* type: complete local name */
        memcpy(adv_raw + pos, adv_name, name_len);
        pos += name_len;

        /* AD: Complete 16-bit Service UUID */
        adv_raw[pos++] = 3;     /* length */
        adv_raw[pos++] = 0x03;  /* type: complete 16-bit UUID list */
        adv_raw[pos++] = (uint8_t)(BLE_PROV_SVC_UUID & 0xFF);
        adv_raw[pos++] = (uint8_t)(BLE_PROV_SVC_UUID >> 8);

        esp_ble_gap_config_adv_data_raw(adv_raw, pos);

        /* Create attribute table */
        esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Create attr table failed: %d", param->add_attr_tab.status);
            return;
        }
        if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "Attr handle count mismatch: got %d, want %d",
                     param->add_attr_tab.num_handle, IDX_NB);
            return;
        }
        memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));

        s_svc_handle        = s_handle_table[IDX_SVC];
        s_info_handle       = s_handle_table[IDX_INFO_VAL];
        s_fw_handle         = s_handle_table[IDX_FW_VAL];
        s_config_handle     = s_handle_table[IDX_CONFIG_VAL];
        s_status_handle     = s_handle_table[IDX_STATUS_VAL];
        s_status_ccc_handle = s_handle_table[IDX_STATUS_CCC];
        (void)s_info_handle;
        (void)s_fw_handle;

        esp_ble_gatts_start_service(s_svc_handle);
        ESP_LOGI(TAG, "Service started, %d handles", IDX_NB);
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        s_config_len = 0;
        ESP_LOGI(TAG, "Client connected, conn_id=%d", s_conn_id);
        /* Request a larger MTU for the config write. */
        esp_ble_gatt_set_local_mtu(247);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_status_notify_enabled = false;
        s_config_len = 0;
        ESP_LOGI(TAG, "Client disconnected");
        start_advertising();
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_config_handle) {
            /* Accumulate data (BLE may chunk writes across multiple packets) */
            if (param->write.len > 0 && s_config_len + param->write.len < CONFIG_BUF_SIZE) {
                memcpy(s_config_buf + s_config_len, param->write.value, param->write.len);
                s_config_len += param->write.len;
            }

            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                         param->write.trans_id, ESP_GATT_OK, NULL);

            /* If this is the final write (not prepare), process the config */
            if (!param->write.is_prep) {
                s_config_buf[s_config_len] = '\0';
                ESP_LOGI(TAG, "Config received (%d bytes): %s",
                         (int)s_config_len, s_config_buf);

                send_status_notify(BLE_PROV_STATUS_RECEIVED);

                if (ble_prov_parse_config((const char *)s_config_buf, s_config_len)) {
                    send_status_notify(BLE_PROV_STATUS_READY);
                    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
                } else {
                    send_status_notify(BLE_PROV_STATUS_WIFI_FAILED);
                }

                s_config_len = 0;
            }
        } else if (param->write.handle == s_status_ccc_handle) {
            if (param->write.len == 2) {
                s_status_notify_enabled = (param->write.value[0] | param->write.value[1]);
                ESP_LOGI(TAG, "Status notifications %s",
                         s_status_notify_enabled ? "enabled" : "disabled");
            }
        }
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU updated to %d", param->mtu.mtu);
        break;

    default:
        break;
    }
}

/* ── Public API ── */

esp_err_t ble_prov_init(bool bluedroid_already_init)
{
    ESP_LOGI(TAG, "Initializing BLE provisioning server");

    if (!bluedroid_already_init) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_bt_controller_init(&bt_cfg);
        if (ret) { ESP_LOGE(TAG, "BT init: %s", esp_err_to_name(ret)); return ret; }

        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret) { ESP_LOGE(TAG, "BT enable: %s", esp_err_to_name(ret)); return ret; }

        ret = esp_bluedroid_init();
        if (ret) { ESP_LOGE(TAG, "BD init: %s", esp_err_to_name(ret)); return ret; }

        ret = esp_bluedroid_enable();
        if (ret) { ESP_LOGE(TAG, "BD enable: %s", esp_err_to_name(ret)); return ret; }
    }

    esp_err_t ret = esp_ble_gatts_register_callback(gatts_handler);
    if (ret) { ESP_LOGE(TAG, "gatts cb reg: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_ble_gap_register_callback(gap_handler);
    if (ret) { ESP_LOGE(TAG, "gap cb reg: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_ble_gatts_app_register(BLE_PROV_APP_ID);
    if (ret) { ESP_LOGE(TAG, "app reg: %s", esp_err_to_name(ret)); return ret; }

    ESP_LOGI(TAG, "BLE provisioning server initialized");
    return ESP_OK;
}

void ble_prov_stop(void)
{
    if (!s_active && s_gatts_if == ESP_GATT_IF_NONE) return;

    if (s_connected) {
        esp_ble_gatts_close(s_gatts_if, s_conn_id);
        s_connected = false;
    }

    esp_ble_gap_stop_advertising();
    esp_ble_gatts_app_unregister(s_gatts_if);
    s_gatts_if = ESP_GATT_IF_NONE;
    s_active = false;
    ESP_LOGI(TAG, "BLE provisioning server stopped");
}

bool ble_prov_is_active(void)
{
    return s_active;
}
