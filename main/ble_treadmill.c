/**
 * @file ble_treadmill.c
 * @brief Bluedroid GATT client for the NordicTrack/iFit "I_TL" treadmill.
 *
 * Proprietary Nordic-Semi iFit control service (NOT FTMS):
 *   Service UUID  00001533-1412-efde-1523-785feabcd123
 *   Notify char   00001535-...  (handle ~0x000B, props NOTIFY/READ)
 *   Write  char   00001534-...  (handle ~0x000E, props WRITE/READ)
 *   CCCD          0x2902 on the notify char — write 0x0001 to subscribe.
 *
 * This board is host-polled request/response, not push. After subscribing we
 * replay the nordictrack10 INIT handshake, then drive a 200 ms poll loop
 * (esp_timer) that writes the FE021403 group every tick. The treadmill answers
 * with notification bursts; the live speed/incline page is a single 20-byte
 * frame beginning `00 12 01 04 02`.
 *
 * S3 HARDENING: the GATTC notify callback runs on the BTC task. It does ONLY a
 * memcpy of the raw bytes into a FreeRTOS queue. A dedicated worker task drains
 * the queue, does FE/00/FF reassembly and per-frame telemetry decode, and
 * updates a mutex-protected treadmill_telemetry_t.
 *
 * Assumes the Bluedroid stack (controller + bluedroid) is already enabled by
 * the caller (mode-split owner). We only register GAP/GATTC callbacks + app.
 */

#include "ble_treadmill.h"
#include "sdkconfig.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static const char *TAG = "treadmill";

/* ── Poll cadence ──────────────────────────────────────────────────────── */
#ifndef CONFIG_POLL_INTERVAL_MS
#define CONFIG_POLL_INTERVAL_MS 200
#endif
#define POLL_INTERVAL_US   ((int64_t)CONFIG_POLL_INTERVAL_MS * 1000)

/* App registration id (GATTC profile slot). */
#define TREADMILL_APP_ID   0

/* ── Target UUIDs (128-bit, little-endian byte order for esp_bt_uuid_t) ──
 * Text UUID:  00001533-1412-efde-1523-785feabcd123
 * Reversed:   23 d1 bc ea 5f 78 23 15 de ef 12 14 33 15 00 00   (svc)
 * Only the 16-bit "short" field (bytes 12,13 here) changes between chars:
 *   1533 = service, 1534 = write, 1535 = notify.
 */
static const uint8_t SVC_UUID[16] = {
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
    0xde, 0xef, 0x12, 0x14, 0x33, 0x15, 0x00, 0x00
};
static const uint8_t WRITE_UUID[16] = {
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
    0xde, 0xef, 0x12, 0x14, 0x34, 0x15, 0x00, 0x00
};
static const uint8_t NOTIFY_UUID[16] = {
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
    0xde, 0xef, 0x12, 0x14, 0x35, 0x15, 0x00, 0x00
};

#define TARGET_NAME "I_TL"

/* ── BLE link state ────────────────────────────────────────────────────── */
static esp_gatt_if_t      g_if = ESP_GATT_IF_NONE;
static uint16_t           g_conn_id;
static esp_bd_addr_t      g_mac;
static esp_ble_addr_type_t g_addr_type = BLE_ADDR_TYPE_PUBLIC;

static bool g_connected = false;   /* link up + CCCD subscribed */
static bool g_scanning  = false;
static bool g_ready     = false;   /* notify enabled, can write commands */

static uint16_t g_svc_start, g_svc_end;
static uint16_t g_write_handle, g_notify_handle;

/* ── Telemetry cache (mutex-protected) ─────────────────────────────────── */
static SemaphoreHandle_t   g_tel_mutex;
static treadmill_telemetry_t g_tel;   /* latest decoded sample */
static bool g_tel_valid = false;      /* a telemetry page has been decoded */

/* ── Notification queue (BTC callback → worker task) ───────────────────── */
#define NOTIFY_FRAME_MAX  64    /* a BLE frame is <= MTU; 64 is generous */
#define NOTIFY_QUEUE_LEN  16

typedef struct {
    uint16_t len;
    uint8_t  data[NOTIFY_FRAME_MAX];
} notify_item_t;

static QueueHandle_t g_notify_queue;
static TaskHandle_t  g_worker_task;

/* ── Poll loop ─────────────────────────────────────────────────────────── */
static esp_timer_handle_t g_poll_timer;

/* nordictrack10 INIT handshake — exact 20-byte frames recovered from QZ
 * btinit(); these match our I_TL capture. Replay verbatim, ~150-400 ms apart. */
static const uint8_t INIT_FRAMES[][20] = {
    {0xFE,0x02,0x08,0x02},
    {0xFF,0x08,0x02,0x04,0x02,0x04,0x02,0x04,0x81,0x87,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0xFE,0x02,0x08,0x02},
    {0xFF,0x08,0x02,0x04,0x02,0x04,0x04,0x04,0x80,0x88,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0xFE,0x02,0x08,0x02},
    {0xFF,0x08,0x02,0x04,0x02,0x04,0x04,0x04,0x88,0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0xFE,0x02,0x0A,0x02},
    {0xFF,0x0A,0x02,0x04,0x02,0x06,0x02,0x06,0x82,0x00,0x00,0x8A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0xFE,0x02,0x0A,0x02},
    {0xFF,0x0A,0x02,0x04,0x02,0x06,0x02,0x06,0x84,0x00,0x00,0x8C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0xFE,0x02,0x08,0x02},
    {0xFF,0x08,0x02,0x04,0x02,0x04,0x02,0x04,0x95,0x9B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0xFE,0x02,0x2C,0x04},
    {0x00,0x12,0x02,0x04,0x02,0x28,0x04,0x28,0x90,0x04,0x00,0x61,0xD8,0x5D,0xD0,0x51,0xD0,0x55,0xE8,0x61},
    {0x01,0x12,0xF8,0x8D,0x00,0x91,0x20,0xD5,0x48,0xE1,0x98,0x3D,0xD0,0x71,0x10,0xB5,0x48,0xE1,0xB8,0x4D},
    {0xFF,0x08,0xE0,0xB1,0x40,0x80,0x02,0x00,0x00,0x75,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};
/* Per-frame lengths (INIT frames are not all 20 bytes; headers are 4). */
static const uint8_t INIT_LENS[] = {
    4, 20, 4, 20, 4, 20, 4, 20, 4, 20, 4, 20, 4, 20, 20, 20,
};
#define INIT_FRAME_COUNT (sizeof(INIT_LENS) / sizeof(INIT_LENS[0]))

/* Steady-state poll group (FE021403) — written every tick. */
static const uint8_t POLL_FRAME1[] = {0xFE,0x02,0x14,0x03};
static const uint8_t POLL_FRAME2[] = {
    0x00,0x12,0x02,0x04,0x02,0x10,0x04,0x10,0x02,0x00,
    0x0A,0x13,0x94,0x33,0x00,0x10,0x00,0x00,0x00,0x00
};
static const uint8_t POLL_FRAME3[] = {
    0xFF,0x02,0x18,0xF2,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* ── Helpers ───────────────────────────────────────────────────────────── */

static bool uuid128_eq(const esp_bt_uuid_t *u, const uint8_t *target)
{
    return u->len == ESP_UUID_LEN_128 && memcmp(u->uuid.uuid128, target, 16) == 0;
}

static esp_err_t write_cmd(const uint8_t *data, uint16_t len)
{
    if (!g_ready || g_write_handle == 0) return ESP_ERR_INVALID_STATE;
    return esp_ble_gattc_write_char(g_if, g_conn_id, g_write_handle,
                                    len, (uint8_t *)data,
                                    ESP_GATT_WRITE_TYPE_NO_RSP,
                                    ESP_GATT_AUTH_REQ_NONE);
}

/* ── Telemetry decode (per QZ proformtreadmill parser) ──
 * Live page is a 20-byte frame starting 00 12 01 04 02.
 *   speed_kmh   = ((b[11]<<8)|b[10]) / 100.0
 *   incline_pct = (int16)((b[13]<<8)|b[12]) / 100.0
 *   elapsed_s   = b[14]
 *   moving      = speed > 0
 * Reject placeholder frames where bytes 12..19 are all 0xFF. */
static void try_decode_telemetry(const uint8_t *b, uint16_t len)
{
    if (len != 20) return;
    if (b[0] != 0x00 || b[1] != 0x12 || b[2] != 0x01 || b[3] != 0x04 || b[4] != 0x02)
        return;
    /* Only page 0x29 is the live telemetry page (response to the FE021403 poll).
     * The INIT handshake also produces 00 12 01 04 02 frames (pages 0x10/0x1c/
     * 0x1d/0x21/...) carrying firmware/serial/config — decoding those as speed
     * yields garbage (e.g. 194 km/h). Verified live: 0x29 holds at 0/3/5 mph. */
    if (b[5] != 0x29) return;

    bool all_ff = true;
    for (int i = 12; i < 20; i++) {
        if (b[i] != 0xFF) { all_ff = false; break; }
    }
    if (all_ff) return;

    float speed = (float)((b[11] << 8) | b[10]) / 100.0f;
    int16_t incl_raw = (int16_t)((b[13] << 8) | b[12]);
    float incline = (float)incl_raw / 100.0f;
    uint32_t elapsed = b[14];

    if (xSemaphoreTake(g_tel_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_tel.speed_kmh   = speed;
        g_tel.incline_pct = incline;
        g_tel.elapsed_s   = elapsed;
        g_tel.moving      = speed > 0.0f;
        g_tel.connected   = g_connected;
        g_tel_valid = true;
        xSemaphoreGive(g_tel_mutex);
    }

    ESP_LOGI(TAG, "page 0x%02x  speed=%.2f km/h  incline=%.2f %%  elapsed=%us",
             b[5], speed, incline, (unsigned)elapsed);
}

/* ── Worker task: drain notify queue, reassemble + decode ──────────────── */
static void worker_task(void *arg)
{
    notify_item_t item;
    for (;;) {
        if (xQueueReceive(g_notify_queue, &item, portMAX_DELAY) != pdTRUE)
            continue;

        /* Telemetry pages arrive as a single self-contained 20-byte frame
         * (00 12 01 04 02 ...) — decode it directly. Reassembly of FE/00/FF
         * fragments is only needed for the longer config/info messages, which
         * we do not consume here, so per-frame inspection is sufficient. */
        try_decode_telemetry(item.data, item.len);
    }
}

/* ── Poll timer callback (runs on esp_timer task) ──────────────────────── */
static void poll_timer_cb(void *arg)
{
    if (!g_ready) return;
    write_cmd(POLL_FRAME1, sizeof(POLL_FRAME1));
    write_cmd(POLL_FRAME2, sizeof(POLL_FRAME2));
    write_cmd(POLL_FRAME3, sizeof(POLL_FRAME3));
}

/* ── INIT handshake replay (runs in a one-shot task; uses delays) ──────── */
static void init_task(void *arg)
{
    ESP_LOGI(TAG, "Replaying INIT handshake (%d frames)", (int)INIT_FRAME_COUNT);
    for (size_t i = 0; i < INIT_FRAME_COUNT; i++) {
        write_cmd(INIT_FRAMES[i], INIT_LENS[i]);
        vTaskDelay(pdMS_TO_TICKS(250));
        if (!g_ready) {   /* disconnected mid-handshake */
            ESP_LOGW(TAG, "INIT aborted — link dropped");
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGI(TAG, "INIT complete — starting %dms poll loop", CONFIG_POLL_INTERVAL_MS);

    if (g_poll_timer) {
        esp_timer_start_periodic(g_poll_timer, POLL_INTERVAL_US);
    }
    vTaskDelete(NULL);
}

/* ── GAP handler ───────────────────────────────────────────────────────── */
static void gap_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0);
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (p->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Scanning for \"%s\"...", TARGET_NAME);
            g_scanning = true;
        } else {
            ESP_LOGE(TAG, "Scan start failed: %d", p->scan_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        uint8_t name_len = 0;
        uint8_t *name = esp_ble_resolve_adv_data(p->scan_rst.ble_adv,
                                                 ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
        if (!name)
            name = esp_ble_resolve_adv_data(p->scan_rst.ble_adv,
                                            ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);

        bool match = false;
        if (name && name_len > 0) {
            char s[32] = {0};
            memcpy(s, name, name_len < 31 ? name_len : 31);
            if (strcmp(s, TARGET_NAME) == 0) {
                ESP_LOGI(TAG, "Found %s RSSI=%d", s, p->scan_rst.rssi);
                match = true;
            }
        }

        if (match) {
            esp_ble_gap_stop_scanning();
            g_scanning = false;
            memcpy(g_mac, p->scan_rst.bda, sizeof(esp_bd_addr_t));
            g_addr_type = p->scan_rst.ble_addr_type;
            ESP_LOGI(TAG, "Connecting to %02x:%02x:%02x:%02x:%02x:%02x",
                     g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
            esp_ble_gattc_open(g_if, p->scan_rst.bda,
                               p->scan_rst.ble_addr_type, true);
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        g_scanning = false;
        break;

    default:
        break;
    }
}

/* ── GATTC handler ─────────────────────────────────────────────────────── */
static void gattc_handler(esp_gattc_cb_event_t event,
                          esp_gatt_if_t gattc_if,
                          esp_ble_gattc_cb_param_t *p)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        g_if = gattc_if;
        esp_ble_gap_set_scan_params(&(esp_ble_scan_params_t){
            .scan_type          = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval      = 0x80,
            .scan_window        = 0x40,
        });
        break;

    case ESP_GATTC_OPEN_EVT:
        if (p->open.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Connected");
            g_conn_id = p->open.conn_id;
            esp_ble_gattc_search_service(gattc_if, g_conn_id, NULL);
        } else {
            ESP_LOGE(TAG, "Connect failed: %d — rescanning", p->open.status);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_ble_gap_start_scanning(0);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (uuid128_eq(&p->search_res.srvc_id.uuid, SVC_UUID)) {
            g_svc_start = p->search_res.start_handle;
            g_svc_end   = p->search_res.end_handle;
            ESP_LOGI(TAG, "iFit service 0x%04x-0x%04x", g_svc_start, g_svc_end);
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (!g_svc_start) {
            ESP_LOGE(TAG, "iFit service not found");
            esp_ble_gattc_close(gattc_if, g_conn_id);
            break;
        }

        uint16_t count = 0;
        esp_ble_gattc_get_attr_count(gattc_if, g_conn_id,
                                     ESP_GATT_DB_CHARACTERISTIC,
                                     g_svc_start, g_svc_end, 0, &count);
        if (!count) {
            ESP_LOGE(TAG, "No characteristics in service");
            esp_ble_gattc_close(gattc_if, g_conn_id);
            break;
        }

        esp_gattc_char_elem_t *ch = malloc(sizeof(esp_gattc_char_elem_t) * count);
        if (!ch) break;
        esp_ble_gattc_get_all_char(gattc_if, g_conn_id,
                                   g_svc_start, g_svc_end, ch, &count, 0);
        for (int i = 0; i < count; i++) {
            if (uuid128_eq(&ch[i].uuid, WRITE_UUID)) {
                g_write_handle = ch[i].char_handle;
                ESP_LOGI(TAG, "Write handle: 0x%04x", g_write_handle);
            }
            if (uuid128_eq(&ch[i].uuid, NOTIFY_UUID)) {
                g_notify_handle = ch[i].char_handle;
                ESP_LOGI(TAG, "Notify handle: 0x%04x", g_notify_handle);
                esp_ble_gattc_register_for_notify(gattc_if, g_mac, g_notify_handle);
            }
        }
        free(ch);

        if (!g_write_handle || !g_notify_handle) {
            ESP_LOGE(TAG, "Missing write/notify char — closing");
            esp_ble_gattc_close(gattc_if, g_conn_id);
        }
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (p->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Register notify failed: %d", p->reg_for_notify.status);
            break;
        }

        /* Find CCCD (0x2902) and write 0x0001 to subscribe. */
        uint16_t dcount = 0;
        esp_ble_gattc_get_attr_count(gattc_if, g_conn_id,
                                     ESP_GATT_DB_DESCRIPTOR,
                                     g_svc_start, g_svc_end,
                                     g_notify_handle, &dcount);
        if (!dcount) break;

        esp_gattc_descr_elem_t *desc = malloc(sizeof(esp_gattc_descr_elem_t) * dcount);
        if (!desc) break;
        esp_ble_gattc_get_all_descr(gattc_if, g_conn_id,
                                    g_notify_handle, desc, &dcount, 0);
        for (int i = 0; i < dcount; i++) {
            if (desc[i].uuid.len == ESP_UUID_LEN_16 &&
                desc[i].uuid.uuid.uuid16 == 0x2902) {
                uint16_t en = 0x0001;
                esp_ble_gattc_write_char_descr(gattc_if, g_conn_id, desc[i].handle,
                                               sizeof(en), (uint8_t *)&en,
                                               ESP_GATT_WRITE_TYPE_RSP,
                                               ESP_GATT_AUTH_REQ_NONE);
                ESP_LOGI(TAG, "Enabling CCCD 0x%04x", desc[i].handle);
                break;
            }
        }
        free(desc);
        break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (p->write.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "Subscribed — link ready");
            g_connected = true;
            g_ready = true;
            if (xSemaphoreTake(g_tel_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_tel.connected = true;
                xSemaphoreGive(g_tel_mutex);
            }
            /* Replay INIT handshake on a task (it sleeps between frames),
             * then it kicks off the poll timer. */
            xTaskCreate(init_task, "tm_init", 3072, NULL, 5, NULL);
        } else {
            ESP_LOGE(TAG, "CCCD write failed: %d", p->write.status);
        }
        break;

    case ESP_GATTC_NOTIFY_EVT: {
        /* BTC task context — copy bytes only, parse on worker task. */
        notify_item_t item;
        uint16_t n = p->notify.value_len;
        if (n > NOTIFY_FRAME_MAX) n = NOTIFY_FRAME_MAX;
        item.len = n;
        memcpy(item.data, p->notify.value, n);
        BaseType_t woken = pdFALSE;
        /* notify cb may run in ISR-adjacent context on some ports; use the
         * non-ISR send (BTC is a task) but never block. */
        if (xQueueSend(g_notify_queue, &item, 0) != pdTRUE) {
            /* queue full — drop oldest semantics not needed; just drop. */
        }
        (void)woken;
        break;
    }

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "Disconnected (reason=0x%x) — rescanning", p->disconnect.reason);
        if (g_poll_timer) esp_timer_stop(g_poll_timer);
        g_connected = false;
        g_ready = false;
        g_write_handle = 0;
        g_notify_handle = 0;
        g_svc_start = g_svc_end = 0;

        if (xSemaphoreTake(g_tel_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_tel.connected = false;
            g_tel.moving = false;
            xSemaphoreGive(g_tel_mutex);
        }

        if (g_if != ESP_GATT_IF_NONE) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_ble_gap_start_scanning(0);
        }
        break;

    default:
        break;
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

esp_err_t ble_treadmill_init(void)
{
    ESP_LOGI(TAG, "Initializing treadmill GATT client");

    g_tel_mutex = xSemaphoreCreateMutex();
    if (!g_tel_mutex) return ESP_ERR_NO_MEM;
    memset(&g_tel, 0, sizeof(g_tel));

    g_notify_queue = xQueueCreate(NOTIFY_QUEUE_LEN, sizeof(notify_item_t));
    if (!g_notify_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(worker_task, "tm_worker", 4096, NULL, 6, &g_worker_task) != pdPASS)
        return ESP_ERR_NO_MEM;

    const esp_timer_create_args_t targs = {
        .callback = poll_timer_cb,
        .name = "tm_poll",
    };
    esp_err_t ret = esp_timer_create(&targs, &g_poll_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "poll timer create: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Bring up the BT controller + Bluedroid (BLE-only). In PROV mode ble_prov
     * performs this init; in RUN mode the GATT client is the only BLE user, so
     * we must do it here. Guarded so it is safe regardless of caller. */
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);  /* S3: no classic BT */
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&bt_cfg);
        if (ret) { ESP_LOGE(TAG, "bt ctrl init: %s", esp_err_to_name(ret)); return ret; }
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret) { ESP_LOGE(TAG, "bt ctrl enable: %s", esp_err_to_name(ret)); return ret; }
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        ret = esp_bluedroid_init();
        if (ret) { ESP_LOGE(TAG, "bluedroid init: %s", esp_err_to_name(ret)); return ret; }
        ret = esp_bluedroid_enable();
        if (ret) { ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(ret)); return ret; }
    }

    ret = esp_ble_gattc_register_callback(gattc_handler);
    if (ret) { ESP_LOGE(TAG, "gattc cb reg: %s", esp_err_to_name(ret)); return ret; }
    ret = esp_ble_gap_register_callback(gap_handler);
    if (ret) { ESP_LOGE(TAG, "gap cb reg: %s", esp_err_to_name(ret)); return ret; }
    ret = esp_ble_gattc_app_register(TREADMILL_APP_ID);
    if (ret) { ESP_LOGE(TAG, "app reg: %s", esp_err_to_name(ret)); return ret; }

    esp_ble_gatt_set_local_mtu(247);

    ESP_LOGI(TAG, "Treadmill GATT client ready (scan starts on REG_EVT)");
    return ESP_OK;
}

bool ble_treadmill_get(treadmill_telemetry_t *out)
{
    if (!out) return false;
    bool ok = false;
    if (xSemaphoreTake(g_tel_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_tel_valid) {
            *out = g_tel;
            ok = true;
        }
        xSemaphoreGive(g_tel_mutex);
    }
    return ok;
}

bool ble_treadmill_is_connected(void)
{
    return g_connected;
}
