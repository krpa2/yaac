#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "coffee_machine.h"
#include "gemini_live.h"
#include "gemini_key.h"         /* generated at build time from yaac/API_key.txt */

static const char *TAG = "gemini";

#define WS_URL          "wss://generativelanguage.googleapis.com/ws/" \
                        "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="
#define CHUNK_SAMPLES   512                     /* 32 ms at 16 kHz, as in the POC */
#define QUEUE_DEPTH     160                     /* ~5 s of audio while the session opens */
#define RX_MAX          (256 * 1024)            /* largest server message we reassemble */
#define TEXT_MAX        2048

extern const char tools_json_start[] asm("_binary_tools_json_start");
extern const char system_prompt_md_start[] asm("_binary_system_prompt_md_start");

typedef enum { ITEM_AUDIO, ITEM_END, ITEM_TEXT } item_type_t;

typedef struct {
    item_type_t type;
    uint16_t    samples;
    char       *text;           /* ITEM_TEXT: JSON to send (heap, freed after sending) */
    int16_t     pcm[CHUNK_SAMPLES];
} item_t;

static QueueHandle_t queue;
static cJSON *tools_root;       /* parsed tools.json (keeps x-range for validation) */
static esp_websocket_client_handle_t ws;
static volatile bool ws_connected, setup_sent, ready, closing;
static volatile bool want_session;
static volatile int64_t last_activity_us, connect_start_us;

/* Downlink reassembly + transcripts (only touched from the websocket task) */
static char *rx_buf;
static int rx_len;
static bool rx_overflow;
static char *user_text, *model_text;
static int audio_chunks, audio_bytes;

/* ---- helpers ------------------------------------------------------------- */

static void *psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }

/* Remove x-* and _* keys before sending the schema to Gemini (strip_private in the POC) */
static void strip_private(cJSON *o)
{
    if (!cJSON_IsObject(o) && !cJSON_IsArray(o)) return;
    cJSON *it = o->child;
    while (it) {
        cJSON *next = it->next;
        if (cJSON_IsObject(o) && it->string && (it->string[0] == '_' || strncmp(it->string, "x-", 2) == 0)) {
            cJSON_DeleteItemFromObjectCaseSensitive(o, it->string);
        } else {
            strip_private(it);
        }
        it = next;
    }
}

static void enqueue_text(char *json, bool front)
{
    static item_t it;   /* only used to carry the pointer; pcm is not copied for text items */
    it.type = ITEM_TEXT;
    it.samples = 0;
    it.text = json;
    const BaseType_t ok = front ? xQueueSendToFront(queue, &it, pdMS_TO_TICKS(100))
                                : xQueueSend(queue, &it, pdMS_TO_TICKS(100));
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping message");
        cJSON_free(json);
    }
}

static void append_text(char *dst, const char *s)
{
    const size_t used = strlen(dst);
    snprintf(dst + used, TEXT_MAX - used, "%s", s);
}

static void flush_transcripts(void)
{
    if (user_text[0]) ESP_LOGI(TAG, "USER: %s", user_text);
    if (model_text[0]) ESP_LOGI(TAG, "GEMINI: %s", model_text);
    user_text[0] = model_text[0] = 0;
}

/* ---- downlink ------------------------------------------------------------ */

static void handle_tool_call(const cJSON *tc)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *tr = cJSON_AddObjectToObject(resp, "toolResponse");
    cJSON *list = cJSON_AddArrayToObject(tr, "functionResponses");
    const cJSON *fc;
    cJSON_ArrayForEach(fc, cJSON_GetObjectItemCaseSensitive(tc, "functionCalls")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(fc, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(fc, "args");
        cJSON *result = coffee_machine_handle(cJSON_IsString(name) ? name->valuestring : NULL, args);
        char *a = args ? cJSON_PrintUnformatted(args) : NULL;
        char *r = cJSON_PrintUnformatted(result);
        ESP_LOGI(TAG, "[toolCall] %s(%s)", cJSON_IsString(name) ? name->valuestring : "?", a ? a : "{}");
        ESP_LOGI(TAG, "[response] %s", r ? r : "?");
        cJSON_free(a);
        cJSON_free(r);
        cJSON *one = cJSON_CreateObject();
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(fc, "id");
        if (id) cJSON_AddItemToObject(one, "id", cJSON_Duplicate(id, true));
        if (name) cJSON_AddItemToObject(one, "name", cJSON_Duplicate(name, true));
        cJSON_AddItemToObject(one, "response", result);
        cJSON_AddItemToArray(list, one);
    }
    enqueue_text(cJSON_PrintUnformatted(resp), true);   /* ahead of queued audio */
    cJSON_Delete(resp);
}

static void handle_message(const char *json, int len)
{
    cJSON *msg = cJSON_ParseWithLength(json, len);
    if (!msg) {
        ESP_LOGW(TAG, "unparsable server message (%d bytes)", len);
        return;
    }
    last_activity_us = esp_timer_get_time();

    if (cJSON_GetObjectItemCaseSensitive(msg, "setupComplete")) {
        ready = true;
        ESP_LOGI(TAG, "Session ready in %lld ms (TLS + setup)", (esp_timer_get_time() - connect_start_us) / 1000);
    }

    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(msg, "serverContent");
    if (sc) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(sc, "inputTranscription"), "text");
        if (cJSON_IsString(t)) {
            if (model_text[0]) flush_transcripts();     /* speaker changed */
            append_text(user_text, t->valuestring);
        }
        t = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(sc, "outputTranscription"), "text");
        if (cJSON_IsString(t)) {
            if (user_text[0]) {
                ESP_LOGI(TAG, "USER: %s", user_text);
                user_text[0] = 0;
            }
            append_text(model_text, t->valuestring);
        }
        const cJSON *part;
        cJSON_ArrayForEach(part, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(sc, "modelTurn"), "parts")) {
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(part, "inlineData"), "data");
            if (cJSON_IsString(data)) {
                audio_chunks++;
                audio_bytes += strlen(data->valuestring) * 3 / 4;     /* 24 kHz PCM, not played yet */
            }
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "interrupted"))) {
            ESP_LOGI(TAG, "[interrupted by user]");
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "turnComplete"))) {
            flush_transcripts();
            ESP_LOGI(TAG, "[turn complete: %d reply audio chunks, %.1f s of 24 kHz audio]",
                     audio_chunks, audio_bytes / (24000.0 * 2));
            audio_chunks = audio_bytes = 0;
        }
    }

    const cJSON *tc = cJSON_GetObjectItemCaseSensitive(msg, "toolCall");
    if (tc) {
        flush_transcripts();
        handle_tool_call(tc);
    }
    const cJSON *tcc = cJSON_GetObjectItemCaseSensitive(msg, "toolCallCancellation");
    if (tcc) {
        char *ids = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(tcc, "ids"));
        ESP_LOGI(TAG, "[toolCall cancelled: %s]", ids ? ids : "?");
        cJSON_free(ids);
    }
    const cJSON *ga = cJSON_GetObjectItemCaseSensitive(msg, "goAway");
    if (ga) {
        const cJSON *tl = cJSON_GetObjectItemCaseSensitive(ga, "timeLeft");
        ESP_LOGW(TAG, "[goAway: server will close in %s]", cJSON_IsString(tl) ? tl->valuestring : "?");
        closing = true;
    }
    cJSON_Delete(msg);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected (%lld ms)", (esp_timer_get_time() - connect_start_us) / 1000);
        ws_connected = true;
        break;
    case WEBSOCKET_EVENT_DATA:
        /* Text or binary JSON (the server often uses binary frames); large
         * frames arrive in pieces, fragmented messages as continuation frames */
        if (d->op_code == 0x8) {            /* close frame */
            if (d->data_len >= 2) {
                ESP_LOGW(TAG, "Server closed: code %d %.*s", ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1],
                         d->data_len - 2, d->data_ptr + 2);
            }
            break;
        }
        if (d->op_code != 0x0 && d->op_code != 0x1 && d->op_code != 0x2) break;   /* ping/pong */
        if (d->op_code != 0x0 && d->payload_offset == 0) {
            rx_len = 0;
            rx_overflow = false;
        }
        if (rx_len + d->data_len <= RX_MAX) {
            memcpy(rx_buf + rx_len, d->data_ptr, d->data_len);
            rx_len += d->data_len;
        } else {
            rx_overflow = true;
        }
        if (d->fin && d->payload_offset + d->data_len >= d->payload_len) {
            if (rx_overflow) {
                ESP_LOGW(TAG, "server message too large, skipped");
            } else {
                handle_message(rx_buf, rx_len);
            }
            rx_len = 0;
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        if (ws_connected || id == WEBSOCKET_EVENT_ERROR) {
            ESP_LOGW(TAG, "WebSocket %s", id == WEBSOCKET_EVENT_ERROR ? "error" :
                                         id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected");
        }
        ws_connected = false;
        ready = false;
        closing = true;
        break;
    default:
        break;
    }
}

/* ---- session ------------------------------------------------------------- */

static void session_open(void)
{
    static char uri[sizeof(WS_URL) + 128];
    snprintf(uri, sizeof(uri), "%s%s", WS_URL, GEMINI_API_KEY);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Task stack and rx/tx buffers are in PSRAM (vendored client, see VENDORED.md) */
        .buffer_size = 4096,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .task_stack = 8192,
        .task_prio = 5,
        .task_core_id_set = true,
        .task_core_id = 0,
        .ping_interval_sec = 20,
    };
    connect_start_us = last_activity_us = esp_timer_get_time();
    ws_connected = setup_sent = ready = closing = false;
    rx_len = 0;
    user_text[0] = model_text[0] = 0;
    ws = esp_websocket_client_init(&cfg);
    if (!ws) {
        ESP_LOGE(TAG, "websocket init failed");
        return;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    ESP_LOGI(TAG, "Connecting to Gemini Live (%s)... internal free %u", GEMINI_MODEL,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (esp_websocket_client_start(ws) != ESP_OK) {
        ESP_LOGE(TAG, "websocket start failed");
        esp_websocket_client_destroy(ws);
        ws = NULL;
    }
}

static void session_close(const char *why)
{
    if (!ws) return;
    ESP_LOGI(TAG, "Closing session (%s)", why);
    if (esp_websocket_client_is_connected(ws)) esp_websocket_client_close(ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(ws);
    ws = NULL;
    ws_connected = setup_sent = ready = closing = false;
    flush_transcripts();
}

static void send_setup(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", "models/" GEMINI_MODEL);
    cJSON *gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *mod = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON_AddItemToArray(mod, cJSON_CreateString("AUDIO"));
    cJSON *sys = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(sys, "parts");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "text", system_prompt_md_start);
    cJSON_AddItemToArray(parts, p);
    cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
    cJSON *t = cJSON_CreateObject();
    cJSON *decls = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(tools_root, "functionDeclarations"), true);
    strip_private(decls);
    cJSON_AddItemToObject(t, "functionDeclarations", decls);
    cJSON_AddItemToArray(tools, t);
    cJSON_AddObjectToObject(setup, "inputAudioTranscription");
    cJSON_AddObjectToObject(setup, "outputAudioTranscription");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const int len = strlen(json);
    const int sent = esp_websocket_client_send_text(ws, json, len, pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "setup sent (%d bytes%s)", len, sent == len ? "" : ", FAILED");
    cJSON_free(json);
    setup_sent = true;
}

static void send_audio(const item_t *it)
{
    static unsigned char b64[CHUNK_SAMPLES * 2 * 4 / 3 + 8];
    static char json[sizeof(b64) + 128];
    size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)it->pcm, it->samples * sizeof(int16_t));
    const int len = snprintf(json, sizeof(json),
                             "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%.*s\"}}}",
                             (int)olen, b64);
    esp_websocket_client_send_text(ws, json, len, pdMS_TO_TICKS(2000));
}

static void gemini_task(void *arg)
{
    static item_t it;
    for (;;) {
        /* Session management */
        if (ws && (closing || (!ws_connected && esp_timer_get_time() - connect_start_us > 15000000))) {
            session_close(closing ? "connection ended" : "connect timeout");
            want_session = false;       /* no reconnect loop; the next wake word reopens it */
        }
        if (want_session && !ws) session_open();
        if (ws && ws_connected && !setup_sent) send_setup();
        if (ws && ready && !want_session && uxQueueMessagesWaiting(queue) == 0 &&
            esp_timer_get_time() - last_activity_us > GEMINI_IDLE_CLOSE_S * 1000000LL) {
            session_close("idle");
        }

        /* Hold queued audio until the session is ready (setupComplete) */
        if (!ready) {
            if (!ws && !want_session) {
                while (xQueueReceive(queue, &it, 0) == pdTRUE) {
                    if (it.type == ITEM_TEXT) cJSON_free(it.text);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (xQueueReceive(queue, &it, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        last_activity_us = esp_timer_get_time();
        switch (it.type) {
        case ITEM_AUDIO:
            send_audio(&it);
            break;
        case ITEM_END: {
            static const char end_msg[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
            esp_websocket_client_send_text(ws, end_msg, strlen(end_msg), pdMS_TO_TICKS(2000));
            ESP_LOGI(TAG, "user turn sent, waiting for the reply...");
            break;
        }
        case ITEM_TEXT:
            esp_websocket_client_send_text(ws, it.text, strlen(it.text), pdMS_TO_TICKS(5000));
            cJSON_free(it.text);
            break;
        }
    }
}

/* ---- API ----------------------------------------------------------------- */

void gemini_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);    /* keep JSON (incl. large audio replies) out of internal RAM */

    tools_root = cJSON_Parse(tools_json_start);
    if (!tools_root || !cJSON_GetObjectItemCaseSensitive(tools_root, "functionDeclarations")) {
        ESP_LOGE(TAG, "tools.json does not parse");
        abort();
    }
    coffee_machine_init(cJSON_GetObjectItemCaseSensitive(tools_root, "functionDeclarations"));

    rx_buf = heap_caps_malloc(RX_MAX, MALLOC_CAP_SPIRAM);
    user_text = heap_caps_calloc(1, TEXT_MAX, MALLOC_CAP_SPIRAM);
    model_text = heap_caps_calloc(1, TEXT_MAX, MALLOC_CAP_SPIRAM);
    queue = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(item_t), MALLOC_CAP_SPIRAM);
    assert(rx_buf && user_text && model_text && queue);
    if (xTaskCreatePinnedToCoreWithCaps(gemini_task, "gemini", 8192, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start gemini task");
        abort();
    }
    ESP_LOGI(TAG, "Gemini Live client ready (model %s, prompt %u chars)", GEMINI_MODEL,
             (unsigned)strlen(system_prompt_md_start));
}

void gemini_turn_begin(void)
{
    want_session = true;
    last_activity_us = esp_timer_get_time();
}

void gemini_push_audio(const int16_t *pcm, int n)
{
    static item_t it;   /* only called from the audio fetch task */
    while (n > 0) {
        const int chunk = n < CHUNK_SAMPLES ? n : CHUNK_SAMPLES;
        it.type = ITEM_AUDIO;
        it.samples = chunk;
        it.text = NULL;
        memcpy(it.pcm, pcm, chunk * sizeof(int16_t));
        xQueueSend(queue, &it, 0);
        pcm += chunk;
        n -= chunk;
    }
}

void gemini_turn_end(void)
{
    static item_t it;
    it.type = ITEM_END;
    it.samples = 0;
    it.text = NULL;
    xQueueSend(queue, &it, 0);
    want_session = false;       /* the session stays open (idle timeout) for follow-ups */
}

bool gemini_session_ready(void) { return ready; }
