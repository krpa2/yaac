/*
 * YAAC - Yet Another Alexa Clone, for the Waveshare ESP32-S3-Touch-LCD-3.49.
 *
 * Stage 1: offline wake word.
 *   ES7210 (MIC1, echo reference, MIC2) -> ESP-SR AFE:
 *     AEC (echo cancellation against the speaker reference) -> SE (2-mic speech
 *     enhancement) -> VAD -> WakeNet9l "Hey GiGi"
 *   On the wake word: a chime on the speaker (cancelled by AEC, so it does not
 *   re-trigger) and the screen switches to LISTENING with a live trace of the
 *   cleaned-up audio until the speaker goes quiet.
 *
 *   While listening, the processed audio (from the wake word on) is streamed as
 *   UDP broadcast on the local subnet, port 5577 (net_stream.h); pc/yaac_scope.py shows it.
 *
 * Tasks: feed (core 0) reads the ES7210 and feeds the AFE; fetch (core 1) gets
 * processed audio + wake/VAD results; ui (core 0) draws ~15 fps; chime plays tones.
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_vad.h"
#include "model_path.h"
#include "board_power.h"
#include "audio_io.h"
#include "audio_post.h"
#include "display.h"
#include "net_stream.h"
#include "gemini_live.h"
#include "scope_draw.h"

static const char *TAG = "yaac";

#define PIN_I2C_SDA         GPIO_NUM_47
#define PIN_I2C_SCL         GPIO_NUM_48

#define WAKE_MODEL_KEY      "heygigi"       /* matched against the model names in the partition */
#define WAKE_WORD_TEXT      "HEY GIGI"

#define LISTEN_TIMEOUT_MS   30000   /* hard limit: stop listening this long after the wake word */

#define LISTEN_SILENCE_MS   1200    /* ...or after this much silence following speech */

/* Audio tuning (see README "Audio tuning"). With WakeNet active, ESP-SR's AFE
 * refuses WebRTC AGC/NS, so they run after the AFE (audio_post.c) on the audio
 * that feeds the screen and the UDP stream; wake word detection is unaffected. */
#define AFE_OUTPUT_GAIN         4.0f    /* AFE linear output gain (0.1..10): +12 dB */
#define POST_NS_MODE            1       /* WebRTC NS: -1 off, 0 mild, 1 medium, 2 aggressive */
#define POST_AGC                true    /* WebRTC AGC, adaptive digital */
#define AGC_TARGET_LEVEL_DBFS   3       /* AGC output target: -3 dBFS */
#define AGC_COMPRESSION_GAIN_DB 15      /* AGC max boost for weak speech */
#define SCOPE_ZOOM              2       /* on-screen trace zoom */

/* Scope timebase: each of the 640 columns shows the min/max of SCOPE_DECIMATE
 * samples, so the screen spans 640 * 16 / 16 kHz = 640 ms */
#define SCOPE_DECIMATE      16
#define SCOPE_SAMPLES       (SCREEN_W * SCOPE_DECIMATE)
#define SCOPE_RING          16384
#define SCOPE_MASK          (SCOPE_RING - 1)
#define PREROLL_MAX         AUDIO_SAMPLE_RATE   /* stream up to 1 s of audio from before the wake */

#define COL_BG_IDLE         RGB(0, 0, 0)
#define COL_BG_LISTEN       RGB(0, 24, 48)
#define COL_TITLE           RGB(0, 200, 255)
#define COL_TEXT            RGB(200, 200, 210)
#define COL_DIM             RGB(90, 90, 100)
#define COL_TRACE           RGB(80, 220, 255)
#define COL_VOICE           RGB(60, 255, 120)
#define COL_METER           RGB(255, 200, 40)

typedef enum { STATE_IDLE, STATE_LISTENING } yaac_state_t;

static i2c_master_bus_handle_t i2c_bus;
static const esp_afe_sr_iface_t *afe;
static esp_afe_sr_data_t *afe_data;
static TaskHandle_t chime_task_handle;

/* Shared between fetch and ui tasks (single writer each, small values) */
static volatile yaac_state_t state = STATE_IDLE;
static volatile int64_t listen_start_us, last_speech_us;
static volatile bool voice;
static volatile float volume_dbfs = -90.0f;
static volatile int wake_count;
static double session_sum_sq;   /* output (post-AGC) energy of the current session */
static uint32_t session_samples;
static int session_peak;
static int16_t *scope_ring;     /* SCOPE_RING samples in PSRAM (internal RAM is needed by Wi-Fi) */

/* Heartbeat counters (see heartbeat_task) */
static volatile uint32_t ui_frames, fetch_loops, feed_loops;
static volatile uint32_t scope_written;

/* ---- Bring-up ------------------------------------------------------------ */

static void i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus));
}

static void on_power_off(void)
{
    audio_mute();
    display_set_backlight(0);
}

static void afe_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num == 0) {
        ESP_LOGE(TAG, "No ESP-SR models found in the \"model\" partition (was srmodels.bin flashed?)");
        abort();
    }
    for (int i = 0; i < models->num; i++) ESP_LOGI(TAG, "model: %s", models->model_name[i]);

    afe_config_t *cfg = afe_config_init(AUDIO_IN_FORMAT, models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    cfg->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, WAKE_MODEL_KEY);
    if (!cfg->wakenet_model_name) {
        ESP_LOGE(TAG, "Wake word model \"%s\" not in the model partition", WAKE_MODEL_KEY);
        abort();
    }
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 5;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    /* AGC/NS run after the AFE instead (audio_post.c): inside it, WakeNet forces
     * them off / to wakenet-AGC mode */
    cfg->agc_init = false;
    cfg->ns_init = false;
    cfg->afe_linear_gain = AFE_OUTPUT_GAIN;
    cfg = afe_config_check(cfg);
    afe_config_print(cfg);

    afe = esp_afe_handle_from_config(cfg);
    afe_data = afe->create_from_config(cfg);
    afe_config_free(cfg);
    assert(afe_data);
    afe->print_pipeline(afe_data);
    ESP_LOGI(TAG, "AFE ready: feed %d samples x %d ch, fetch %d samples, %d Hz",
             afe->get_feed_chunksize(afe_data), afe->get_feed_channel_num(afe_data),
             afe->get_fetch_chunksize(afe_data), afe->get_samp_rate(afe_data));
}

/* ---- Audio tasks --------------------------------------------------------- */

static void feed_task(void *arg)
{
    const int chunk = afe->get_feed_chunksize(afe_data);
    const int channels = afe->get_feed_channel_num(afe_data);
    int16_t *buf = heap_caps_malloc(chunk * channels * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(buf && channels == AUDIO_IN_CHANNELS);
    for (;;) {
        if (audio_read(buf, chunk) == 0) {
            afe->feed(afe_data, buf);
            feed_loops++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void fetch_task(void *arg)
{
    for (;;) {
        afe_fetch_result_t *res = afe->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;
        fetch_loops++;

        /* WebRTC NS + AGC on the AFE output; everything below uses the result */
        static int16_t out[NET_STREAM_MAX_SAMPLES + AUDIO_POST_FRAME];
        int in_n = res->data_size / sizeof(int16_t);
        if (in_n > NET_STREAM_MAX_SAMPLES) in_n = NET_STREAM_MAX_SAMPLES;
        const int n = audio_post_process(res->data, in_n, out);

        const uint32_t w = scope_written;
        double sq = 0;
        for (int i = 0; i < n; i++) {
            scope_ring[(w + i) & SCOPE_MASK] = out[i];
            sq += (double)out[i] * out[i];
        }
        scope_written = w + n;
        if (n) volume_dbfs = 20.0f * log10f(sqrtf((float)(sq / n)) / 32768.0f + 1e-9f);

        const int64_t now = esp_timer_get_time();
        voice = res->vad_state == VAD_SPEECH;
        if (voice) last_speech_us = now;

        if (res->wakeup_state == WAKENET_DETECTED) {
            wake_count++;
            ESP_LOGI(TAG, "\"%s\" detected (#%d, channel %d, volume %.1f dBFS)",
                     WAKE_WORD_TEXT, wake_count, res->trigger_channel_id, res->data_volume);
            listen_start_us = last_speech_us = now;
            state = STATE_LISTENING;
            xTaskNotifyGive(chime_task_handle);

            /* Start a network session with the wake word itself as pre-roll
             * (already in the scope ring, including this chunk) */
            if (net_stream_active()) net_stream_end();
            net_stream_begin(AUDIO_SAMPLE_RATE);
            gemini_turn_begin();
            session_sum_sq = 0;
            session_samples = 0;
            session_peak = 0;
            int pre = res->wake_word_length + n;
            if (pre > PREROLL_MAX) pre = PREROLL_MAX;
            const uint32_t end = scope_written;
            for (int i = 0; i < pre; i += NET_STREAM_MAX_SAMPLES) {
                int16_t chunk[NET_STREAM_MAX_SAMPLES];
                const int len = pre - i < NET_STREAM_MAX_SAMPLES ? pre - i : NET_STREAM_MAX_SAMPLES;
                for (int k = 0; k < len; k++) chunk[k] = scope_ring[(end - pre + i + k) & SCOPE_MASK];
                net_stream_push(chunk, len);
            }
        } else if (state == STATE_LISTENING) {
            net_stream_push(out, n);
            gemini_push_audio(out, n);      /* only audio after the wake word */
        }

        if (state == STATE_LISTENING) {
            for (int i = 0; i < n; i++) {
                const int s = out[i];
                session_sum_sq += (double)s * s;
                if (abs(s) > session_peak) session_peak = abs(s);
            }
            session_samples += n;
            const bool timeout = now - listen_start_us > LISTEN_TIMEOUT_MS * 1000LL;
            const bool silence = now - last_speech_us > LISTEN_SILENCE_MS * 1000LL &&
                                 now - listen_start_us > LISTEN_SILENCE_MS * 1000LL;
            if (timeout || silence) {
                const double rms = sqrt(session_sum_sq / (session_samples ? session_samples : 1));
                ESP_LOGI(TAG, "Back to idle (%s) after %.1f s; output RMS %.1f dBFS, peak %.1f dBFS",
                         timeout ? "timeout" : "silence", (now - listen_start_us) / 1e6,
                         20.0 * log10(rms / 32768.0 + 1e-9), 20.0 * log10(session_peak / 32768.0 + 1e-9));
                state = STATE_IDLE;
                net_stream_end();
                gemini_turn_end();
            }
        }
    }
}

/* Two rising tones with a soft envelope */
static void chime_task(void *arg)
{
    static int16_t tone[AUDIO_SAMPLE_RATE / 8];     /* 125 ms */
    const float freqs[2] = {880.0f, 1318.5f};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (int t = 0; t < 2; t++) {
            const int len = sizeof(tone) / sizeof(tone[0]);
            for (int i = 0; i < len; i++) {
                const float env = sinf((float)M_PI * i / len);     /* no clicks */
                tone[i] = (int16_t)(9000.0f * env * sinf(2.0f * (float)M_PI * freqs[t] * i / AUDIO_SAMPLE_RATE));
            }
            audio_play(tone, len);
        }
    }
}

/* ---- UI ------------------------------------------------------------------ */

static int text_width(const char *s, int scale) { return (int)strlen(s) * 6 * scale - scale; }

static void draw_centered(int y, const char *s, int scale, uint16_t color)
{
    draw_text((SCREEN_W - text_width(s, scale)) / 2, y, s, scale, color);
}

static void ui_task(void *arg)
{
    int16_t *snap = heap_caps_malloc(SCOPE_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(snap);
    char text[48];
    for (;;) {
        const bool listening = state == STATE_LISTENING;
        draw_clear(listening ? COL_BG_LISTEN : COL_BG_IDLE);

        draw_text(8, 6, "YAAC", 3, COL_TITLE);
        draw_text(8 + text_width("YAAC", 3) + 10, 13, "YET ANOTHER ALEXA CLONE", 1, COL_DIM);
        if (net_stream_connected()) {
            snprintf(text, sizeof(text), "WAKES %d   WIFI %s", wake_count, net_stream_ip());
        } else {
            snprintf(text, sizeof(text), "WAKES %d   WIFI CONNECTING...", wake_count);
        }
        draw_text(SCREEN_W - 8 - text_width(text, 1), 13, text, 1, net_stream_connected() ? COL_DIM : COL_METER);
        if (net_stream_active()) {
            draw_text(SCREEN_W - 8 - text_width("STREAMING UDP", 1), 26, "STREAMING UDP", 1, COL_VOICE);
        }

        if (listening) {
            draw_centered(40, "LISTENING...", 4, COL_TEXT);
        } else {
            draw_centered(40, "SAY \"" WAKE_WORD_TEXT "\"", 4, COL_TEXT);
        }

        /* Processed (AEC + SE) audio, latest 640 ms as a min/max envelope per column */
        const uint32_t end = scope_written;
        for (int i = 0; i < SCOPE_SAMPLES; i++) snap[i] = scope_ring[(end - SCOPE_SAMPLES + i) & SCOPE_MASK];
        const int mid = 112, half = 26;
        draw_hline_dotted(0, SCREEN_W - 1, mid, 4, COL_DIM);
        for (int x = 0; x < SCREEN_W; x += SCREEN_W / 8) {
            draw_vline_dotted(x, mid - half, mid + half, 3, COL_DIM);   /* 80 ms per division */
        }
        for (int x = 0; x < SCREEN_W; x++) {
            int lo = 32767, hi = -32768;
            for (int k = 0; k < SCOPE_DECIMATE; k++) {
                const int s = snap[x * SCOPE_DECIMATE + k];
                if (s < lo) lo = s;
                if (s > hi) hi = s;
            }
            int y_hi = mid - hi * SCOPE_ZOOM * half / 32768;
            int y_lo = mid - lo * SCOPE_ZOOM * half / 32768;
            if (y_hi < mid - half) y_hi = mid - half;
            if (y_lo > mid + half) y_lo = mid + half;
            draw_vline(x, y_hi, y_lo, COL_TRACE);
        }
        draw_text(SCREEN_W - 8 - text_width("80 MS/DIV", 1), mid - half, "80 MS/DIV", 1, COL_DIM);

        /* Level meter (-90..0 dBFS) + VAD */
        const float v = volume_dbfs;
        int bar = (int)((v + 90.0f) / 90.0f * (SCREEN_W - 200));
        if (bar < 0) bar = 0;
        draw_fill_rect(8, 150, SCREEN_W - 200, 12, RGB(30, 30, 36));
        draw_fill_rect(8, 150, bar, 12, COL_METER);
        snprintf(text, sizeof(text), "%5.1f dBFS", v);
        draw_text(SCREEN_W - 184, 152, text, 1, COL_TEXT);
        draw_text(SCREEN_W - 96, 150, voice ? "VOICE" : "-", 2, voice ? COL_VOICE : COL_DIM);

        display_present();
        ui_frames++;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

/* Every 5 s: progress counters, memory, and every task's state and CPU share.
 * Printed with esp_rom_printf so it still gets out if the normal log path
 * (stdout lock) is stuck; runs at high priority on core 1 so it isn't starved. */
#define HEARTBEAT_S             30
#define HEARTBEAT_TASK_TABLES   0   /* 1: also dump every task's state and CPU share */

static void heartbeat_task(void *arg)
{
    static char list[2048];
    uint32_t ui0 = 0, fe0 = 0, fd0 = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_S * 1000));
        const uint32_t ui = ui_frames, fe = fetch_loops, fd = feed_loops;
        /* esp_rom_printf has no %f: print rates in tenths */
        const unsigned ui10 = (ui - ui0) * 10 / HEARTBEAT_S, fe10 = (fe - fe0) * 10 / HEARTBEAT_S,
                       fd10 = (fd - fd0) * 10 / HEARTBEAT_S;
        esp_rom_printf("[hb] ui %u.%u fps, fetch %u.%u/s, feed %u.%u/s | internal free %u (min %u) psram %u | wifi %s\n",
                       ui10 / 10, ui10 % 10, fe10 / 10, fe10 % 10, fd10 / 10, fd10 % 10,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                       net_stream_connected() ? net_stream_ip() : "down");
        ui0 = ui; fe0 = fe; fd0 = fd;
        if (HEARTBEAT_TASK_TABLES) {
            vTaskList(list);
            esp_rom_printf("[hb] name          state prio stack num core\n%s", list);
            vTaskGetRunTimeStats(list);
            esp_rom_printf("[hb] name          run-time     %%cpu\n%s", list);
        }
    }
}

static void start_task(TaskFunction_t fn, const char *name, uint32_t stack, UBaseType_t prio,
                       TaskHandle_t *handle, BaseType_t core)
{
    if (xTaskCreatePinnedToCoreWithCaps(fn, name, stack, NULL, prio, handle, core, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start task %s", name);
        abort();
    }
}

void app_main(void)
{
    i2c_init();
    ESP_ERROR_CHECK(board_power_init(i2c_bus, on_power_off));      /* first: hold battery power on */
    scope_ring = heap_caps_calloc(SCOPE_RING, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(scope_ring);

    draw_bind(display_init());
    draw_clear(0);
    draw_centered(70, "YAAC STARTING...", 3, COL_TEXT);
    display_present();

    audio_io_init(i2c_bus);
    net_stream_init();      /* connects in the background */
    afe_init();
    const audio_post_config_t post = {
        .ns_mode = POST_NS_MODE,
        .agc = POST_AGC,
        .agc_target_level_dbfs = AGC_TARGET_LEVEL_DBFS,
        .agc_compression_gain_db = AGC_COMPRESSION_GAIN_DB,
    };
    audio_post_init(&post);
    gemini_init();

    ESP_LOGI(TAG, "After AFE: internal free %u (largest block %u), psram free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Internal RAM is almost all taken by Wi-Fi and the AFE: give the app's
     * tasks PSRAM stacks (none of them run while flash is being written) */
    start_task(chime_task, "chime", 4096, 6, &chime_task_handle, 0);
    start_task(feed_task, "afe_feed", 6144, 8, NULL, 0);
    start_task(fetch_task, "afe_fetch", 6144, 8, NULL, 1);
    start_task(ui_task, "ui", 6144, 3, NULL, 0);
    start_task(heartbeat_task, "heartbeat", 4096, 20, NULL, 1);
    ESP_LOGI(TAG, "Ready - say \"%s\" (internal free %u)", WAKE_WORD_TEXT,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
