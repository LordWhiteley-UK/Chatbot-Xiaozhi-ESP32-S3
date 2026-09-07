/**
 * ota.c — device activation / endpoint discovery.
 *
 * POSTs the device identity to CONFIG_OTA_URL; the response supplies the
 * WebSocket endpoint + access token and, for a not-yet-activated device,
 * a short numeric code to display on the OLED (entered at xiaozhi.me).
 *
 * NOTE: the response schema is NOT covered by the three protocol docs in
 * docs/ — parsing below is deliberately lenient (fields are looked up by
 * name wherever they appear) and flagged to the user for confirmation.
 */
#include "app.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ota";
ota_info_t g_ota_info;

/* app_firmware_version() is provided by board.c */

static void find_field(cJSON *root, const char *dotted, char *out, size_t out_len)
{
    /* walk "a.b.c" through nested objects; also try top-level "b" directly */
    char path[64];
    strlcpy(path, dotted, sizeof(path));
    cJSON *node = root;
    char *save = NULL;
    for (char *tok = strtok_r(path, ".", &save); tok && node; tok = strtok_r(NULL, ".", &save))
        node = cJSON_GetObjectItemCaseSensitive(node, tok);
    if (node && cJSON_IsString(node))
        strlcpy(out, cJSON_GetStringValue(node), out_len);
}

void ota_clear(void)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "activated");
        nvs_commit(h);
        nvs_close(h);
    }
}

bool ota_is_activated(void)
{
    nvs_handle_t h;
    bool on = false;
    uint8_t v = 0;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "activated", &v) == ESP_OK) on = v != 0;
        nvs_close(h);
    }
    return on;
}

static void ota_set_activated(bool on)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "activated", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

int ota_fetch(ota_info_t *out)
{
    memset(out, 0, sizeof(*out));

    char body[256];
    snprintf(body, sizeof(body),
             "{\"mac\":\"%s\",\"device_id\":\"%s\",\"application\":"
             "{\"version\":\"%s\",\"compile_time\":\"%s %s\"}}",
             app_device_id(), app_device_id(), app_firmware_version(),
             __DATE__, __TIME__);

    esp_http_client_config_t cfg = {
        .url = CONFIG_OTA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        /* server verification via the built-in x509 bundle (CONFIG
           MBEDTLS_CERTIFICATE_BUNDLE); without this https init fails */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", app_device_id());
    esp_http_client_set_header(client, "Client-Id", app_client_id());
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_post_field(client, body, strlen(body));

    /* response can be up to a few KB */
    char *resp = malloc(8192);
    if (!resp) { esp_http_client_cleanup(client); return -1; }
    int resp_len = 0;
    /* open/write/fetch_headers/read flow — perform() discards the body,
       so esp_http_client_read_response would return 0 bytes after it */
    int wlen = strlen(body);
    esp_err_t err = esp_http_client_open(client, wlen);
    if (err == ESP_OK) {
        esp_http_client_write(client, body, wlen);
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        resp_len = esp_http_client_read_response(client, resp, 8191);
        resp[resp_len > 0 ? resp_len : 0] = 0;
        ESP_LOGI(TAG, "OTA status %d, %d bytes", status, resp_len);
        if (status >= 200 && status < 300 && resp_len > 0) {
            cJSON *root = cJSON_Parse(resp);
            if (root) {
                find_field(root, "websocket.url", out->websocket_url, sizeof(out->websocket_url));
                find_field(root, "websocket.token", out->token, sizeof(out->token));
                find_field(root, "firmware.version", out->firmware_version, sizeof(out->firmware_version));
                /* MQTT+UDP transport credentials (docs/mqtt-udp.md §6.1).
                   NOTE: the server appends the OTA request's Client-Id header
                   to mqtt.client_id, so that header must be sent (below). */
                find_field(root, "mqtt.endpoint", out->mqtt_endpoint, sizeof(out->mqtt_endpoint));
                find_field(root, "mqtt.client_id", out->mqtt_client_id, sizeof(out->mqtt_client_id));
                find_field(root, "mqtt.username", out->mqtt_username, sizeof(out->mqtt_username));
                find_field(root, "mqtt.password", out->mqtt_password, sizeof(out->mqtt_password));
                find_field(root, "mqtt.publish_topic", out->mqtt_publish_topic, sizeof(out->mqtt_publish_topic));
                find_field(root, "mqtt.subscribe_topic", out->mqtt_subscribe_topic, sizeof(out->mqtt_subscribe_topic));
                /* activation code: string field "code" under "activation" */
                char code[16] = {0}, msg[64] = {0};
                find_field(root, "activation.code", code, sizeof(code));
                find_field(root, "activation.message", msg, sizeof(msg));
                if (code[0]) {
                    strlcpy(out->activation_code, code, sizeof(out->activation_code));
                    out->needs_activation = true;
                    ESP_LOGI(TAG, "device not activated: code=%s (%s)", code, msg);
                } else if (!ota_is_activated()) {
                    ota_set_activated(true);
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "OTA response not JSON");
            }
        }
    } else {
        ESP_LOGE(TAG, "OTA request failed: %s", esp_err_to_name(err));
    }
    free(resp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (out->websocket_url[0] || out->activation_code[0]) ? 0 : -1;
}