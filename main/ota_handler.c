#include "ota_handler.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

static const char *TAG = "ota";

esp_err_t ota_handler_start(const char *url)
{
    if (!url || url[0] == '\0') {
        ESP_LOGE(TAG, "Empty OTA URL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Firmware size: %d bytes", total);

    int written = 0;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            written = esp_https_ota_get_image_len_read(handle);
            if (total > 0 && (written % (64 * 1024)) < 4096) {
                ESP_LOGI(TAG, "Progress: %d / %d (%d%%)", written, total, written * 100 / total);
            }
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
            esp_https_ota_abort(handle);
            return err;
        }
        break;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Incomplete OTA data");
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA complete, restarting...");
    esp_restart();
    return ESP_OK;
}

void ota_handler_confirm_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First heartbeat OK after OTA — marking partition valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

bool ota_handler_is_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        return state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    return false;
}
