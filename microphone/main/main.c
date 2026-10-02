/*
 * Two-channel microphone oscilloscope for the Waveshare ESP32-S3-Touch-LCD-3.49.
 *
 * Audio: ES7210 4-channel ADC in TDM mode (as in Waveshare's codec_board setup),
 *        16 kHz / 16 bit, read through esp_codec_dev on I2S0 (DIN = GPIO6).
 *        TDM slot order measured on this board: 0 = MIC1, 1 = speaker echo reference
 *        (AEC; flat while the amp is off), 2 = MIC2, 3 = unused.
 * Screen: 640x172 landscape, MIC1 trace on top, MIC2 below, 4 ms/div grid,
 *         rising-edge trigger on MIC1 (free-running when the signal is too quiet),
 *         RMS level of the shown window in dBFS.
 * Log:    RMS of all four ES7210 channels once a second, to confirm channel order.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_power.h"
#include "display.h"
#include "scope_draw.h"

static const char *TAG = "mic_scope";

/* Pins (see docs/BOARD_REFERENCE.md) */
#define PIN_I2C_SDA     GPIO_NUM_47
#define PIN_I2C_SCL     GPIO_NUM_48
#define PIN_I2S_MCLK    GPIO_NUM_7
#define PIN_I2S_BCLK    GPIO_NUM_15
#define PIN_I2S_WS      GPIO_NUM_46
#define PIN_I2S_DIN     GPIO_NUM_6
#define EXIO_SPK_AMP    IO_EXPANDER_PIN_NUM_7

#define SAMPLE_RATE     16000
#define ADC_CHANNELS    4       /* ES7210 TDM slots */
#define CH_MIC1         0
#define CH_MIC2         2       /* not 1: slot 1 is the echo reference */
#define MIC_GAIN_DB     30.0f   /* ES7210 PGA */
#define READ_FRAMES     256

#define TRACE_GAIN      8       /* display zoom: +-32768/TRACE_GAIN fills half a pane */
#define TRIG_LEVEL      400     /* rising-edge trigger threshold on MIC1 (raw) */
#define WINDOW          SCREEN_W            /* samples on screen: 640 = 40 ms */
#define RING_SIZE       4096                /* per channel, power of two */
#define RING_MASK       (RING_SIZE - 1)

#define PANE_H          (SCREEN_H / 2)      /* 86 px per channel */
#define DIV_PX          64                  /* 64 samples = 4 ms per division */

#define COL_GRID        RGB(40, 40, 48)
#define COL_AXIS        RGB(70, 70, 84)
#define COL_MIC1        RGB(60, 255, 120)
#define COL_MIC2        RGB(255, 210, 40)
#define COL_TEXT        RGB(200, 200, 210)

static i2c_master_bus_handle_t i2c_bus;
static esp_codec_dev_handle_t mic;

static int16_t ring[2][RING_SIZE];
static volatile uint32_t ring_written;      /* total samples written per channel */

/* ---- Board bring-up ------------------------------------------------------ */

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
    display_set_backlight(0);
}

static void mic_init(void)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_DIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm_cfg));

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    es7210_codec_cfg_t es_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg),
        /* Selecting 3+ inputs puts the ES7210 in TDM mode (4 slots on SDOUT1) */
        .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4,
    };
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx,
    };
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_codec_new(&es_cfg),
        .data_if = audio_codec_new_i2s_data(&i2s_cfg),
    };
    mic = esp_codec_dev_new(&dev_cfg);
    assert(mic);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = ADC_CHANNELS,
        .sample_rate = SAMPLE_RATE,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_in_gain(mic, MIC_GAIN_DB);
    ESP_LOGI(TAG, "ES7210: %d ch TDM, %d Hz, gain %.1f dB", ADC_CHANNELS, SAMPLE_RATE, MIC_GAIN_DB);
}

/* ---- Audio capture (core 1) --------------------------------------------- */

static void audio_task(void *arg)
{
    static int16_t buf[READ_FRAMES * ADC_CHANNELS];
    double sum_sq[ADC_CHANNELS] = {0};
    double sum_diff = 0, sum_xy = 0;
    uint32_t frames = 0;
    for (;;) {
        if (esp_codec_dev_read(mic, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "mic read failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint32_t w = ring_written;
        for (int i = 0; i < READ_FRAMES; i++) {
            const int16_t *f = &buf[i * ADC_CHANNELS];
            ring[0][(w + i) & RING_MASK] = f[CH_MIC1];
            ring[1][(w + i) & RING_MASK] = f[CH_MIC2];
            for (int c = 0; c < ADC_CHANNELS; c++) sum_sq[c] += (double)f[c] * f[c];
            const double d = (double)f[CH_MIC1] - f[CH_MIC2];
            sum_diff += d * d;
            sum_xy += (double)f[CH_MIC1] * f[CH_MIC2];
        }
        ring_written = w + READ_FRAMES;

        frames += READ_FRAMES;
        if (frames >= SAMPLE_RATE) {
            /* MIC1 vs MIC2: two real mics give a non-zero difference and a
             * correlation below 1; a duplicated channel gives diff = 0, corr = 1. */
            const double corr = sum_xy / (sqrt(sum_sq[CH_MIC1] * sum_sq[CH_MIC2]) + 1e-9);
            ESP_LOGI(TAG, "MIC1-MIC2 diff %6.1f dBFS, correlation %.3f",
                     20.0 * log10(sqrt(sum_diff / frames) / 32768.0 + 1e-9), corr);
            sum_diff = sum_xy = 0;

            char line[96];
            int n = 0;
            for (int c = 0; c < ADC_CHANNELS; c++) {
                const double rms = sqrt(sum_sq[c] / frames);
                n += snprintf(line + n, sizeof(line) - n, " ch%d %6.1f", c, 20.0 * log10(rms / 32768.0 + 1e-9));
                sum_sq[c] = 0;
            }
            ESP_LOGI(TAG, "RMS dBFS:%s", line);
            frames = 0;
        }
    }
}

/* ---- Scope (core 0) ----------------------------------------------------- */

static void draw_grid(void)
{
    for (int x = 0; x < SCREEN_W; x += DIV_PX) {
        draw_vline_dotted(x, 0, SCREEN_H - 1, 3, COL_GRID);
    }
    for (int p = 0; p < 2; p++) {
        const int top = p * PANE_H, mid = top + PANE_H / 2;
        draw_hline_dotted(0, SCREEN_W - 1, top + PANE_H / 4, 4, COL_GRID);
        draw_hline_dotted(0, SCREEN_W - 1, top + 3 * PANE_H / 4, 4, COL_GRID);
        draw_hline_dotted(0, SCREEN_W - 1, mid, 2, COL_AXIS);
    }
    for (int x = 0; x < SCREEN_W; x++) draw_pixel(x, PANE_H, COL_AXIS);
}

static int sample_to_y(int pane, int s)
{
    const int half = PANE_H / 2 - 1;
    int dy = s * TRACE_GAIN * half / 32768;
    if (dy > half) dy = half;
    if (dy < -half) dy = -half;
    return pane * PANE_H + PANE_H / 2 - dy;
}

static void draw_trace(int pane, const int16_t *s, uint16_t color)
{
    int prev = sample_to_y(pane, s[0]);
    for (int x = 0; x < WINDOW; x++) {
        const int y = sample_to_y(pane, s[x]);
        draw_vline(x, prev, y, color);      /* connect to the previous sample */
        prev = y;
    }
}

static float window_dbfs(const int16_t *s)
{
    double acc = 0;
    for (int i = 0; i < WINDOW; i++) acc += (double)s[i] * s[i];
    return 20.0f * log10f(sqrtf((float)(acc / WINDOW)) / 32768.0f + 1e-9f);
}

static void scope_task(void *arg)
{
    static int16_t snap[2][2 * WINDOW];
    char text[32];
    int64_t last_fps_us = esp_timer_get_time();
    int frames = 0, fps = 0;

    for (;;) {
        /* Latest 2 screens of samples, oldest first */
        const uint32_t end = ring_written;
        const uint32_t start = end - 2 * WINDOW;
        for (int i = 0; i < 2 * WINDOW; i++) {
            snap[0][i] = ring[0][(start + i) & RING_MASK];
            snap[1][i] = ring[1][(start + i) & RING_MASK];
        }

        /* Rising edge through TRIG_LEVEL on MIC1 in the first screen */
        int trig = -1;
        for (int i = 1; i < WINDOW; i++) {
            if (snap[0][i - 1] < TRIG_LEVEL && snap[0][i] >= TRIG_LEVEL) {
                trig = i;
                break;
            }
        }
        const int off = trig >= 0 ? trig : WINDOW;     /* free-run: newest screen */

        draw_clear(0);
        draw_grid();
        draw_trace(0, &snap[0][off], COL_MIC1);
        draw_trace(1, &snap[1][off], COL_MIC2);

        snprintf(text, sizeof(text), "MIC1 %5.1f dBFS", window_dbfs(&snap[0][off]));
        draw_text(4, 3, text, 2, COL_MIC1);
        snprintf(text, sizeof(text), "MIC2 %5.1f dBFS", window_dbfs(&snap[1][off]));
        draw_text(4, PANE_H + 3, text, 2, COL_MIC2);
        snprintf(text, sizeof(text), "16 KHZ  4 MS/DIV  X%d", TRACE_GAIN);
        draw_text(SCREEN_W - 4 - 6 * (int)strlen(text), 3, text, 1, COL_TEXT);
        snprintf(text, sizeof(text), "%s  %d FPS", trig >= 0 ? "TRIG MIC1" : "AUTO", fps);
        draw_text(SCREEN_W - 4 - 6 * (int)strlen(text), PANE_H + 3, text, 1, COL_TEXT);

        display_present();

        frames++;
        const int64_t now = esp_timer_get_time();
        if (now - last_fps_us >= 1000000) {
            fps = frames;
            frames = 0;
            last_fps_us = now;
        }
        vTaskDelay(1);      /* let lower-priority tasks on this core run */
    }
}

void app_main(void)
{
    i2c_init();
    ESP_ERROR_CHECK(board_power_init(i2c_bus, on_power_off));      /* first: hold battery power on */

    /* Speaker amp off: this example only records */
    esp_io_expander_handle_t io = board_power_io_expander();
    esp_io_expander_set_dir(io, EXIO_SPK_AMP, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, EXIO_SPK_AMP, 0);

    draw_bind(display_init());
    mic_init();

    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(scope_task, "scope", 6144, NULL, 5, NULL, 0);
}
