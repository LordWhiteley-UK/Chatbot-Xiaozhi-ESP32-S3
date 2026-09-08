/**
 * session_mqtt.c — MQTT+UDP transport backend (xiaozhi protocol v3).
 *
 * MQTT (TLS, port 8883) carries JSON control; a connected UDP socket
 * carries AES-128-CTR encrypted Opus audio.  The 16-byte packet header
 * doubles as the AES-CTR IV (nonce template with payload_len, timestamp
 * and sequence overwritten per packet).
 *
 * Architectural notes — derived from analysis of the reference firmware
 * (78/xiaozhi-esp32 and espressif/esp-iot-solution/examples/ai/xiaozhi_chat):
 *  - UDP socket uses connect() + send()/recv() (connected mode).
 *  - recv timeout 200 ms, send timeout 20 ms (espressif reference values).
 *  - UDP rx task at low priority with a startup log line so we can confirm
 *    it is alive (a race condition where s_open was set after xTaskCreate
 *    caused the task to exit immediately in a previous build).
 *  - Listen mode is "auto": the server detects end-of-speech with its own
 *    VAD; the device never sends "listen stop" (matches the 78 firmware
 *    default for non-AEC devices).
 *  - MCP is disabled in the hello ("features":{"mcp":false}) to avoid
 *    server-side tools/list polling that was generating ~3 MQTT messages
 *    per second and a recurring "duplicate tool names" alert.
 *  - TLS uses the ESP x509 certificate bundle (esp_crt_bundle_attach),
 *    identical to both reference implementations.
 */
#include "session_priv.h"
#include "app.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "psa/crypto.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "mqtt_udp";

#define EV_HELLO_OK  BIT0
#define EV_CLOSED    BIT1

/* ── session state ──────────────────────────────────────────────── */
static esp_mqtt_client_handle_t s_mqtt;
static EventGroupHandle_t s_ev;

static char s_session_id[64];
static volatile bool s_open;
static volatile bool s_listening;

static char s_pub_topic[64];
static char s_sub_topic[80];

/* UDP socket + AES */
static int s_sock = -1;
static struct sockaddr_in s_udp_dst;
static psa_key_id_t s_aes_key;
static bool s_aes_ready;
static uint8_t s_nonce[16];         /* server-supplied IV template    */
static uint32_t s_tx_seq;           /* uplink sequence counter        */
static uint32_t s_rx_seq;           /* downlink (anti-replay)         */
static volatile int s_downlink_rate;

/* diagnostics */
static int s_rx_cnt, s_tx_cnt, s_rx_bad;

/* MQTT fragment reassembly */
static char s_json_buf[4096];
static size_t s_json_len;

/* ── helpers ────────────────────────────────────────────────────── */
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

static const char *sid(void) { return s_session_id[0] ? s_session_id : ""; }

/* ── UDP close ──────────────────────────────────────────────────── */
static void udp_close(void)
{
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
}

/* ── UDP receive task ───────────────────────────────────────────── */
static void udp_rx_task(void *arg)
{
    static uint8_t pkt[1600];
    int timeout_cnt = 0;
    ESP_LOGI(TAG, "udp_rx_task started (sock=%d, open=%d)", s_sock, (int)s_open);

    while (s_open && s_sock >= 0) {
        int n = lwip_recv(s_sock, pkt, sizeof(pkt), 0);
        if (n < 0) {
            /* EAGAIN / ETIMEDOUT — normal timeout, keep waiting */
            if (++timeout_cnt <= 3 || timeout_cnt % 50 == 0)
                ESP_LOGI(TAG, "udp recv timeout #%d (errno %d, tx=%d)",
                         timeout_cnt, (int)errno, s_tx_cnt);
            continue;
        }
        if (n == 0) continue;
        timeout_cnt = 0;
        s_rx_cnt++;
        if (s_rx_cnt <= 10)
            ESP_LOGI(TAG, "udp rx #%d: %d bytes", s_rx_cnt, n);

        /* validate header */
        if (n < 17 || pkt[0] != 0x01) {
            if (++s_rx_bad <= 5 || s_rx_bad % 100 == 0)
                ESP_LOGW(TAG, "udp rx drop: %d bytes, type=0x%02x", n, pkt[0]);
            continue;
        }
        int plen = (pkt[2] << 8) | pkt[3];
        if (16 + plen > n) {
            if (++s_rx_bad <= 5 || s_rx_bad % 100 == 0)
                ESP_LOGW(TAG, "udp rx drop: payload_len %d > %d", plen, n - 16);
            continue;
        }
        uint32_t seq = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                       ((uint32_t)pkt[14] << 8) | pkt[15];
        if (s_rx_seq && (int32_t)(seq - s_rx_seq) <= 0) continue;  /* replay */
        s_rx_seq = seq;

        /* AES-128-CTR decrypt — the 16-byte header IS the IV */
        uint8_t plain[1600];
        size_t olen, olen2;
        psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
        psa_status_t st =
            psa_cipher_decrypt_setup(&op, s_aes_key, PSA_ALG_CTR);
        if (st == PSA_SUCCESS) st = psa_cipher_set_iv(&op, pkt, 16);
        if (st == PSA_SUCCESS) st = psa_cipher_update(&op, pkt + 16, plen,
                                                      plain, sizeof(plain), &olen);
        if (st == PSA_SUCCESS) st = psa_cipher_finish(&op, plain + olen,
                                                      sizeof(plain) - olen, &olen2);
        if (st != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            ESP_LOGE(TAG, "decrypt failed (seq %lu)", (unsigned long)seq);
            continue;
        }
        /* forward to decode pipeline — the app gates by state */
        audio_play(plain, olen + olen2, s_downlink_rate);
    }
    ESP_LOGI(TAG, "udp_rx_task exiting (open=%d, sock=%d)", (int)s_open, s_sock);
    vTaskDelete(NULL);
}

/* ── hello response: set up UDP + AES ───────────────────────────── */
static void on_hello(const cJSON *json)
{
    const cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(json, "session_id");
    if (sid_j && cJSON_IsString(sid_j))
        strlcpy(s_session_id, cJSON_GetStringValue(sid_j), sizeof(s_session_id));

    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(json, "audio_params");
    int rate = 24000;
    if (ap) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(ap, "sample_rate");
        if (cJSON_IsNumber(r)) rate = r->valueint;
    }

    const cJSON *udp = cJSON_GetObjectItemCaseSensitive(json, "udp");
    if (!cJSON_IsObject(udp)) { ESP_LOGE(TAG, "hello: no udp block"); return; }
    const cJSON *srv  = cJSON_GetObjectItemCaseSensitive(udp, "server");
    const cJSON *prt  = cJSON_GetObjectItemCaseSensitive(udp, "port");
    const cJSON *key  = cJSON_GetObjectItemCaseSensitive(udp, "key");
    const cJSON *nce  = cJSON_GetObjectItemCaseSensitive(udp, "nonce");
    if (!cJSON_IsString(srv) || !cJSON_IsNumber(prt) ||
        !cJSON_IsString(key) || !cJSON_IsString(nce)) {
        ESP_LOGE(TAG, "hello: incomplete udp block");
        return;
    }
    const char *server = cJSON_GetStringValue(srv);
    int port = prt->valueint;

    /* hex-decode 128-bit key + nonce */
    uint8_t aes_key[16];
    const char *kh = cJSON_GetStringValue(key);
    const char *nh = cJSON_GetStringValue(nce);
    if (strlen(kh) != 32 || strlen(nh) != 32) {
        ESP_LOGE(TAG, "hello: bad key/nonce length");
        return;
    }
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(kh + 2*i, "%2x", &v) != 1) return;
        aes_key[i] = v;
        if (sscanf(nh + 2*i, "%2x", &v) != 1) return;
        s_nonce[i] = v;
    }

    /* import AES key into PSA */
    if (s_aes_ready) psa_destroy_key(s_aes_key);
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 128);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_CTR);
    if (psa_import_key(&attrs, aes_key, 16, &s_aes_key) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "aes key import failed");
        return;
    }
    s_aes_ready = true;
    s_tx_seq = 0;
    s_rx_seq = 0;
    memset(aes_key, 0, sizeof(aes_key));

    opus_decoder_setup(rate);
    s_downlink_rate = rate;
    /* Reconfigure I2S TX to the downlink sample rate BEFORE any audio arrives.
       This avoids disabling/enabling the I2S channel during playback, which
       races with the play_task and was causing a crash after TTS_STOP. */
    audio_set_playback_rate(rate);

    /* create + connect UDP socket */
    udp_close();
    s_sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) { ESP_LOGE(TAG, "udp socket failed"); return; }

    memset(&s_udp_dst, 0, sizeof(s_udp_dst));
    s_udp_dst.sin_family = AF_INET;
    s_udp_dst.sin_port = lwip_htons((uint16_t)port);
    if (inet_aton(server, &s_udp_dst.sin_addr) == 0) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res;
        if (lwip_getaddrinfo(server, NULL, &hints, &res) != 0 || !res) {
            ESP_LOGE(TAG, "cannot resolve %s", server);
            udp_close();
            return;
        }
        s_udp_dst.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        lwip_freeaddrinfo(res);
    }

    if (lwip_connect(s_sock, (struct sockaddr *)&s_udp_dst, sizeof(s_udp_dst)) < 0) {
        ESP_LOGE(TAG, "udp connect failed (errno %d)", errno);
        udp_close();
        return;
    }

    /* log local port (NAT mapping verification) */
    struct sockaddr_in local_sa = { 0 };
    socklen_t local_len = sizeof(local_sa);
    if (getsockname(s_sock, (struct sockaddr *)&local_sa, &local_len) == 0)
        ESP_LOGI(TAG, "udp connected, local port %d",
                 (int)lwip_ntohs(local_sa.sin_port));

    /* 200 ms recv timeout, 20 ms send timeout (espressif reference values) */
    struct timeval rtv = { .tv_usec = 200000 };
    lwip_setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    struct timeval stv = { .tv_usec = 20000 };
    lwip_setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

    /* NAT hole-punch: send a 16-byte header with 0-byte payload */
    {
        uint8_t probe[16];
        memcpy(probe, s_nonce, 16);
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
        probe[2] = 0; probe[3] = 0;
        probe[8]  = (uint8_t)(ts >> 24); probe[9]  = (uint8_t)(ts >> 16);
        probe[10] = (uint8_t)(ts >> 8);  probe[11] = (uint8_t)(ts);
        probe[12] = probe[13] = probe[14] = probe[15] = 0;
        int pr = lwip_send(s_sock, probe, 16, 0);
        ESP_LOGI(TAG, "udp probe sent (%d bytes)", pr);
    }

    s_rx_cnt = s_tx_cnt = s_rx_bad = 0;

    /* CRITICAL: set s_open BEFORE creating the task.  The task has higher
       priority than the MQTT event handler and preempts xTaskCreate
       immediately — if s_open is still false it exits before recv(). */
    s_open = true;

    if (xTaskCreate(udp_rx_task, "udp_rx", 6144, NULL, 2, NULL) != pdTRUE)
        ESP_LOGE(TAG, "udp_rx_task create failed");

    ESP_LOGI(TAG, "session %s ready, udp %s:%d, downlink %d Hz",
             s_session_id, server, port, rate);
    xEventGroupSetBits(s_ev, EV_HELLO_OK);
}

/* ── MQTT events ────────────────────────────────────────────────── */
static void mqtt_event(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected");
        esp_mqtt_client_subscribe(s_mqtt, s_sub_topic, 0);
        send_json("{\"type\":\"hello\",\"version\":3,\"transport\":\"udp\","
                  "\"features\":{\"mcp\":false},"
                  "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                  "\"channels\":1,\"frame_duration\":%d}}",
                  CONFIG_OPUS_FRAME_DURATION_MS);
        ESP_LOGI(TAG, "hello sent (v3, udp, mcp off)");
        break;
    case MQTT_EVENT_DATA: {
        if (!e->data_len) break;
        if (e->current_data_offset > 0) {
            /* continuation fragment */
            if (s_json_len + (size_t)e->data_len < sizeof(s_json_buf)) {
                memcpy(s_json_buf + s_json_len, e->data, e->data_len);
                s_json_len += e->data_len;
            }
            if (s_json_len >= e->total_data_len) {
                session_dispatch_json(s_json_buf, s_json_len);
                s_json_len = 0;
            }
        } else if ((size_t)e->data_len < (size_t)e->total_data_len) {
            /* first fragment */
            s_json_len = 0;
            if ((size_t)e->data_len < sizeof(s_json_buf)) {
                memcpy(s_json_buf, e->data, e->data_len);
                s_json_len = e->data_len;
            }
        } else {
            /* complete message */
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
            app_post_event(APP_EVENT_WS_CLOSED, NULL);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error");
        if (!s_open) xEventGroupSetBits(s_ev, EV_CLOSED);
        break;
    default:
        break;
    }
}

/* ── public API ─────────────────────────────────────────────────── */
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

    /* broker URI — default port 8883 for TLS */
    static char uri[192];
    const char *ep = o->mqtt_endpoint;
    uri[0] = 0;
    if (!strstr(ep, "://")) strlcpy(uri, "mqtts://", sizeof(uri));
    strlcat(uri, ep, sizeof(uri));
    if (!strstr(ep, "://") && !strchr(ep, ':')) strlcat(uri, ":8883", sizeof(uri));

    /* topics */
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

    if (s_mqtt) { esp_mqtt_client_destroy(s_mqtt); s_mqtt = NULL; }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials = {
            .client_id = o->mqtt_client_id,
            .username = o->mqtt_username,
            .authentication = { .password = o->mqtt_password },
        },
        .session = { .keepalive = 240 },
        .network = { .disable_auto_reconnect = true },
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
            send_json("{\"session_id\":\"%s\",\"type\":\"goodbye\"}", sid());
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    s_session_id[0] = 0;
}

void mqttsess_start_listening(void)
{
    if (!s_open || s_listening) return;
    /* AUTO mode: the server detects end-of-speech with its own VAD.
       The device never sends "listen stop" — the server handles it. */
    ESP_LOGI(TAG, ">> listen start (auto)");
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\","
              "\"mode\":\"auto\"}", sid());
    s_listening = true;
}

void mqttsess_stop_listening(void)
{
    /* In AUTO mode the server stops listening on its own, but we keep
       this for completeness / barge-in scenarios. */
    if (!s_listening) return;
    ESP_LOGI(TAG, ">> listen stop");
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}", sid());
    s_listening = false;
}

void mqttsess_send_abort(void)
{
    if (!s_open) return;
    ESP_LOGI(TAG, ">> abort");
    send_json("{\"session_id\":\"%s\",\"type\":\"abort\","
              "\"reason\":\"wake_word_detected\"}", sid());
}

void mqttsess_send_audio(const uint8_t *opus, size_t len)
{
    if (!s_open || s_sock < 0 || len == 0 || len > 1400) return;

    /* header = copy of nonce template with per-packet fields overwritten */
    uint8_t hdr[16];
    uint32_t ts  = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t seq = ++s_tx_seq;
    memcpy(hdr, s_nonce, 16);
    hdr[2]  = (uint8_t)(len >> 8);
    hdr[3]  = (uint8_t)(len & 0xff);
    hdr[8]  = (uint8_t)(ts >> 24);  hdr[9]  = (uint8_t)(ts >> 16);
    hdr[10] = (uint8_t)(ts >> 8);   hdr[11] = (uint8_t)(ts);
    hdr[12] = (uint8_t)(seq >> 24); hdr[13] = (uint8_t)(seq >> 16);
    hdr[14] = (uint8_t)(seq >> 8);  hdr[15] = (uint8_t)(seq);

    static uint8_t pkt[1416];
    memcpy(pkt, hdr, 16);

    /* AES-128-CTR encrypt — multipart (header is the IV, not prepended by API) */
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t olen, olen2;
    psa_status_t st = psa_cipher_encrypt_setup(&op, s_aes_key, PSA_ALG_CTR);
    if (st == PSA_SUCCESS) st = psa_cipher_set_iv(&op, hdr, 16);
    if (st == PSA_SUCCESS) st = psa_cipher_update(&op, opus, len,
                                                   pkt + 16, sizeof(pkt) - 16, &olen);
    if (st == PSA_SUCCESS) st = psa_cipher_finish(&op, pkt + 16 + olen,
                                                   sizeof(pkt) - 16 - olen, &olen2);
    if (st != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        return;
    }
    s_tx_cnt++;
    if (s_tx_cnt <= 5)
        ESP_LOGI(TAG, "udp tx #%d: %d bytes", s_tx_cnt, (int)(16 + olen + olen2));
    int sent = lwip_send(s_sock, pkt, 16 + olen + olen2, 0);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        ESP_LOGW(TAG, "udp send failed (errno %d)", (int)errno);
}

void mqttsess_send_mcp(const char *payload_json)
{
    send_json("{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
              sid(), payload_json);
}

void mqttsess_init(void)
{
    if (!s_ev) s_ev = xEventGroupCreate();
    if (psa_crypto_init() != PSA_SUCCESS)
        ESP_LOGE(TAG, "psa crypto init failed");
}