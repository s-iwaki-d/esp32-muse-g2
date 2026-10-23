/* Even G2 <-> Cloudflare <-> paired Muse Gadget. Credentials and the Noise
 * connection remain owned by the upstream SDK. This file never forwards them. */
#include "muse_even_bridge.h"
#include "muse_bridge_ui.h"
#include "muse_chat.h"
#include "muse_wifi.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INPUT_LIMIT 8192u
#define WIRE_LIMIT 65536u
#define ID_SIZE 129
#define WS_TASK_NAME "even_bridge_ws"
_Static_assert(sizeof(WS_TASK_NAME) <= configMAX_TASK_NAME_LEN, "WebSocket task name must not be truncated");
static const char *TAG = "even_bridge";
static QueueHandle_t s_incoming;
static SemaphoreHandle_t s_lock;
static esp_websocket_client_handle_t s_ws;
static bridge_reply_t s_reply;
static bool s_online, s_fault, s_ws_closed;
static uint32_t s_epoch, s_next_generation;
static char s_id[ID_SIZE];
static int64_t s_deadline;

typedef struct { uint32_t epoch; char *json; } incoming_t;
/* Only the websocket event task touches these framing fields. */
static char *s_rx;
static size_t s_rx_len;
static bool s_rx_started;

static bool online(void) { return __atomic_load_n(&s_online, __ATOMIC_ACQUIRE); }
static uint32_t epoch(void) { return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE); }
static void rx_clear(void) { free(s_rx); s_rx = NULL; s_rx_len = 0; s_rx_started = false; }
static void fault(void) { __atomic_store_n(&s_fault, true, __ATOMIC_RELEASE); }

static void ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    (void)arg; (void)base;
    if (event_id == WEBSOCKET_EVENT_CONNECTED || event_id == WEBSOCKET_EVENT_DISCONNECTED ||
        event_id == WEBSOCKET_EVENT_CLOSED) {
        __atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&s_online, event_id == WEBSOCKET_EVENT_CONNECTED, __ATOMIC_RELEASE);
        rx_clear();
        if (event_id == WEBSOCKET_EVENT_CONNECTED)
            ESP_LOGI(TAG, "Cloudflare websocket connected");
        if (event_id == WEBSOCKET_EVENT_CLOSED)
            __atomic_store_n(&s_ws_closed, true, __ATOMIC_RELEASE);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA) return;
    esp_websocket_event_data_t *e = data;
    if (e->op_code != 1 && e->op_code != 0) return; /* protocol ping/close handled by client */
    if (e->payload_offset == 0 && e->op_code == 1) {
        if (s_rx_started) { fault(); rx_clear(); return; }
        s_rx_started = true;
    }
    if (!s_rx_started || e->data_len < 0 || e->payload_len < 0 || e->payload_offset < 0 ||
        (size_t)e->data_len > WIRE_LIMIT - s_rx_len) { fault(); rx_clear(); return; }
    char *next = realloc(s_rx, s_rx_len + (size_t)e->data_len + 1);
    if (!next) { fault(); rx_clear(); return; }
    s_rx = next;
    memcpy(s_rx + s_rx_len, e->data_ptr, (size_t)e->data_len);
    s_rx_len += (size_t)e->data_len;
    s_rx[s_rx_len] = '\0';
    if (e->fin && e->payload_offset + e->data_len == e->payload_len) {
        if (memchr(s_rx, '\0', s_rx_len)) { fault(); rx_clear(); return; }
        incoming_t item = {epoch(), s_rx};
        if (xQueueSend(s_incoming, &item, 0) != pdTRUE) { fault(); free(s_rx); }
        s_rx = NULL; s_rx_len = 0; s_rx_started = false;
    }
}

/* Called on the Muse session task. No network, logging, or LVGL work here. */
void muse_bridge_emit(uint32_t generation, bridge_event_t event, unsigned message,
                      const char *text, bool complete) {
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) { fault(); return; }
    bridge_reply_event(&s_reply, generation, event, message, text, complete);
    xSemaphoreGive(s_lock);
}

static bool send_json(cJSON *body, uint32_t expected_epoch) {
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return false;
    size_t n = strlen(json);
    bool ok = online() && epoch() == expected_epoch &&
        esp_websocket_client_send_text(s_ws, json, (int)n, pdMS_TO_TICKS(3000)) == (int)n;
    cJSON_free(json);
    return ok;
}
static bool send_event(const char *type, const char *id, const char *text, const char *code,
                       uint32_t expected_epoch) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return false;
    cJSON_AddStringToObject(body, "type", type);
    cJSON_AddStringToObject(body, "id", id);
    if (text) cJSON_AddStringToObject(body, code ? "message" : "text", text);
    if (code) cJSON_AddStringToObject(body, "code", code);
    return send_json(body, expected_epoch);
}
static bool identifier(const char *s, size_t max) {
    if (!s || !*s || strlen(s) > max) return false;
    for (; *s; s++) if (!( (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                           (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' )) return false;
    return true;
}
static bool uuid(const char *s) {
    if (!s || strlen(s) != 36) return false;
    for (unsigned i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
                   (s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return true;
}
static void cancel_active(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t generation = s_reply.active ? s_reply.generation : 0;
    bridge_reply_clear(&s_reply);
    s_id[0] = 0;
    xSemaphoreGive(s_lock);
    if (generation) muse_bridge_session_cancel(generation);
}
static void receive(char *json, uint32_t connection_epoch) {
    if (!strcmp(json, "pong")) return;
    const char *end = NULL;
    cJSON *body = cJSON_ParseWithOpts(json, &end, true);
    if (!body) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "type"));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "id"));
    if (!type || !identifier(id, ID_SIZE - 1)) { cJSON_Delete(body); return; }
    if (!strcmp(type, "cancel")) {
        if (!strcmp(id, s_id)) cancel_active();
        cJSON_Delete(body); return;
    }
    if (strcmp(type, "chat")) { cJSON_Delete(body); return; }
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "text"));
    const char *session = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "session_id"));
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(body, "expires_at");
    time_t now = time(NULL);
    const char *error = NULL;
    if (!text || !*text || strlen(text) > INPUT_LIMIT || !uuid(session)) error = "Invalid chat request";
    else if (!cJSON_IsNumber(expires) || !isfinite(expires->valuedouble) ||
             now < 1700000000 || expires->valuedouble <= (double)now * 1000 ||
             expires->valuedouble > (double)now * 1000 + 15 * 60 * 1000) error = "Request expired or clock is not synchronized";
    else if (s_id[0]) error = "Muse is busy with another request";
    if (error) {
        send_event("error", id, error, "device_rejected", connection_epoch);
        cJSON_Delete(body); return;
    }
    uint32_t generation = ++s_next_generation;
    if (!generation) generation = ++s_next_generation;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bridge_reply_begin(&s_reply, generation);
    snprintf(s_id, sizeof(s_id), "%s", id);
    s_deadline = esp_timer_get_time() + (int64_t)((expires->valuedouble - (double)now * 1000) * 1000);
    xSemaphoreGive(s_lock);
    if (!muse_bridge_session_submit(generation, text, session)) {
        send_event("error", id, "Muse is not paired or its command queue is full", "muse_unavailable", connection_epoch);
        cancel_active();
    } else if (!send_event("started", id, NULL, NULL, connection_epoch)) cancel_active();
    cJSON_Delete(body);
}
static void finish_if_ready(uint32_t connection_epoch) {
    char id[ID_SIZE], error[96]; char *reply = NULL;
    bool terminal, success; uint32_t generation;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_reply.active && esp_timer_get_time() >= s_deadline)
        bridge_reply_event(&s_reply, s_reply.generation, BRIDGE_ERROR, 0, "Request timed out", false);
    terminal = s_reply.active && s_reply.terminal;
    success = s_reply.success;
    generation = s_reply.generation;
    snprintf(id, sizeof(id), "%s", s_id);
    snprintf(error, sizeof(error), "%s", s_reply.error);
    if (terminal && success) reply = bridge_reply_join(&s_reply);
    xSemaphoreGive(s_lock);
    if (!terminal) return;
    if (success && reply) {
        /* UTF-8 boundaries matter: every delta is valid JSON text on its own. */
        const char *p = reply;
        bool ok = true;
        while (*p && ok) {
            size_t n = bridge_utf8_chunk(p, 512);
            if (!n) { ok = false; break; }
            char chunk[513]; memcpy(chunk, p, n); chunk[n] = 0;
            ok = send_event("delta", id, chunk, NULL, connection_epoch);
            p += n;
        }
        if (ok) send_event("done", id, NULL, NULL, connection_epoch);
        else fault();
    } else {
        send_event("error", id, success ? "Not enough memory for reply" : error,
                   "muse_error", connection_epoch);
        muse_bridge_session_cancel(generation);
    }
    free(reply);
    cancel_active();
}
static void bridge_task(void *unused) {
    (void)unused;
    uint32_t seen_epoch = epoch();
    int64_t last_ping = 0;
    int64_t next_lifecycle_attempt = 0;
    bool ws_started = false;
    bool stop_pending = false;
    bool awaiting_exit = false;
    for (;;) {
        bool wifi_ready = muse_wifi_connected();
        uint32_t current_epoch = epoch();
        if (seen_epoch != current_epoch) { cancel_active(); seen_epoch = current_epoch; }
        if (__atomic_exchange_n(&s_fault, false, __ATOMIC_ACQ_REL)) {
            if (wifi_ready && online() && s_id[0])
                send_event("error", s_id, "Bridge transport failed", "transport_error", seen_epoch);
            cancel_active();
            stop_pending = ws_started;
        }
        if (!wifi_ready && ws_started) stop_pending = true;
        if (ws_started && __atomic_load_n(&s_ws_closed, __ATOMIC_ACQUIRE)) {
            stop_pending = true;
            awaiting_exit = true;
        }
        if (stop_pending) {
            cancel_active();
            if (!awaiting_exit && esp_timer_get_time() >= next_lifecycle_attempt) {
                esp_err_t err = esp_websocket_client_stop(s_ws);
                if (err == ESP_OK) {
                    awaiting_exit = true;
                } else {
                    ESP_LOGW(TAG, "Bridge websocket stop failed: %s", esp_err_to_name(err));
                    next_lifecycle_attempt = esp_timer_get_time() + 5 * 1000000LL;
                }
            }
            /* In client 1.2.3, CLOSED precedes transport cleanup and stop()
             * returns before the task deletes itself. The IDF lookup includes
             * tasks awaiting deletion, so NULL confirms this uniquely named
             * task can no longer access the client or framing state. */
            if (xTaskGetHandle(WS_TASK_NAME) == NULL) {
                ws_started = stop_pending = awaiting_exit = false;
                __atomic_store_n(&s_online, false, __ATOMIC_RELEASE);
                __atomic_store_n(&s_ws_closed, false, __ATOMIC_RELEASE);
                seen_epoch = __atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
                rx_clear();
                incoming_t stale;
                while (xQueueReceive(s_incoming, &stale, 0) == pdTRUE) free(stale.json);
                __atomic_store_n(&s_fault, false, __ATOMIC_RELEASE);
                next_lifecycle_attempt = 0;
            }
        }
        if (!ws_started && muse_wifi_connected() &&
            esp_timer_get_time() >= next_lifecycle_attempt) {
            esp_err_t err = esp_websocket_client_start(s_ws);
            if (err == ESP_OK) {
                ws_started = true;
                last_ping = esp_timer_get_time();
            } else {
                ESP_LOGW(TAG, "Bridge websocket start failed: %s", esp_err_to_name(err));
                next_lifecycle_attempt = esp_timer_get_time() + 5 * 1000000LL;
            }
        }
        incoming_t item;
        if (xQueueReceive(s_incoming, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            current_epoch = epoch();
            if (seen_epoch != current_epoch) { cancel_active(); seen_epoch = current_epoch; }
            if (ws_started && !stop_pending && muse_wifi_connected() && online() && item.epoch == seen_epoch)
                receive(item.json, item.epoch);
            free(item.json);
        }
        current_epoch = epoch();
        if (seen_epoch != current_epoch) { cancel_active(); seen_epoch = current_epoch; }
        bool usable = ws_started && !stop_pending && muse_wifi_connected() && online();
        if (usable) {
            finish_if_ready(seen_epoch);
            if (esp_timer_get_time() - last_ping > 20 * 1000000LL) {
                esp_websocket_client_send_text(s_ws, "ping", 4, pdMS_TO_TICKS(1000));
                last_ping = esp_timer_get_time();
            }
        }
        muse_hatch_status_t status;
        muse_hatch_status(&status);
        muse_bridge_ui_status(usable, status.state == MUSE_HATCH_REACHABLE, s_id[0] != 0);
    }
}
void muse_even_bridge_start(void) {
    if (s_ws) return;
    muse_hatch_status_t status;
    muse_hatch_status(&status);
    if (status.state == MUSE_HATCH_NOT_SET) {
        /* Pairing restarts the upstream application. Until then, leave all
         * bridge task stacks, queues and TLS/WS buffers unallocated. */
        ESP_LOGI(TAG, "Bridge waits for Muse pairing");
        return;
    }
    if (strncmp(CONFIG_MUSE_BRIDGE_URL, "wss://", 6) || !CONFIG_MUSE_BRIDGE_TOKEN[0] ||
        strpbrk(CONFIG_MUSE_BRIDGE_TOKEN, "\r\n")) {
        ESP_LOGW(TAG, "Set the Worker WSS URL and device token in menuconfig"); return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_incoming = xQueueCreate(4, sizeof(incoming_t));
    char *headers = malloc(strlen(CONFIG_MUSE_BRIDGE_TOKEN) + 32);
    if (!s_lock || !s_incoming || !headers) {
        ESP_LOGE(TAG, "Bridge allocation failed");
        free(headers);
        goto init_failed;
    }
    sprintf(headers, "Authorization: Bearer %s\r\n", CONFIG_MUSE_BRIDGE_TOKEN);
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_MUSE_BRIDGE_URL,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
        .buffer_size = 2048,
        .task_name = WS_TASK_NAME,
        .task_stack = 8192,
    };
    s_ws = esp_websocket_client_init(&cfg);
    free(headers);
    if (!s_ws) { ESP_LOGE(TAG, "Bridge websocket initialization failed"); goto init_failed; }
    if (esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Bridge websocket event registration failed");
        goto init_failed;
    }
    /* This task only uses cached status, queues and network/JSON APIs, never
     * NVS. Keep its full stack in PSRAM, as the SDK does for muse_chat, so
     * credential refresh can allocate its required internal 8 KiB stack. */
    if (xTaskCreateWithCaps(bridge_task, "even_bridge", 8192, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Bridge PSRAM task allocation failed");
        goto init_failed;
    }
    return; /* The bridge task waits for Wi-Fi before starting WebSocket. */

init_failed:
    /* No bridge/websocket task has started on these failure paths. */
    if (s_ws) { esp_websocket_client_destroy(s_ws); s_ws = NULL; }
    if (s_incoming) { vQueueDelete(s_incoming); s_incoming = NULL; }
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = NULL; }
}
