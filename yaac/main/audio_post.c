#include <string.h>
#include "esp_log.h"
#include "esp_agc.h"
#include "esp_ns.h"
#include "audio_post.h"
#include "audio_io.h"

static const char *TAG = "audio_post";

static ns_handle_t ns;
static void *agc;
static int16_t fifo[AUDIO_POST_FRAME];
static int fifo_len;

void audio_post_init(const audio_post_config_t *cfg)
{
    if (cfg->ns_mode >= 0) {
        ns = ns_pro_create(10, cfg->ns_mode, AUDIO_SAMPLE_RATE);
        if (!ns) ESP_LOGE(TAG, "WebRTC NS create failed");
    }
    if (cfg->agc) {
        agc = esp_agc_open(AGC_MODE_2, AUDIO_SAMPLE_RATE);
        if (agc) {
            set_agc_config(agc, cfg->agc_compression_gain_db, 1, cfg->agc_target_level_dbfs);
        } else {
            ESP_LOGE(TAG, "WebRTC AGC create failed");
        }
    }
    ESP_LOGI(TAG, "WebRTC NS %s, AGC %s (target -%d dBFS, compression %d dB, limiter on)",
             ns ? (const char *[]){"mild", "medium", "aggressive"}[cfg->ns_mode] : "off",
             agc ? "adaptive digital" : "off", cfg->agc_target_level_dbfs, cfg->agc_compression_gain_db);
}

int audio_post_process(const int16_t *in, int n, int16_t *out)
{
    int written = 0;
    while (n > 0) {
        const int take = AUDIO_POST_FRAME - fifo_len < n ? AUDIO_POST_FRAME - fifo_len : n;
        memcpy(&fifo[fifo_len], in, take * sizeof(int16_t));
        fifo_len += take;
        in += take;
        n -= take;
        if (fifo_len < AUDIO_POST_FRAME) break;

        int16_t tmp[AUDIO_POST_FRAME];
        int16_t *frame = fifo;
        if (ns) {
            ns_process(ns, frame, tmp);
            frame = tmp;
        }
        if (agc) {
            esp_agc_process(agc, frame, &out[written], AUDIO_POST_FRAME, AUDIO_SAMPLE_RATE);
        } else {
            memcpy(&out[written], frame, sizeof(tmp));
        }
        written += AUDIO_POST_FRAME;
        fifo_len = 0;
    }
    return written;
}
