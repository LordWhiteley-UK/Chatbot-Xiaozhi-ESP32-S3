/**
 * wifi_prov.c — WiFi provisioning.
 *
 * If valid credentials exist in NVS, connects as a station directly.
 * Otherwise starts a SoftAP ("Xiaozhi-XXXXXX") with a simple web form at
 * http://192.168.4.1 to collect SSID/password, stores them in NVS and
 * reboots into station mode.
 */
#include "app.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_count;

static const char *FORM_HTML =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>XIAO voice setup</title></head>"
"<body style='font-family:sans-serif;max-width:22em;margin:3em auto'>"
"<h2>WiFi setup</h2>"
"<h3>Available networks <small><a href='/'>(rescan)</a></small></h3>"
"<div id='nets' style='display:flex;flex-direction:column;gap:.4em;margin-bottom:1em'>"
"Scanning...</div>"
"<form method='POST' action='/save'>"
"<p><label>SSID<br><input id='ssid' name='ssid' maxlength='32' required></label></p>"
"<p><label>Password<br><input id='pass' name='pass' type='password' maxlength='64'></label></p>"
"<p><button type='submit'>Save &amp; connect</button></p>"
"</form>"
"<script>"
"fetch('/scan').then(r=>r.json()).then(list=>{"
"const el=document.getElementById('nets');el.textContent='';"
"if(!list.length){el.textContent='None found - use manual entry below';return;}"
"list.forEach(ap=>{const b=document.createElement('button');b.type='button';"
"b.textContent=ap.ssid+' ('+ap.rssi+(ap.lock?', locked':', open')+')';"
"b.onclick=()=>{document.getElementById('ssid').value=ap.ssid;"
"document.getElementById('pass').focus();};el.appendChild(b);});"
"}).catch(()=>{document.getElementById('nets').textContent='';});"
"</script></body></html>";

void app_nvs_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

static bool app_nvs_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t l1 = ssid_len, l2 = pass_len;
    esp_err_t e = nvs_get_str(h, "ssid", ssid, &l1);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &l2);
    nvs_close(h);
    return e == ESP_OK && e2 == ESP_OK && l1 > 1;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_retry_count++;
            if (s_retry_count < 12) {
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            }
            break;
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP up: join 'Xiaozhi-XXXXXX' and open http://192.168.4.1");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "connected, ip=" IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* ---- provisioning HTTP server handlers ---- */
static esp_err_t form_handler(httpd_req_t *req)
{
    return httpd_resp_send(req, FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Escape a string for embedding inside a JSON string literal. */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (; *in && o < out_len - 1; in++) {
        char c = *in;
        if (c == '"' || c == '\\') {
            if (o >= out_len - 2) break;
            out[o++] = '\\';
        }
        out[o++] = c;
    }
    out[o] = 0;
}

/* GET /scan — scan for APs visible to the STA interface (the radio runs
   in APSTA mode during provisioning) and return them as a JSON array. */
static esp_err_t scan_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    static wifi_ap_record_t records[20];
    uint16_t n = 0;
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_time.active = { .min = 100, .max = 300 },
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        return httpd_resp_send(req, "[]", 2);
    }
    err = esp_wifi_scan_get_ap_records(&n, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan fetch failed: %s", esp_err_to_name(err));
        return httpd_resp_send(req, "[]", 2);
    }
    ESP_LOGI(TAG, "scan found %u APs", (unsigned)n);

    /* up to 20 SSIDs, each quoted/escaped: 32 chars -> up to 66 bytes */
    char *buf = malloc(20 * 90 + 8);
    if (!buf) return httpd_resp_send(req, "[]", 2);
    size_t o = 0;
    buf[o++] = '[';
    for (uint16_t i = 0; i < n; i++) {
        char esc[3 * 32 + 3];
        json_escape((const char *)records[i].ssid, esc, sizeof(esc));
        int w = snprintf(buf + o, 100, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"lock\":%d}",
                         o > 1 ? "," : "", esc, records[i].rssi,
                         records[i].authmode != WIFI_AUTH_OPEN ? 1 : 0);
        if (w < 0 || o + w >= 20 * 90 - 1) break;
        o += w;
    }
    buf[o++] = ']';
    buf[o] = 0;
    esp_err_t r = httpd_resp_send(req, buf, (int)o);
    free(buf);
    return r;
}

static int urldecode(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (; *in && o < out_len - 1; in++) {
        if (*in == '+') out[o++] = ' ';
        else if (*in == '%' && in[1] && in[2]) {
            char hex[3] = { in[1], in[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            in += 2;
        } else out[o++] = *in;
    }
    out[o] = 0;
    return (int)o;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no data");
    body[len] = 0;

    char ssid[33] = {0}, pass[65] = {0};
    /* x-www-form-urlencoded: ssid=...&pass=... */
    char *p = strstr(body, "ssid=");
    if (!p) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    char raw_ssid[33] = {0}, raw_pass[65] = {0};
    p += 5;
    for (int i = 0; *p && *p != '&' && i < (int)sizeof(raw_ssid) - 1; p++, i++)
        raw_ssid[i] = *p;
    char *q = strstr(p, "pass=");
    if (q) {
        q += 5;
        for (int i = 0; *q && *q != '&' && i < (int)sizeof(raw_pass) - 1; q++, i++)
            raw_pass[i] = *q;
    }
    urldecode(raw_ssid, ssid, sizeof(ssid));
    urldecode(raw_pass, pass, sizeof(pass));
    if (!ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty ssid");
    ESP_LOGI(TAG, "saving wifi: ssid=%s", ssid);
    app_nvs_set_wifi(ssid, pass);
    httpd_resp_send(req, "<html><body>Saved. Rebooting...</body></html>",
                    HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static void run_provisioning_ap(void)
{
    esp_netif_create_default_wifi_ap();
    /* the STA netif must exist too: esp_wifi_scan_start() runs on the STA
       interface even in APSTA mode, and without it every /scan fails */
    esp_netif_create_default_wifi_sta();

    wifi_config_t ap_cfg = { 0 };
    /* AP name suffix: the MAC without separators (Xiaozhi-XXXXXXXXXXXX) */
    char suffix[13];
    const char *id = app_device_id();
    for (int i = 0, j = 0; id[i] && j < 12; i++)
        if (id[i] != ':') suffix[j++] = id[i];
    suffix[12] = 0;
    char ap_name[32];
    snprintf(ap_name, sizeof(ap_name), "Xiaozhi-%s", suffix);
    strlcpy((char *)ap_cfg.ap.ssid, ap_name, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_name);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    /* APSTA: keeps the provisioning AP up while the STA interface scans */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "provisioning AP '%s' started", ap_name);
    display_status_line("WiFi setup",
                        "In WiFi, join Xiaozhi.\nThen open\n192.168.4.1 in browser");
}

void wifi_prov_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_events = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    char ssid[33] = {0}, pass[65] = {0};
    bool have = app_nvs_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

    if (have) {
        esp_netif_create_default_wifi_sta();
        wifi_config_t sta_cfg = { 0 };
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        display_status_line("WiFi connecting", ssid);

        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        if (bits & WIFI_CONNECTED_BIT) {
            char detail[48];
            snprintf(detail, sizeof(detail), "%s connected", ssid);
            display_status_line("WiFi OK", detail);
            return;
        }
        ESP_LOGE(TAG, "stored credentials failed; falling back to provisioning");
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
        /* re-init for AP mode */
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &event_handler, NULL, NULL));
    } else {
        ESP_LOGI(TAG, "no stored credentials; starting provisioning");
    }

    /* first boot (or failed creds): SoftAP + form */
    run_provisioning_ap();

    httpd_handle_t server = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));
    httpd_uri_t uri_form = { .uri = "/", .method = HTTP_GET, .handler = form_handler };
    httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler };
    httpd_uri_t uri_scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler };
    httpd_register_uri_handler(server, &uri_form);
    httpd_register_uri_handler(server, &uri_save);
    httpd_register_uri_handler(server, &uri_scan);

    /* park here forever; a successful /save reboots the device */
    while (true) vTaskDelay(pdMS_TO_TICKS(10000));
}