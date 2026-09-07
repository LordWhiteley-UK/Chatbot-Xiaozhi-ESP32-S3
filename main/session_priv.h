/**
 * session_priv.h — internal interface between the session layer and its
 * transport backends (docs/websocket.md, docs/mqtt-udp.md).
 *
 * session.c owns the public session_* API, the shared JSON dispatcher and
 * the WebSocket backend; session_mqtt.c implements the MQTT+UDP backend.
 */
#pragma once

#include "app.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by a backend for every server JSON message. Dispatches stt/tts/
 * llm/mcp/system/alert to the app; "hello" is forwarded to the handler
 * registered by the active backend. */
void session_dispatch_json(const char *data, size_t len);

typedef void (*session_hello_fn_t)(const cJSON *hello);
void session_set_hello_handler(session_hello_fn_t fn);

/* MQTT+UDP backend (docs/mqtt-udp.md) — same semantics as the ws_* calls */
void mqttsess_init(void);
int  mqttsess_start(void);
void mqttsess_stop(void);
void mqttsess_start_listening(void);
void mqttsess_stop_listening(void);
void mqttsess_send_abort(void);
void mqttsess_send_audio(const uint8_t *opus, size_t len);
void mqttsess_send_mcp(const char *payload_json);
bool mqttsess_is_open(void);

/* WebSocket backend (docs/websocket.md), implemented in session.c */
int  ws_start(void);
void ws_stop(void);
void ws_start_listening(void);
void ws_stop_listening(void);
void ws_send_abort(void);
void ws_send_audio(const uint8_t *opus, size_t len);
void ws_send_mcp(const char *payload_json);
bool ws_is_open(void);

#ifdef __cplusplus
}
#endif