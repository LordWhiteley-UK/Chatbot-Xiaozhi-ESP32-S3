/**
 * mcp_server.c — device-side MCP tools (docs/mcp-protocol.md).
 *
 * Responds to JSON-RPC 2.0 requests carried inside {"type":"mcp"} payloads:
 *   initialize  -> protocolVersion + capabilities + serverInfo
 *   tools/list  -> the registered tools
 *   tools/call  -> invokes a registered tool by name
 * Responses are wrapped by the session layer via session_send_mcp().
 */
#include "app.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mcp";
static const char *MCP_PROTOCOL_VERSION = "2024-11-05";

typedef void (*tool_fn_t)(cJSON *args, char *out, size_t out_len);

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;   /* JSON schema as a string */
    tool_fn_t fn;
} mcp_tool_t;

/* ---- tools ---- */

static void tool_get_device_status(cJSON *args, char *out, size_t out_len)
{
    (void)args;
    snprintf(out, out_len, "{\"content\":[{\"type\":\"text\",\"text\":"
             "\"{\\\"mic\\\":\\\"ok\\\",\\\"speaker\\\":\\\"ok\\\","
             "\\\"display\\\":\\\"ok\\\",\\\"network\\\":\\\"ok\\\"}\"}],"
             "\"isError\":false}");
}

static void tool_reboot(cJSON *args, char *out, size_t out_len)
{
    (void)args;
    snprintf(out, out_len, "{\"content\":[{\"type\":\"text\",\"text\":\"true\"}],"
             "\"isError\":false}");
    vTaskDelay(pdMS_TO_TICKS(500));    /* let the response flush */
    esp_restart();
}

static const mcp_tool_t s_tools[] = {
    { "self.get_device_status",
      "Get the device's hardware status (mic, speaker, display, network)",
      "{\"type\":\"object\"}",
      tool_get_device_status },
    { "self.reboot",
      "Reboot the device",
      "{\"type\":\"object\"}",
      tool_reboot },
};
#define TOOL_COUNT (sizeof(s_tools) / sizeof(s_tools[0]))

/* ---- helpers ---- */

static void send_result(cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    cJSON_AddItemToObject(resp, "result", result);
    char *s = cJSON_PrintUnformatted(resp);
    if (s) {
        session_send_mcp(s);
        cJSON_free(s);
    }
    cJSON_Delete(resp);
}

static void send_error(cJSON *id, int code, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg);
    cJSON_AddItemToObject(resp, "error", err);
    char *s = cJSON_PrintUnformatted(resp);
    if (s) {
        session_send_mcp(s);
        cJSON_free(s);
    }
    cJSON_Delete(resp);
}

static void handle_initialize(cJSON *id, cJSON *params)
{
    (void)params;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", "XIAO ESP32-S3 Voice");
    cJSON_AddStringToObject(info, "version", app_firmware_version());
    send_result(id, result);
}

static void handle_tools_list(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", s_tools[i].name);
        cJSON_AddStringToObject(t, "description", s_tools[i].description);
        cJSON *schema = cJSON_Parse(s_tools[i].input_schema);
        if (schema) {
            cJSON_AddItemToObject(t, "inputSchema", schema);
        } else {
            cJSON_AddItemToObject(t, "inputSchema", cJSON_CreateObject());
        }
        cJSON_AddItemToArray(tools, t);
    }
    cJSON_AddStringToObject(result, "nextCursor", "");
    send_result(id, result);
}

static void handle_tools_call(cJSON *id, cJSON *params)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!cJSON_IsString(name)) {
        send_error(id, -32602, "Invalid params: missing tool name");
        return;
    }
    const char *name_s = cJSON_GetStringValue(name);
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        if (!strcmp(s_tools[i].name, name_s)) {
            static char result_json[512];
            s_tools[i].fn((cJSON *)(cJSON_IsObject(args) ? args : NULL),
                          result_json, sizeof(result_json));
            cJSON *result = cJSON_Parse(result_json);
            if (result) {
                send_result(id, result);
            } else {
                send_error(id, -32603, "Internal error: tool result parse failure");
            }
            return;
        }
    }
    static char msg[96];
    snprintf(msg, sizeof(msg), "Unknown tool: %.64s", name_s);
    send_error(id, -32601, msg);
}

void mcp_handle_payload(const char *payload_json, size_t len)
{
    (void)len;
    cJSON *payload = cJSON_Parse(payload_json);
    if (!payload) { ESP_LOGE(TAG, "bad json-rpc payload"); return; }

    const cJSON *method = cJSON_GetObjectItemCaseSensitive(payload, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");

    if (!cJSON_IsString(method)) {
        /* response to something we sent — we only receive requests */
        ESP_LOGD(TAG, "ignoring non-request payload");
        cJSON_Delete(payload);
        return;
    }
    const char *m = cJSON_GetStringValue(method);
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");

    ESP_LOGI(TAG, "mcp: %s", m);
    if (!strcmp(m, "initialize")) {
        handle_initialize((cJSON *)id, (cJSON *)params);
    } else if (!strcmp(m, "tools/list")) {
        handle_tools_list((cJSON *)id);
    } else if (!strcmp(m, "tools/call")) {
        handle_tools_call((cJSON *)id, (cJSON *)params);
    } else if (!strncmp(m, "notifications/", 14)) {
        ESP_LOGD(TAG, "notification: %s", m);
    } else {
        if (id) send_error((cJSON *)id, -32601, "Method not found");
    }
    cJSON_Delete(payload);
}