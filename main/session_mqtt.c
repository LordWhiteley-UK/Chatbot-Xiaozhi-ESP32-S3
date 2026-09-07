/**
 * session_mqtt.c — MQTT+UDP transport backend (docs/mqtt-udp.md).
 *
 * MQTT (TLS) carries JSON control messages; a UDP socket carries AES-128-CTR
 * encrypted Opus audio. Handshake: MQTT connect -> publish hello (version 3,
 * transport "udp") -> server hello carries the session id, the UDP endpoint
 * and the AES key + nonce.
 *
 * Interpretations of underspecified points, verified empirically against
 * api.tenclass.net on 2026-09-07 (docs don't pin these down — flagged to the
 * user per the project ground rules):
 *   1. Broker port 8883 when the OTA `mqtt.endpoint` carries no port.
 *   2. Server->device topic is "devices/p2p/<mac-without-colons>": the OTA
 *      response's `mqtt.subscribe_topic` was literally the string "null",
 *      and the observed traffic arrives on the derived topic.
 *   3. The 16-byte audio packet header doubles as the AES-CTR IV: the server
 *      nonce is a template into which payload_len (offset 2), timestamp
 *      (offset 8) and sequence (offset 12) are written before encryption;
 *      the same 16 bytes are sent in the clear as the header.
 *   4. Each conversation round is armed by a listen "detect" message; with
 *      an empty text the server transcribes the uplink audio itself.
 */
#include "session_priv.h"
#include "app.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "psa/crypto.h"   /* mbedtls 4 (IDF v6): AES via PSA — hw-accelerated on S3 */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "mqtt_udp";

#define EV_HELLO_OK  BIT0
#define EV_CLOSED    BIT1

static esp_mqtt_client_handle_t s_mqtt;
static EventGroupHandle_t s_ev;

static char s_session_id[64];
static volatile bool s_open;
static volatile bool s_listening;

static char s_pub_topic[64];
static char s_sub_topic[80];

static int s_sock = -1;
static psa_key_id_t s_aes_key;
static bool s_aes_ready;
static uint8_t s_nonce[16];
static uint32_t s_tx_seq;
static uint32_t s_rx_seq;
static volatile int s_downlink_rate;

/* reassembly for MQTT messages that arrive in multiple data events */
static char s_json_buf[4096];
static size_t s_json_len;

/* ---- helpers ---- */
static void send_json_str(const char *s)
{
    if (!s_mqtt || !s_pub_topic[0]) return;
    esp_mqtt_client_publish(s_mqtt, s_pub_topic, s, 0, 0, 0);
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

/* ---- hello response: set up UDP + AES ---- */
static void udp_rx_task(void *arg);

static void udp_close(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static void on_hello(const cJSON *json)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(json, "session_id");
    if (sid && cJSON_IsString(sid))
        strlcpy(s_session_id, cJSON_GetStringValue(sid), sizeof(s_session_id));

    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(json, "audio_params");
    int rate = 24000;
    if (ap) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(ap, "sample_rate");
        if (cJSON_IsNumber(r)) rate = r->valueint;
    }

    const cJSON *udp = cJSON_GetObjectItemCaseSensitive(json, "udp");
    if (!cJSON_IsObject(udp)) { ESP_LOGE(TAG, "hello: no udp block"); return; }
    const cJSON *srv = cJSON_GetObjectItemCaseSensitive(udp, "server");
    const cJSON *prt = cJSON_GetObjectItemCaseSensitive(udp, "port");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(udp, "key");
    const cJSON *nce = cJSON_GetObjectItemCaseSensitive(udp, "nonce");
    if (!cJSON_IsString(srv) || !cJSON_IsNumber(prt) ||
        !cJSON_IsString(key) || !cJSON_IsString(nce)) {
        ESP_LOGE(TAG, "hello: incomplete udp block");
        return;
    }
    const char *server = cJSON_GetStringValue(srv);
    int port = prt->valueint;

    /* hex-decode the 128-bit key and nonce */
    uint8_t aes_key[16];
    const char *kh = cJSON_GetStringValue(key);
    const char *nh = cJSON_GetStringValue(nce);
    if (strlen(kh) != 32 || strlen(nh) != 32) {
        ESP_LOGE(TAG, "hello: bad key/nonce length");
        return;
    }
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(kh + 2 * i, "%2x", &v) != 1) { ESP_LOGE(TAG, "bad key hex"); return; }
        aes_key[i] = v;
        if (sscanf(nh + 2 * i, "%2x", &v) != 1) { ESP_LOGE(TAG, "bad nonce hex"); return; }
        s_nonce[i] = v;
    }

    if (s_aes_ready) psa_destroy_key(s_aes_key);
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 128);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_CTR);
    psa_status_t st = psa_import_key(&attrs, aes_key, sizeof(aes_key), &s_aes_key);
    psa_reset_key_attributes(&attrs);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "aes key import failed: %d", (int)st);
        return;
    }
    s_aes_ready = true;
    s_tx_seq = 0;
    s_rx_seq = 0;

    opus_decoder_setup(rate);
    s_downlink_rate = rate;

    udp_close();
    s_sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) { ESP_LOGE(TAG, "udp socket failed"); return; }

    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = lwip_htons((uint16_t)port);
    if (inet_aton(server, &dst.sin_addr) == 0) {
        /* endpoint may be a hostname */
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
        if (lwip_getaddrinfo(server, NULL, &hints, &res) != 0 || !res) {
            ESP_LOGE(TAG, "cannot resolve udp server %s", server);
            udp_close();
            return;
        }
        dst.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &dst.sin_addr, ipstr, sizeof(ipstr));
        ESP_LOGI(TAG, "udp server %s -> %s", server, ipstr);
        lwip_freeaddrinfo(res);
    }
    if (lwip_connect(s_sock, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        ESP_LOGE(TAG, "udp connect failed");
        udp_close();
        return;
    }
    int tv_ms = 2000;
    lwip_setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_ms, sizeof(tv_ms));

    /* rx pump for downlink audio */
    xTaskCreate(udp_rx_task, "udp_rx", 4096, NULL, 8, NULL);

    s_open = true;
    ESP_LOGI(TAG, "session %s ready, udp %s:%d, downlink %d Hz",
             s_session_id, server, port, rate);
    xEventGroupSetBits(s_ev, EV_HELLO_OK);
}

/* ---- downlink audio pump ---- */
static void udp_rx_task(void *arg)
{
    static uint8_t pkt[1600];
    while (s_open && s_sock >= 0) {
        int n = lwip_recv(s_sock, pkt, sizeof(pkt), 0);
        if (n < 0) continue;                     /* timeout or transient */
        if (n < 20 || pkt[0] != 0x01) continue;  /* not an audio packet */
        int plen = (pkt[2] << 8) | pkt[3];
        if (16 + plen > n) continue;             /* malformed */
        uint32_t seq = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                       ((uint32_t)pkt[14] << 8) | pkt[15];
        if (s_rx_seq && (int32_t)(seq - s_rx_seq) <= 0) continue;   /* anti-replay */
        s_rx_seq = seq;
        uint8_t plain[1600];
        size_t olen, olen2;
        /* multipart: the IV is the packet header itself, so the one-shot
           psa_cipher_decrypt (which expects a prepended IV) can't be used */
        psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
        if (psa_cipher_decrypt_setup(&op, s_aes_key, PSA_ALG_CTR) != PSA_SUCCESS ||
            psa_cipher_set_iv(&op, pkt, 16) != PSA_SUCCESS ||
            psa_cipher_update(&op, pkt + 16, plen, plain, sizeof(plain), &olen) != PSA_SUCCESS ||
            psa_cipher_finish(&op, plain + olen, sizeof(plain) - olen, &olen2) != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            ESP_LOGE(TAG, "decrypt failed");
            continue;
        }
        if (olen + olen2 != (size_t)plen) {
            ESP_LOGE(TAG, "decrypt size mismatch");
            continue;
        }
        if (s_listening) continue;               /* half-duplex: not while we talk */
        audio_play(plain, plen, s_downlink_rate);
    }
    vTaskDelete(NULL);
}

/* ---- MQTT events ---- */
static void mqtt_event(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected");
        esp_mqtt_client_subscribe(s_mqtt, s_sub_topic, 0);
        send_json("{\"type\":\"hello\",\"version\":3,\"transport\":\"udp\","
                  "\"features\":{\"mcp\":true},"
                  "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                  "\"channels\":1,\"frame_duration\":%d}}",
                  CONFIG_OPUS_FRAME_DURATION_MS);
        ESP_LOGI(TAG, "hello sent, waiting for server hello");
        break;
    case MQTT_EVENT_DATA: {
        if (!e->data_len) break;
        if (e->current_data_offset > 0) {
            /* continuation fragment of a multi-part message */
            if (s_json_len + (size_t)e->data_len < sizeof(s_json_buf)) {
                memcpy(s_json_buf + s_json_len, e->data, e->data_len);
                s_json_len += e->data_len;
            }
            if (s_json_len >= e->total_data_len) {
                session_dispatch_json(s_json_buf, s_json_len);
                s_json_len = 0;
            }
        } else if ((size_t)e->data_len < (size_t)e->total_data_len) {
            /* first fragment of a multi-part message */
            s_json_len = 0;
            if ((size_t)e->data_len < sizeof(s_json_buf)) {
                memcpy(s_json_buf, e->data, e->data_len);
                s_json_len = e->data_len;
            }
        } else {
            /* complete message in one event */
            session_dispatch_json(e->data, e->data_len);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "mqtt disconnected");
        if (s_open) {
            s_open = false;
            s_listening = false;
            udp_close();
            xEventGroupSetBits(s_ev, EV_CLOSED);
            app_post_event(APP_EVENT_WS_CLOSED, NULL);   /* app reconnects */
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error: %s",
                 e->error_handle && e->error_handle->error_type != MQTT_ERROR_TYPE_NONE
                     ? "see error_handle" : "unknown");
        if (!s_open) xEventGroupSetBits(s_ev, EV_CLOSED);
        break;
    default:
        break;
    }
}

/* ---- backend API ---- */
bool mqttsess_is_open(void) { return s_open; }

int mqttsess_start(void)
{
    if (s_open) return 0;
    const ota_info_t *o = &g_ota_info;
    if (!o->mqtt_endpoint[0]) { ESP_LOGE(TAG, "no mqtt endpoint"); return -1; }

    s_session_id[0] = 0;
    s_json_len = 0;
    s_listening = false;
    xEventGroupClearBits(s_ev, EV_HELLO_OK | EV_CLOSED);
    session_set_hello_handler(on_hello);

    /* broker uri — 8883 is the standard MQTT-over-TLS port [interpretation 1] */
    static char uri[192];
    const char *ep = o->mqtt_endpoint;
    uri[0] = 0;
    if (!strstr(ep, "://")) strlcpy(uri, "mqtts://", sizeof(uri));
    strlcat(uri, ep, sizeof(uri));
    if (!strstr(ep, "://") && !strchr(ep, ':')) strlcat(uri, ":8883", sizeof(uri));

    /* publish topic from OTA; subscribe topic derived [interpretation 2] */
    strlcpy(s_pub_topic,
            o->mqtt_publish_topic[0] ? o->mqtt_publish_topic : "device-server",
            sizeof(s_pub_topic));
    if (o->mqtt_subscribe_topic[0] && strcmp(o->mqtt_subscribe_topic, "null"))
        strlcpy(s_sub_topic, o->mqtt_subscribe_topic, sizeof(s_sub_topic));
    else {
        char mac[18];
        const char *id = app_device_id();
        size_t j = 0;
        for (size_t i = 0; id[i] && j < sizeof(mac) - 1; i++)
            mac[j++] = id[i] == ':' ? '_' : (char)tolower((unsigned char)id[i]);
        mac[j] = 0;
        snprintf(s_sub_topic, sizeof(s_sub_topic), "devices/p2p/%s", mac);
    }
    ESP_LOGI(TAG, "mqtt uri=%s pub='%s' sub='%s'", uri, s_pub_topic, s_sub_topic);
    ESP_LOGI(TAG, "mqtt cid='%s' user='%s' pass_len=%d",
             o->mqtt_client_id, o->mqtt_username, (int)strlen(o->mqtt_password));

    if (s_mqtt) { esp_mqtt_client_destroy(s_mqtt); s_mqtt = NULL; }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials = {
            .client_id = o->mqtt_client_id,
            .username = o->mqtt_username,
            .authentication = { .password = o->mqtt_password },
        },
        .session = {
            .keepalive = 240,                 /* docs/mqtt-udp.md §6.1 */
        },
        .network = {
            .disable_auto_reconnect = true,   /* app owns reconnection */
        },
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) return -1;
    esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqtt_event, NULL);
    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) return -1;

    EventBits_t bits = xEventGroupWaitBits(s_ev, EV_HELLO_OK | EV_CLOSED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_XZ_SESSION_TIMEOUT_MS));
    if (bits & EV_HELLO_OK) return 0;
    ESP_LOGE(TAG, "session setup timeout");
    mqttsess_stop();
    return -1;
}

void mqttsess_stop(void)
{
    s_open = false;
    s_listening = false;
    udp_close();
    if (s_aes_ready) { psa_destroy_key(s_aes_key); s_aes_ready = false; }
    if (s_mqtt) {
        if (s_session_id[0] && s_pub_topic[0]) {
            send_json("{\"session_id\":\"%s\",\"type\":\"goodbye\"}", s_session_id);
            vTaskDelay(pdMS_TO_TICKS(100));   /* let the goodbye flush */
        }
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    s_session_id[0] = 0;
}

void mqttsess_start_listening(void)
{
    if (!s_open || s_listening) return;
    /* empty detect text: the server transcribes the uplink audio itself
       [interpretation 4] */
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"\"}",
              session_id_or_empty());
    s_listening = true;
}

void mqttsess_stop_listening(void)
{
    /* no message: mic is muted for playback; the next round re-arms with a
       fresh "detect" */
    s_listening = false;
}

void mqttsess_send_abort(void)
{
    if (!s_open) return;
    send_json("{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"wake_word_detected\"}",
              session_id_or_empty());
}

void mqttsess_send_audio(const uint8_t *opus, size_t len)
{
    if (!s_open || s_sock < 0 || len == 0 || len > 1400) return;

    /* header == AES-CTR IV [interpretation 3] */
    uint8_t hdr[16];
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);   /* ms uptime */
    uint32_t seq = ++s_tx_seq;
    memcpy(hdr, s_nonce, 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len & 0xff);
    hdr[8]  = (uint8_t)(ts >> 24);  hdr[9]  = (uint8_t)(ts >> 16);
    hdr[10] = (uint8_t)(ts >> 8);   hdr[11] = (uint8_t)(ts);
    hdr[12] = (uint8_t)(seq >> 24); hdr[13] = (uint8_t)(seq >> 16);
    hdr[14] = (uint8_t)(seq >> 8);  hdr[15] = (uint8_t)(seq);

    static uint8_t pkt[1416];
    memcpy(pkt, hdr, 16);

    /* multipart CTR encrypt — one-shot psa_cipher_encrypt generates its own
       IV, but the wire protocol requires our header to BE the IV */
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t olen, olen2;
    if (psa_cipher_encrypt_setup(&op, s_aes_key, PSA_ALG_CTR) != PSA_SUCCESS ||
        psa_cipher_set_iv(&op, hdr, 16) != PSA_SUCCESS ||
        psa_cipher_update(&op, opus, len, pkt + 16, sizeof(pkt) - 16, &olen) != PSA_SUCCESS ||
        psa_cipher_finish(&op, pkt + 16 + olen, sizeof(pkt) - 16 - olen, &olen2) != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        return;
    }
    lwip_send(s_sock, pkt, 16 + olen + olen2, 0);
}

void mqttsess_send_mcp(const char *payload_json)
{
    send_json("{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
              session_id_or_empty(), payload_json);
}

/* one-time init */
void mqttsess_init(void)
{
    if (!s_ev) s_ev = xEventGroupCreate();
    if (psa_crypto_init() != PSA_SUCCESS)
        ESP_LOGE(TAG, "psa crypto init failed");
}