/**
 * @file mqtt_manager.c
 * @brief MQTT client with Home Assistant discovery for iFit treadmill telemetry.
 *
 * esp_mqtt_client based. Publishes Home Assistant MQTT discovery configs for six
 * entities (Speed, Incline, Elapsed, Distance, Moving, Connectivity) once on
 * connect, then publishes telemetry samples at the poll interval.
 *
 * A Last Will & Testament marks the device offline on the availability topic
 * ("<prefix>/availability"); "online" is published on connect.
 */

#include "mqtt_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt_manager";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

/* Device ID based on MAC address (e.g. "treadmill_d0cf132fdfdc"). */
static char device_id[40] = {0};
static uint8_t device_mac[6] = {0};

/* Topic prefix (e.g. "treadmill") used for telemetry + availability topics. */
static char topic_prefix[48] = {0};
static char availability_topic[80] = {0};

/* Object IDs for each entity (used as the trailing topic segment + unique_id). */
#define OBJ_SPEED      "speed"
#define OBJ_INCLINE    "incline"
#define OBJ_ELAPSED    "elapsed"
#define OBJ_DISTANCE   "distance"
#define OBJ_MOVING     "moving"
#define OBJ_CONNECTED  "connected"

/* km/h -> mph conversion factor. */
#define KMH_TO_MPH 0.6214f

/* Generate the unique device ID + availability topic from the MAC address. */
static void generate_device_id(void)
{
    esp_efuse_mac_get_default(device_mac);

    snprintf(device_id, sizeof(device_id), "treadmill_%02x%02x%02x%02x%02x%02x",
             device_mac[0], device_mac[1], device_mac[2],
             device_mac[3], device_mac[4], device_mac[5]);

    snprintf(availability_topic, sizeof(availability_topic),
             "%s/availability", topic_prefix);

    ESP_LOGI(TAG, "Device ID: %s", device_id);
    ESP_LOGI(TAG, "MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
             device_mac[0], device_mac[1], device_mac[2],
             device_mac[3], device_mac[4], device_mac[5]);
    ESP_LOGI(TAG, "Availability topic: %s", availability_topic);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        mqtt_connected = true;
        /* Announce availability immediately (retained) and (re)publish the HA
         * discovery configs so the entities reappear after a broker restart. */
        esp_mqtt_client_publish(mqtt_client, availability_topic, "online", 0, 1, 1);
        mqtt_manager_publish_discovery();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        mqtt_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

esp_err_t mqtt_manager_init(const char *host, int port,
                            const char *user, const char *pass,
                            const char *prefix)
{
    /* Capture the topic prefix first; generate_device_id() needs it. */
    if (prefix != NULL && prefix[0] != '\0') {
        snprintf(topic_prefix, sizeof(topic_prefix), "%s", prefix);
    } else {
        snprintf(topic_prefix, sizeof(topic_prefix), "treadmill");
    }

    generate_device_id();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = host,
        .broker.address.port = port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        /* Last Will & Testament: broker publishes "offline" (retained) if we
         * drop without a clean disconnect. */
        .session.last_will.topic = availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 0,   /* 0 => strlen() */
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    if (user != NULL && user[0] != '\0') {
        mqtt_cfg.credentials.username = user;
    }
    if (pass != NULL && pass[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = pass;
    }

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started, broker: %s:%d, prefix: %s",
             host ? host : "(null)", port, topic_prefix);
    return ESP_OK;
}

/*
 * Publish one Home Assistant discovery config (retained).
 *
 * component   "sensor" or "binary_sensor"
 * object_id   entity object id (also the state-topic trailing segment)
 * name        friendly name (becomes the entity name)
 * unit        unit_of_measurement, or NULL to omit
 * device_class HA device_class, or NULL to omit
 * state_class HA state_class (e.g. "measurement", "total_increasing"), or NULL
 */
static void publish_discovery_entity(const char *component,
                                     const char *object_id,
                                     const char *name,
                                     const char *unit,
                                     const char *device_class,
                                     const char *state_class)
{
    if (mqtt_client == NULL) {
        return;
    }

    char topic[160];
    char payload[640];

    /* homeassistant/<component>/<device_id>/<object_id>/config */
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config",
             component, device_id, object_id);

    int n = snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"state_topic\":\"%s/%s/state\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"unique_id\":\"%s_%s\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"iFit Treadmill\","
            "\"manufacturer\":\"NordicTrack\","
            "\"model\":\"iFit I_TL\","
            "\"connections\":[[\"mac\",\"%02x:%02x:%02x:%02x:%02x:%02x\"]]"
        "}",
        name,
        topic_prefix, object_id,
        availability_topic,
        device_id, object_id,
        device_id,
        device_mac[0], device_mac[1], device_mac[2],
        device_mac[3], device_mac[4], device_mac[5]);

    if (n < 0 || n >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "Discovery payload overflow for %s", object_id);
        return;
    }

    if (unit != NULL && unit[0] != '\0') {
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"unit_of_measurement\":\"%s\"", unit);
    }
    if (device_class != NULL && device_class[0] != '\0') {
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"device_class\":\"%s\"", device_class);
    }
    if (state_class != NULL && state_class[0] != '\0') {
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"state_class\":\"%s\"", state_class);
    }
    /* binary_sensor on/off payloads (HA defaults are ON/OFF). */
    if (strcmp(component, "binary_sensor") == 0) {
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"");
    }

    if (n < 0 || n >= (int)sizeof(payload) - 1) {
        ESP_LOGE(TAG, "Discovery payload overflow for %s", object_id);
        return;
    }

    strcat(payload, "}");

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish discovery for %s", object_id);
        return;
    }
    ESP_LOGI(TAG, "Published discovery for %s", object_id);
}

void mqtt_manager_publish_discovery(void)
{
    /* Sensors */
    publish_discovery_entity("sensor", OBJ_SPEED, "Speed",
                             "mph", "speed", "measurement");
    publish_discovery_entity("sensor", OBJ_INCLINE, "Incline",
                             "%", NULL, "measurement");
    publish_discovery_entity("sensor", OBJ_ELAPSED, "Elapsed",
                             "s", "duration", "measurement");
    publish_discovery_entity("sensor", OBJ_DISTANCE, "Distance",
                             "mi", "distance", "total_increasing");
    /* Binary sensors */
    publish_discovery_entity("binary_sensor", OBJ_MOVING, "Moving",
                             NULL, "running", NULL);
    publish_discovery_entity("binary_sensor", OBJ_CONNECTED, "Connectivity",
                             NULL, "connectivity", NULL);
}

static void publish_state(const char *object_id, const char *payload)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s/state", topic_prefix, object_id);
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish state to %s", topic);
    }
}

void mqtt_manager_publish_telemetry(const treadmill_telemetry_t *t)
{
    if (t == NULL) {
        return;
    }
    if (!mqtt_connected || mqtt_client == NULL) {
        return;
    }

    char payload[32];

    /* Speed: internal km/h -> mph. */
    float speed_mph = t->speed_kmh * KMH_TO_MPH;
    snprintf(payload, sizeof(payload), "%.2f", speed_mph);
    publish_state(OBJ_SPEED, payload);

    /* Incline: percent grade. */
    snprintf(payload, sizeof(payload), "%.1f", t->incline_pct);
    publish_state(OBJ_INCLINE, payload);

    /* Elapsed: seconds. */
    snprintf(payload, sizeof(payload), "%u", (unsigned)t->elapsed_s);
    publish_state(OBJ_ELAPSED, payload);

    /* Distance: integrate speed (mph) over elapsed time -> miles. */
    float distance_mi = speed_mph * ((float)t->elapsed_s / 3600.0f);
    snprintf(payload, sizeof(payload), "%.3f", distance_mi);
    publish_state(OBJ_DISTANCE, payload);

    /* Moving (binary). */
    publish_state(OBJ_MOVING, t->moving ? "ON" : "OFF");

    /* Connectivity (binary). */
    publish_state(OBJ_CONNECTED, t->connected ? "ON" : "OFF");
}

bool mqtt_manager_is_connected(void)
{
    return mqtt_connected;
}
