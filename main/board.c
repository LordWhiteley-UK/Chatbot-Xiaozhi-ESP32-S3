/**
 * board.c — device identity helpers.
 *
 * Device-Id: the physical MAC address (docs/websocket.md §2).
 * Client-Id: a software-generated UUID, persisted in NVS.
 */
#include "app.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "esp_mac.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static char s_device_id[18];
static char s_client_id[37];
static bool s_inited;

const char *app_firmware_version(void) { return "1.0.0"; }

static void init_identity(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    /* canonical AA:BB:CC:DD:EE:FF — the OTA endpoint rejects bare hex */
    snprintf(s_device_id, sizeof(s_device_id),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t h;
    if (nvs_open("identity", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(s_client_id);
        if (nvs_get_str(h, "client_id", s_client_id, &len) != ESP_OK || !s_client_id[0]) {
            /* UUIDv4 from the hardware RNG */
            uint32_t r[4];
            esp_fill_random(r, sizeof(r));
            snprintf(s_client_id, sizeof(s_client_id),
                     "%08x-%04x-4%03x-%04x-%08x%04x",
                     (unsigned)r[0], (unsigned)(r[1] & 0xffff),
                     (unsigned)((r[1] >> 16) & 0xfff),
                     (unsigned)(r[2] & 0xffff) | 0x8000u, (unsigned)r[3],
                     (unsigned)((r[2] >> 16) & 0xffff));
            ESP_ERROR_CHECK(nvs_set_str(h, "client_id", s_client_id));
            ESP_ERROR_CHECK(nvs_commit(h));
        }
        nvs_close(h);
    }
    s_inited = true;
}

const char *app_device_id(void)
{
    if (!s_inited) init_identity();
    return s_device_id;
}

const char *app_client_id(void)
{
    if (!s_inited) init_identity();
    return s_client_id;
}