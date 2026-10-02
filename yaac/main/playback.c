#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "audio_io.h"
#include "playback.h"

static const char *TAG = "playback";

#define RING_SAMPLES    (AUDIO_SAMPLE_RATE * 20)    /* 20 s of 16 kHz audio (640 KB PSRAM) */
#define OUT_CHUNK       256                         /* samples per I2S write (16 ms) */

/* 24 kHz -> 16 kHz: low-pass FIR (cutoff 7 kHz, below the new 8 kHz Nyquist)
 * on the 24 kHz input, then linear interpolation every 1.5 input samples */
#define FIR_TAPS        31
#define FIR_CUTOFF_HZ   7000.0f

static int16_t *ring;
static uint32_t rd, wr;         /* free-running sample counters */
static SemaphoreHandle_t lock;
static TaskHandle_t player;
static volatile bool playing;

static float fir[FIR_TAPS];
static float hist[FIR_TAPS];
static int hist_pos;
static int64_t in_index;        /* filtered 24 kHz samples produced so far */
static double next_t;           /* input-sample position of the next 16 kHz output */
static float y_prev;

static void fir_init(void)
{
    const float fc = FIR_CUTOFF_HZ / 24000.0f;     /* normalised to the 24 kHz input */
    float sum = 0;
    for (int i = 0; i < FIR_TAPS; i++) {
        const int m = i - FIR_TAPS / 2;
        const float sinc = m == 0 ? 2 * fc : sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m);
        const float hamming = 0.54f - 0.46f * cosf(2 * (float)M_PI * i / (FIR_TAPS - 1));
        fir[i] = sinc * hamming;
        sum += fir[i];
    }
    for (int i = 0; i < FIR_TAPS; i++) fir[i] /= sum;     /* unity gain at DC */
}

static void resampler_reset(void)
{
    memset(hist, 0, sizeof(hist));
    hist_pos = 0;
    in_index = 0;
    next_t = 0;
    y_prev = 0;
}

static void ring_write(const int16_t *pcm, int n)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint32_t free_space = RING_SAMPLES - (wr - rd);
    if ((uint32_t)n > free_space) n = free_space;
    for (int i = 0; i < n; i++) ring[(wr + i) % RING_SAMPLES] = pcm[i];
    wr += n;
    xSemaphoreGive(lock);
    if (n) xTaskNotifyGive(player);
}

static void player_task(void *arg)
{
    static int16_t chunk[OUT_CHUNK];
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        uint32_t avail = wr - rd;
        int n = avail < OUT_CHUNK ? avail : OUT_CHUNK;
        for (int i = 0; i < n; i++) chunk[i] = ring[(rd + i) % RING_SAMPLES];
        rd += n;
        xSemaphoreGive(lock);

        if (n == 0) {
            playing = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* wait for new audio */
            continue;
        }
        playing = true;
        audio_play(chunk, n);       /* blocks at the I2S rate */
    }
}

void playback_init(void)
{
    ring = heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    lock = xSemaphoreCreateMutex();
    assert(ring && lock);
    fir_init();
    resampler_reset();
    if (xTaskCreatePinnedToCoreWithCaps(player_task, "player", 4096, NULL, 6, &player, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start player task");
        abort();
    }
    ESP_LOGI(TAG, "Speaker playback ready (%d s buffer, 24 kHz replies resampled to %d Hz)",
             RING_SAMPLES / AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE);
}

void playback_push(const int16_t *pcm, int n)
{
    ring_write(pcm, n);
}

void playback_push_24k(const int16_t *pcm, int n)
{
    int16_t out[256];
    int out_n = 0;
    for (int i = 0; i < n; i++) {
        hist[hist_pos] = pcm[i];
        float y = 0;
        int k = hist_pos;
        for (int t = 0; t < FIR_TAPS; t++) {
            y += fir[t] * hist[k];
            k = k == 0 ? FIR_TAPS - 1 : k - 1;
        }
        hist_pos = (hist_pos + 1) % FIR_TAPS;

        /* Outputs whose position lies between filtered samples in_index-1 and in_index */
        while (next_t <= in_index) {
            const float frac = (float)(next_t - (in_index - 1));
            float v = y_prev + frac * (y - y_prev);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[out_n++] = (int16_t)v;
            next_t += 1.5;
            if (out_n == (int)(sizeof(out) / sizeof(out[0]))) {
                ring_write(out, out_n);
                out_n = 0;
            }
        }
        y_prev = y;
        in_index++;
    }
    if (out_n) ring_write(out, out_n);
}

void playback_clear(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    rd = wr;
    resampler_reset();
    xSemaphoreGive(lock);
}

bool playback_active(void)
{
    return playing || wr != rd;
}
