/*
 * AMY synthesizer test for the Waveshare ESP32-S3-Touch-LCD-3.49.
 * PLAY_MIDI_FILES 1: plays every MIDI file embedded from the ../assets folder in turn
 *                    (midi_player.c: GM programs -> AMY Juno/DX7 patches, ch10 drums).
 * PLAY_MIDI_FILES 0 (or no MIDI files): plays SONG from song_*.c:
 *   song_korobeiniki()           Tetris theme A as a 4-channel chiptune
 *   song_imperial_march_piano()  The Imperial March on AMY's piano (patch 256)
 *
 * Audio path: AMY (44.1 kHz, 16-bit stereo, rendering split over both cores)
 *             -> esp_codec_dev -> I2S0 -> ES8311 DAC -> speaker amp (TCA9554 EXIO7).
 * AMY runs with audio = AMY_AUDIO_IS_NONE: it only renders blocks, and this
 * file owns the I2S/codec, so the board's ES8311 setup stays the same as ../sound.
 */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander_tca9554.h"
#include "board_power.h"
#include "amy.h"
#include "amy_player.h"
#include "midi_file.h"
#include "midi_player.h"
#include "midi_playlist.h"

static const char *TAG = "amy_test";

/* Pins (see docs/BOARD_REFERENCE.md) */
#define PIN_I2C_SDA     GPIO_NUM_47
#define PIN_I2C_SCL     GPIO_NUM_48
#define PIN_I2S_MCLK    GPIO_NUM_7
#define PIN_I2S_BCLK    GPIO_NUM_15
#define PIN_I2S_WS      GPIO_NUM_46
#define PIN_I2S_DOUT    GPIO_NUM_45
#define EXIO_SPK_AMP    IO_EXPANDER_PIN_NUM_7

#define OUTPUT_VOLUME   85      /* ES8311 volume 0..100 */
#define AMY_VOLUME      1.0f    /* extra gain on top of each song's volume; AMY soft-clips above full scale */

#define PLAY_MIDI_FILES 1
#define SONG            song_korobeiniki
#define LOOKAHEAD_MS    50      /* schedule notes this far ahead of the audio clock */

static i2c_master_bus_handle_t i2c_bus;
static i2s_chan_handle_t i2s_out;
static esp_codec_dev_handle_t codec;

/* ---- Board audio bring-up (same as ../sound) -------------------------- */

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

static void speaker_amp_enable(void)
{
    /* The TCA9554 handle belongs to board_power (it also holds the power latch) */
    esp_io_expander_handle_t io = board_power_io_expander();
    ESP_ERROR_CHECK(esp_io_expander_set_dir(io, EXIO_SPK_AMP, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io, EXIO_SPK_AMP, 1));
}

/* Long press on PWR: silence the speaker before the power goes */
static void on_power_off(void)
{
    esp_io_expander_set_level(board_power_io_expander(), EXIO_SPK_AMP, 0);
    if (codec) esp_codec_dev_set_out_vol(codec, 0);
}

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_out, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AMY_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_out, &std_cfg));
}

static void codec_init(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg),
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,           /* amplifier is on the IO expander instead */
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = i2s_out,
    };
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_codec_new(&es_cfg),
        .data_if = audio_codec_new_i2s_data(&i2s_cfg),
    };
    codec = esp_codec_dev_new(&dev_cfg);
    assert(codec);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AMY_SAMPLE_RATE,
        .channel = AMY_NCHANS,
        .bits_per_sample = 16,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(codec, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_out_vol(codec, OUTPUT_VOLUME);
}

/* ---- AMY -------------------------------------------------------------- */

static amy_config_t base_config(void)
{
    amy_config_t cfg = amy_default_config();
    cfg.audio = AMY_AUDIO_IS_NONE;      /* we write the blocks to the codec ourselves */
    cfg.midi = AMY_MIDI_IS_NONE;
    cfg.features.default_synths = 0;
    cfg.features.startup_bleep = 0;
    cfg.platform.multicore = 1;         /* AMY render helper task on core 0 */
    cfg.platform.multithread = 0;       /* amy_update() renders in our audio task */
    return cfg;
}

/* Render one block and hand it to the codec (this blocks on I2S, which is what
 * paces AMY's clock); logs the render load every 5 s. */
static void render_block(void)
{
    static int64_t peak_us, sum_us, blocks, last_report_us;
    const int64_t t0 = esp_timer_get_time();
    int16_t *block = amy_update();
    const int64_t dt = esp_timer_get_time() - t0;
    if (dt > peak_us) peak_us = dt;
    sum_us += dt;
    blocks++;
    esp_codec_dev_write(codec, block, AMY_BLOCK_SIZE * AMY_NCHANS * sizeof(int16_t));

    if (t0 - last_report_us > 5000000) {
        const int64_t budget_us = (int64_t)AMY_BLOCK_SIZE * 1000000 / AMY_SAMPLE_RATE;
        ESP_LOGI(TAG, "render per block: avg %lld us, peak %lld us, budget %lld us (avg %.0f%%, peak %.0f%%)",
                 sum_us / blocks, peak_us, budget_us,
                 100.0 * sum_us / blocks / budget_us, 100.0 * peak_us / budget_us);
        peak_us = sum_us = blocks = 0;
        last_report_us = t0;
    }
}

static void play_song_forever(const amy_song_t *song)
{
    /* AMY splits rendering by osc index (first half on the helper core, second
     * half here), so each song sizes the osc pool to what its voices use. */
    amy_config_t cfg = base_config();
    cfg.max_oscs = song->max_oscs;
    amy_start(cfg);
    amy_event e = amy_default_event();
    e.volume = song->volume * AMY_VOLUME;
    amy_add_event(&e);
    song->setup();

    player_t player;
    for (;;) {
        ESP_LOGI(TAG, "Playing: %s", song->name);
        player_start(&player, song, amy_sysclock() + 100);
        bool playing = true;
        while (playing) {
            /* Queue score events inside the lookahead window; AMY applies each
             * one at its exact timestamp. */
            playing = player_feed(&player, amy_sysclock() + LOOKAHEAD_MS);
            render_block();
        }
    }
}

static void play_midi_file(const midi_playlist_entry_t *f)
{
    midi_song_t song;
    if (midi_file_load(f->start, f->end - f->start, &song) != 0) {
        ESP_LOGE(TAG, "%s: cannot parse MIDI file", f->name);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    ESP_LOGI(TAG, "Playing: %s (%u events, %lu s)", f->name, (unsigned)song.count,
             (unsigned long)(song.length_ms / 1000));
    for (int ch = 0; ch < 16; ch++) {
        if (!song.notes[ch]) continue;
        const uint8_t prog = song.first_program[ch] < 0 ? 0 : (uint8_t)song.first_program[ch];
        const char *chip = ch == 9 ? NULL : midi_gm_chip_voice(prog);
        if (chip) {
            ESP_LOGI(TAG, "  ch%-2d program %3d -> %s, %u notes, poly %u", ch + 1,
                     song.first_program[ch], chip, song.notes[ch], song.max_poly[ch]);
        } else {
            ESP_LOGI(TAG, "  ch%-2d program %3d -> AMY patch %3u, %u notes, poly %u", ch + 1,
                     song.first_program[ch], ch == 9 ? 258 : midi_gm_to_amy_patch(prog),
                     song.notes[ch], song.max_poly[ch]);
        }
    }

    amy_config_t cfg = base_config();
    midi_player_t player;
    const int oscs = midi_player_amy_start(&player, &song, &cfg, MIDI_VOLUME * AMY_VOLUME);
    ESP_LOGI(TAG, "  AMY osc pool %d, free heap %u (internal %u)", oscs,
             (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    midi_player_start(&player, amy_sysclock() + 100);
    bool playing = true;
    uint32_t tail_end = 0;
    while (playing || amy_sysclock() < tail_end) {
        if (playing) {
            playing = midi_player_feed(&player, amy_sysclock() + LOOKAHEAD_MS);
            if (!playing) tail_end = amy_sysclock() + LOOKAHEAD_MS + 2000;   /* let notes ring out */
        }
        render_block();
    }
    amy_stop();
    midi_file_free(&song);
}

static void audio_task(void *arg)
{
    /* AMY must be started from the task that calls amy_update() */
    if (!PLAY_MIDI_FILES || midi_playlist_count == 0) {
        play_song_forever(SONG());
    }
    for (;;) {
        for (size_t i = 0; i < midi_playlist_count; i++) {
            play_midi_file(&midi_playlist[i]);
        }
    }
}

void app_main(void)
{
    i2c_init();
    ESP_ERROR_CHECK(board_power_init(i2c_bus, on_power_off));  /* first: hold battery power on */
    speaker_amp_enable();
    i2s_init();
    codec_init();
    xTaskCreatePinnedToCore(audio_task, "audio", 12288, NULL, 10, NULL, 1);
}
