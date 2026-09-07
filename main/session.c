/**
 * session.c — session layer: public API, shared JSON dispatch, and the
 * WebSocket transport backend (docs/websocket.md).
 *
 * Two transports are compiled in; the active one is chosen at connect time
 * from what the OTA response provides:
 *   - MQTT+UDP (docs/mqtt-udp.md) when `mqtt.endpoint` is present — this is
 *     what api.tenclass.net actually serves (the WebSocket gateway closes
 *     every connection); see session_mqtt.c.
 *   - WebSocket otherwise (e.g. self-hosted servers).
 *
 * Binary protocol version 1 (raw Opus frames both ways). JSON messages are
 * dispatched by the "type" field per §4.2. Handshake: client hello with
 * 16 kHz / mono / CONFIG_OPUS_FRAME_DURATION_MS opus params; waits for the
 * server hello (transport must be "websocket"), stores session_id, and
 * (re)initializes the opus decoder at the server's announced sample rate.
 */
#include "session_priv.h"
#include "app.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>

#include "esp_system.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "session";
static const char *PROTOCOL_VERSION = "1";

/* ---- websocket backend state (shared with on_server_hello above) ---- */
static esp_websocket_client_handle_t s_client;
static EventGroupHandle_t s_events;
#define EV_HELLO_OK  BIT0
#define EV_CLOSED    BIT1

static char s_session_id[64];
static bool s_channel_open;
static bool s_listening;          /* server-side listen state we last announced */

/* ---- shared: JSON dispatch ---- */
static session_hello_fn_t s_hello_fn;

void session_set_hello_handler(session_hello_fn_t fn) { s_hello_fn = fn; }

static void on_server_hello(const cJSON *json)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(json, "session_id");
    if (sid && cJSON_IsString(sid))
        strlcpy(s_session_id, cJSON_GetStringValue(sid), sizeof(s_session_id));

    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(json, "audio_params");
    int rate = 16000;
    if (ap) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(ap, "sample_rate");
        if (cJSON_IsNumber(r)) rate = r->valueint;
    }
    const cJSON *transport = cJSON_GetObjectItemCaseSensitive(json, "transport");
    if (!transport || !cJSON_IsString(transport) ||
        strcmp(cJSON_GetStringValue(transport), "websocket") != 0) {
        ESP_LOGE(TAG, "server hello transport mismatch");
        return;
    }
    opus_decoder_setup(rate);
    ESP_LOGI(TAG, "server hello ok, session=%s, rate=%d", s_session_id, rate);
    s_channel_open = true;
    xEventGroupSetBits(s_events, EV_HELLO_OK);
}

void session_dispatch_json(const char *data, size_t len)
{
    static char scratch[4096];
    if (len >= sizeof(scratch)) { ESP_LOGE(TAG, "json too long (%d)", (int)len); return; }
    memcpy(scratch, data, len);
    scratch[len] = 0;

    cJSON *json = cJSON_Parse(scratch);
    if (!json) { ESP_LOGE(TAG, "bad json"); return; }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    if (!type || !cJSON_IsString(type)) {
        ESP_LOGE(TAG, "Missing message type, data: %.64s", scratch);
        cJSON_Delete(json);
        return;
    }
    const char *t = cJSON_GetStringValue(type);

    if (!strcmp(t, "hello")) {
        if (s_hello_fn) s_hello_fn(json);
    } else if (!strcmp(t, "stt")) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(json, "text");
        if (cJSON_IsString(text)) {
            ESP_LOGI(TAG, "STT: %s", cJSON_GetStringValue(text));
            app_post_event(APP_EVENT_STT, cJSON_GetStringValue(text));
        }
    } else if (!strcmp(t, "tts")) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(json, "state");
        const char *st = cJSON_IsString(state) ? cJSON_GetStringValue(state) : "";
        if (!strcmp(st, "start")) {
            app_post_event(APP_EVENT_TTS_START, NULL);
        } else if (!strcmp(st, "stop")) {
            app_post_event(APP_EVENT_TTS_STOP, NULL);
        } else if (!strcmp(st, "sentence_start")) {
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(json, "text");
            if (cJSON_IsString(text))
                app_post_event(APP_EVENT_TTS_TEXT, cJSON_GetStringValue(text));
        }
    } else if (!strcmp(t, "llm")) {
        const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(json, "emotion");
        if (cJSON_IsString(emotion))
            app_post_event(APP_EVENT_EMOTION, cJSON_GetStringValue(emotion));
    } else if (!strcmp(t, "mcp")) {
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(json, "payload");
        if (cJSON_IsObject(payload)) {
            char *s = cJSON_PrintUnformatted(payload);
            if (s) { mcp_handle_payload(s, strlen(s)); cJSON_free(s); }
        }
    } else if (!strcmp(t, "system")) {
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "command");
        if (cJSON_IsString(cmd) && !strcmp(cJSON_GetStringValue(cmd), "reboot")) {
            ESP_LOGW(TAG, "server requested reboot");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    } else if (!strcmp(t, "alert")) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(json, "message");
        ESP_LOGW(TAG, "alert: %.64s", cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : "");
        app_post_event(APP_EVENT_ALERT, cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : "alert");
    } else if (!strcmp(t, "goodbye")) {
        /* server-initiated teardown of the audio session */
        ESP_LOGI(TAG, "server goodbye");
        app_post_event(APP_EVENT_WS_CLOSED, NULL);
    } else {
        ESP_LOGD(TAG, "ignored json type: %s", t);
    }
    cJSON_Delete(json);
}

/* ---- websocket backend ---- */

/* reassembly for fragmented text frames */
static char s_json_buf[4096];
static size_t s_json_len, s_json_total;

/* ---- sending ---- */
static void send_json_str(const char *s)
{
    if (!s_client) return;
    esp_websocket_client_send_text(s_client, s, strlen(s), portMAX_DELAY);
    ESP_LOGD(TAG, ">> %s", s);
}

static void send_json(const char *fmt, ...)
{
    static char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send_json_str(buf);
}

static const char *session_id_or_empty(void)
{
    return s_session_id[0] ? s_session_id : "";
}

/* ---- hello handshake ---- */
static void send_hello(void)
{
    send_json("{\"type\":\"hello\",\"version\":%s,"
              "\"features\":{\"mcp\":true},"
              "\"transport\":\"websocket\","
              "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
              "\"channels\":1,\"frame_duration\":%d}}",
              PROTOCOL_VERSION, CONFIG_OPUS_FRAME_DURATION_MS);
}

/* ---- incoming websocket frames ---- */
static void ws_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *d = event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        send_hello();
        ESP_LOGI(TAG, "hello sent, waiting for server hello");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->data_len && d->op_code != 0x2)
            ESP_LOGI(TAG, "ws rx op=%d: %.*s", d->op_code, d->data_len > 80 ? 80 : d->data_len, d->data_ptr);
        if (d->op_code == 0x1 || (!d->op_code && s_json_len)) {   /* text frame(s) */
            if (d->payload_offset == 0) { s_json_len = 0; s_json_total = d->payload_len; }
            if (d->data_len && s_json_len + d->data_len < sizeof(s_json_buf)) {
                memcpy(s_json_buf + s_json_len, d->data_ptr, d->data_len);
                s_json_len += d->data_len;
            }
            if (s_json_len >= s_json_total && s_json_len) {
                session_dispatch_json(s_json_buf, s_json_len);
                s_json_len = s_json_total = 0;
            }
        } else if (d->op_code == 0x2) {                           /* binary opus frame */
            if (s_listening) {   /* spec: frames arriving while listening are dropped */
                ESP_LOGD(TAG, "dropping downlink audio while listening");
                break;
            }
            audio_play((const uint8_t *)d->data_ptr, d->data_len, 0);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "ws disconnected");
        if (s_channel_open) {
            s_channel_open = false;
            s_listening = false;
            xEventGroupSetBits(s_events, EV_CLOSED);
            app_post_event(APP_EVENT_WS_CLOSED, NULL);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "ws error");
        s_channel_open = false;
        s_listening = false;
        xEventGroupSetBits(s_events, EV_CLOSED);
        break;
    default:
        break;
    }
}

/* ---- websocket backend API ---- */
bool ws_is_open(void) { return s_channel_open; }

int ws_start(void)
{
    if (s_channel_open) return 0;
    if (!g_ota_info.websocket_url[0]) { ESP_LOGE(TAG, "no websocket url"); return -1; }

    s_session_id[0] = 0;
    s_json_len = s_json_total = 0;
    xEventGroupClearBits(s_events, EV_HELLO_OK | EV_CLOSED);
    session_set_hello_handler(on_server_hello);

    if (s_client) { esp_websocket_client_destroy(s_client); s_client = NULL; }

    static char headers[700];
    snprintf(headers, sizeof(headers),
             "Authorization:Bearer %s\r\nProtocol-Version:%s\r\n"
             "Device-Id:%s\r\nClient-Id:%s\r\n",
             g_ota_info.token, PROTOCOL_VERSION, app_device_id(), app_client_id());

    esp_websocket_client_config_t cfg = {
        .uri = g_ota_info.websocket_url,
        .headers = headers,
        .buffer_size = 4096,
        .reconnect_timeout_ms = INT32_MAX,   /* app decides reconnects */
        .network_timeout_ms = CONFIG_XZ_SESSION_TIMEOUT_MS,
        /* server verification via the built-in x509 bundle, same as ota.c */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return -1;
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_client) != ESP_OK) return -1;

    EventBits_t bits = xEventGroupWaitBits(s_events, EV_HELLO_OK | EV_CLOSED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_XZ_SESSION_TIMEOUT_MS));
    if (bits & EV_HELLO_OK) return 0;
    ESP_LOGE(TAG, "server hello timeout");
    ws_stop();
    return -1;
}

void ws_stop(void)
{
    s_channel_open = false;
    s_listening = false;
    if (s_client) {
        esp_websocket_client_close(s_client, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
}

void ws_start_listening(void)
{
    if (!s_channel_open || s_listening) return;
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\","
              "\"mode\":\"manual\"}", session_id_or_empty());
    s_listening = true;
}

void ws_stop_listening(void)
{
    if (!s_channel_open || !s_listening) return;
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
              session_id_or_empty());
    s_listening = false;
}

void ws_send_abort(void)
{
    if (!s_channel_open) return;
    send_json("{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"wake_word_detected\"}",
              session_id_or_empty());
}

void ws_send_audio(const uint8_t *opus, size_t len)
{
    if (!s_channel_open || !s_client) return;
    esp_websocket_client_send_bin(s_client, (const char *)opus, len, portMAX_DELAY);
}

void ws_send_mcp(const char *payload_json)
{
    send_json("{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
              session_id_or_empty(), payload_json);
}

/* ---- public API: backend selection ---- */
static bool s_use_mqtt;

int session_start(void)
{
    s_use_mqtt = g_ota_info.mqtt_endpoint[0] != 0;
    if (s_use_mqtt) {
        ESP_LOGI(TAG, "transport: MQTT+UDP (endpoint %s)", g_ota_info.mqtt_endpoint);
        return mqttsess_start();
    }
    ESP_LOGI(TAG, "transport: WebSocket (%s)", g_ota_info.websocket_url);
    return ws_start();
}

void session_stop(void)
{
    if (s_use_mqtt) mqttsess_stop();
    else ws_stop();
}

void session_start_listening(void)
{
    if (s_use_mqtt) mqttsess_start_listening();
    else ws_start_listening();
}

void session_stop_listening(void)
{
    if (s_use_mqtt) mqttsess_stop_listening();
    else ws_stop_listening();
}

void session_send_abort(void)
{
    if (s_use_mqtt) mqttsess_send_abort();
    else ws_send_abort();
}

void session_send_audio(const uint8_t *opus, size_t len)
{
    if (s_use_mqtt) mqttsess_send_audio(opus, len);
    else ws_send_audio(opus, len);
}

void session_send_mcp(const char *payload_json)
{
    if (s_use_mqtt) mqttsess_send_mcp(payload_json);
    else ws_send_mcp(payload_json);
}

bool session_is_open(void)
{
    return s_use_mqtt ? mqttsess_is_open() : ws_is_open();
}

void session_init(void)
{
    s_events = xEventGroupCreate();
    mqttsess_init();
}