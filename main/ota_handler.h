#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Start an OTA update from the given URL.
 * Downloads firmware to the inactive OTA partition, switches the boot
 * partition, and restarts. Returns ESP_OK only if the update succeeds
 * (the device reboots before returning). On failure, returns an error
 * code and the device continues running the current firmware.
 */
esp_err_t ota_handler_start(const char *url);

/**
 * Call once after a successful boot/connection cycle.
 * Marks the current partition valid (cancels the rollback) the first
 * time it is called after an OTA update.
 */
void ota_handler_confirm_boot(void);

/**
 * @return true if the current boot is a pending OTA validation
 *         (first boot after an update, not yet confirmed).
 */
bool ota_handler_is_pending_verify(void);
