#include "captive_portal.h"
#include "nvs_config.h"
#include "sdkconfig.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "portal";

#define PORTAL_AP_CHANNEL 1
#define PORTAL_MAX_CONN   4

/* ── Cached WiFi scan results ──────────────────────────────────────── */

static char s_scan_json[1024] = "[]";

static void do_scan_and_cache(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;

    wifi_ap_record_t *aps = calloc(num, sizeof(wifi_ap_record_t));
    if (!aps) return;
    esp_wifi_scan_get_ap_records(&num, aps);

    size_t pos = 1;
    s_scan_json[0] = '[';
    for (int i = 0; i < num; i++) {
        if (i > 0) s_scan_json[pos++] = ',';
        pos += snprintf(s_scan_json + pos, sizeof(s_scan_json) - pos,
                        "\"%s\"", (char *)aps[i].ssid);
        if (pos >= sizeof(s_scan_json) - 10) break;
    }
    s_scan_json[pos++] = ']';
    s_scan_json[pos] = '\0';
    free(aps);

    ESP_LOGI(TAG, "WiFi scan cached: %d networks", num);
}

/* ── DNS hijack server ───────────────────────────────────────────────
 * Listens on UDP:53, responds to every A query with 192.168.4.1.
 * Killed by handle_save() before esp_restart() so it never runs
 * after the device switches to STA mode.
 * ------------------------------------------------------------------ */

static TaskHandle_t s_dns_task = NULL;

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* 1-second receive timeout so the task yields periodically */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = inet_addr("192.168.4.1"),  /* AP interface only */
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* 192.168.4.1 as 4 bytes */
    uint8_t portal_ip[4] = {192, 168, 4, 1};

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    ESP_LOGI(TAG, "DNS hijack server running");

    while (true) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 0) continue;   /* timeout or error — loop and yield */
        if (len < 12) continue;  /* too short to be a valid DNS query */

        /* Build response in-place:
         *   - Copy transaction ID (bytes 0-1) unchanged
         *   - Set flags: response, no error (0x8180)
         *   - 1 question, 1 answer, 0 authority, 0 additional
         *   - Append answer: name ptr → type A → class IN → TTL 0 → 4 bytes IP
         */
        buf[2]  = 0x81; buf[3]  = 0x80;  /* flags: standard response */
        buf[4]  = 0x00; buf[5]  = 0x01;  /* questions = 1 */
        buf[6]  = 0x00; buf[7]  = 0x01;  /* answers   = 1 */
        buf[8]  = 0x00; buf[9]  = 0x00;  /* authority = 0 */
        buf[10] = 0x00; buf[11] = 0x00;  /* additional= 0 */

        /* Find end of question section (skip QNAME + QTYPE + QCLASS) */
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            int label_len = buf[pos];
            if (pos + label_len + 1 > len) break;
            pos += label_len + 1;
        }
        if (pos >= len) continue;
        pos += 5;  /* skip null label + QTYPE(2) + QCLASS(2) */

        if (pos + 16 > (int)sizeof(buf)) continue;  /* won't fit */

        /* Answer record */
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;  /* name ptr → offset 12 */
        buf[pos++] = 0x00; buf[pos++] = 0x01;  /* type  A  */
        buf[pos++] = 0x00; buf[pos++] = 0x01;  /* class IN */
        buf[pos++] = 0x00; buf[pos++] = 0x00;  /* TTL      */
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = 0x04;  /* rdlength = 4 */
        buf[pos++] = portal_ip[0];
        buf[pos++] = portal_ip[1];
        buf[pos++] = portal_ip[2];
        buf[pos++] = portal_ip[3];

        sendto(sock, buf, pos, 0, (struct sockaddr *)&client, clen);
    }

    close(sock);
    vTaskDelete(NULL);
}

/* --------------- HTML --------------- */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Treadmill Setup</title>"
    "<style>"
    "body{background:#0F1120;color:#E0E0E0;font-family:system-ui;margin:0;padding:20px;}"
    ".card{background:#1A1D35;border-radius:12px;padding:24px;max-width:380px;margin:40px auto;}"
    "h1{color:#667EEA;font-size:20px;margin:0 0 4px;}"
    "h2{color:#888;font-size:12px;margin:0 0 20px;font-weight:normal;}"
    "h3{color:#667EEA;font-size:14px;margin:20px 0 4px;border-top:1px solid #333;padding-top:16px;}"
    "label{display:block;color:#888;font-size:12px;margin:12px 0 4px;}"
    "select,input[type=text],input[type=password],input[type=number]{width:100%;padding:10px;background:#252840;border:1px solid #333;border-radius:8px;"
    "color:#E0E0E0;font-size:14px;box-sizing:border-box;}"
    "button{width:100%;padding:12px;background:#667EEA;color:#fff;border:none;border-radius:8px;"
    "font-size:15px;font-weight:600;cursor:pointer;margin-top:20px;}"
    "button:disabled{background:#444;}"
    ".status{color:#4ADE80;font-size:13px;margin-top:12px;text-align:center;}"
    ".err{color:#F87171;}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>Treadmill Bridge</h1>"
    "<h2>WiFi &amp; MQTT Setup</h2>"
    "<form id='f' method='POST' action='/save'>"
    "<label>WiFi Network (SSID)</label>"
    "<select name='ssid' id='ssid'><option>Scanning...</option></select>"
    "<label>WiFi Password</label>"
    "<input type='password' name='pass' id='pass' placeholder='WiFi password'>"
    "<h3>MQTT Broker</h3>"
    "<label>MQTT Host</label>"
    "<input type='text' name='mqtt_host' id='mqtt_host' placeholder='192.168.1.10'>"
    "<label>MQTT Port</label>"
    "<input type='number' name='mqtt_port' id='mqtt_port' value='1883'>"
    "<label>MQTT Username (optional)</label>"
    "<input type='text' name='mqtt_user' id='mqtt_user' placeholder='username'>"
    "<label>MQTT Password (optional)</label>"
    "<input type='password' name='mqtt_pass' id='mqtt_pass' placeholder='password'>"
    "<label>Topic Prefix</label>"
    "<input type='text' name='topic_prefix' id='topic_prefix' value='treadmill'>"
    "<button type='submit' id='btn'>Save &amp; Connect</button>"
    "</form>"
    "<div class='status' id='st'></div>"
    "</div>"
    "<script>"
    "fetch('/scan').then(r=>r.json()).then(d=>{"
    "let s=document.getElementById('ssid');s.innerHTML='';"
    "d.forEach(n=>{let o=document.createElement('option');o.value=n;o.textContent=n;s.appendChild(o);});"
    "}).catch(()=>{document.getElementById('st').textContent='Scan failed';});"
    "document.getElementById('f').onsubmit=function(e){"
    "e.preventDefault();let b=document.getElementById('btn');b.disabled=true;b.textContent='Saving...';"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(document.getElementById('ssid').value)"
    "+'&pass='+encodeURIComponent(document.getElementById('pass').value)"
    "+'&mqtt_host='+encodeURIComponent(document.getElementById('mqtt_host').value)"
    "+'&mqtt_port='+encodeURIComponent(document.getElementById('mqtt_port').value)"
    "+'&mqtt_user='+encodeURIComponent(document.getElementById('mqtt_user').value)"
    "+'&mqtt_pass='+encodeURIComponent(document.getElementById('mqtt_pass').value)"
    "+'&topic_prefix='+encodeURIComponent(document.getElementById('topic_prefix').value)"
    "}).then(r=>r.text()).then(t=>{document.getElementById('st').textContent=t;});"
    "};"
    "</script></body></html>";

/* --------------- URL decode helper --------------- */

static size_t url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

/* --------------- Form field parser --------------- */

/**
 * Extract a URL-encoded form field value from a POST body.
 * e.g. parse_field("ssid=hello&pass=world", "pass", buf, 64)
 * Returns true if field found.
 */
static bool parse_field(char *body, const char *name, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t nlen = strlen(name);

    /* Try to find name= at start or after & */
    char *pos = body;
    while ((pos = strstr(pos, name)) != NULL) {
        /* Ensure it's at start or preceded by '&' */
        if (pos != body && *(pos - 1) != '&') {
            pos += nlen;
            continue;
        }
        /* Ensure next char is '=' */
        if (pos[nlen] != '=') {
            pos += nlen;
            continue;
        }
        break;
    }
    if (!pos) return false;

    char *val = pos + nlen + 1;  /* skip "name=" */
    char *end = strchr(val, '&');
    char saved = 0;
    if (end) { saved = *end; *end = '\0'; }

    url_decode(val, out, out_size);

    if (end) *end = saved;  /* restore */
    return true;
}

/* --------------- HTTP handlers --------------- */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, s_scan_json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_save(httpd_req_t *req)
{
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[len] = 0;

    char ssid[33] = {0}, pass[65] = {0};
    char mqtt_host[65] = {0}, mqtt_port[8] = {0};
    char mqtt_user[65] = {0}, mqtt_pass[65] = {0};
    char topic_prefix[33] = {0};

    parse_field(body, "ssid", ssid, sizeof(ssid));
    parse_field(body, "pass", pass, sizeof(pass));
    parse_field(body, "mqtt_host", mqtt_host, sizeof(mqtt_host));
    parse_field(body, "mqtt_port", mqtt_port, sizeof(mqtt_port));
    parse_field(body, "mqtt_user", mqtt_user, sizeof(mqtt_user));
    parse_field(body, "mqtt_pass", mqtt_pass, sizeof(mqtt_pass));
    parse_field(body, "topic_prefix", topic_prefix, sizeof(topic_prefix));

    if (strlen(ssid) == 0) {
        return httpd_resp_send(req, "Please select a network", HTTPD_RESP_USE_STRLEN);
    }

    /* Store WiFi credentials */
    nvs_config_set_wifi(ssid, pass);
    ESP_LOGI(TAG, "WiFi saved: %s", ssid);

    /* Store MQTT broker settings */
    if (strlen(mqtt_host) > 0) {
        nvs_config_set_mqtt_host(mqtt_host);
        ESP_LOGI(TAG, "MQTT host saved: %s", mqtt_host);
    }
    if (strlen(mqtt_port) > 0) {
        int port = atoi(mqtt_port);
        if (port > 0 && port <= 65535) {
            nvs_config_set_mqtt_port(port);
            ESP_LOGI(TAG, "MQTT port saved: %d", port);
        }
    }
    if (strlen(mqtt_user) > 0 || strlen(mqtt_pass) > 0) {
        nvs_config_set_mqtt_creds(mqtt_user, mqtt_pass);
        ESP_LOGI(TAG, "MQTT credentials saved");
    }
    if (strlen(topic_prefix) > 0) {
        nvs_config_set_topic_prefix(topic_prefix);
        ESP_LOGI(TAG, "Topic prefix saved: %s", topic_prefix);
    }

    /* Switch to RUN mode on next boot */
    nvs_config_set_op_mode(OP_MODE_RUN);

    httpd_resp_send(req, "Saved! Rebooting...", HTTPD_RESP_USE_STRLEN);
    /* Kill DNS server before rebooting into STA mode */
    if (s_dns_task) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/* --------------- Public API --------------- */

void captive_portal_start(void)
{
    const char *ap_ssid = CONFIG_AP_SSID;
    const char *ap_pass = CONFIG_AP_PASS;

    ESP_LOGI(TAG, "Starting captive portal AP: %s", ap_ssid);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = PORTAL_AP_CHANNEL;
    ap_config.ap.max_connection = PORTAL_MAX_CONN;

    if (strlen(ap_pass) >= 8) {
        strncpy((char *)ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(CONFIG_TX_POWER_QDBM);

    do_scan_and_cache();

    /* Start DNS hijack task */
    xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    /* Start HTTP server */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 10;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t uri_root  = { .uri = "/",     .method = HTTP_GET,  .handler = handle_root };
    httpd_uri_t uri_scan  = { .uri = "/scan", .method = HTTP_GET,  .handler = handle_scan };
    httpd_uri_t uri_save  = { .uri = "/save", .method = HTTP_POST, .handler = handle_save };
    httpd_uri_t uri_catch = { .uri = "/*",    .method = HTTP_GET,  .handler = handle_redirect };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_uri_handler(server, &uri_save);
    httpd_register_uri_handler(server, &uri_catch);

    ESP_LOGI(TAG, "Captive portal running at http://192.168.4.1/");

    /* Block forever (reboot happens in handle_save) */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
