/*
 * Plays "The Imperial March" on a synth piano through the onboard ES8311
 * codec and speaker of the Waveshare ESP32-S3-Touch-LCD-3.49.
 *
 * Audio path: piano_synth (44.1 kHz, 16-bit stereo) -> I2S0 -> ES8311 DAC
 *             -> speaker amplifier (enabled via TCA9554 EXIO7).
 */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander_tca9554.h"
#include "piano_synth.h"
#include "imperial_march.h"
#include "sequencer.h"

static const char *TAG = "sound";

/* Pins (see docs/BOARD_REFERENCE.md) */
#define PIN_I2C_SDA     GPIO_NUM_47
#define PIN_I2C_SCL     GPIO_NUM_48
#define PIN_I2S_MCLK    GPIO_NUM_7
#define PIN_I2S_BCLK    GPIO_NUM_15
#define PIN_I2S_WS      GPIO_NUM_46
#define PIN_I2S_DOUT    GPIO_NUM_45
#define EXIO_SPK_AMP    IO_EXPANDER_PIN_NUM_7

#define OUTPUT_VOLUME   100     /* 0..100 */
#define BLOCK_FRAMES    256

static i2c_master_bus_handle_t i2c_bus;
static i2s_chan_handle_t i2s_tx;

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
    esp_io_expander_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(io, EXIO_SPK_AMP, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io, EXIO_SPK_AMP, 1));
}

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SYNTH_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
}

static esp_codec_dev_handle_t codec_init(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,           /* amplifier is on the IO expander instead */
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
    assert(dev);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SYNTH_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(dev, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_out_vol(dev, OUTPUT_VOLUME);
    return dev;
}

static void play_score(esp_codec_dev_handle_t dev)
{
    static int16_t block[BLOCK_FRAMES * 2];
    static sequencer_t seq;
    sequencer_start(&seq, imperial_march_tracks, imperial_march_track_count, imperial_march_bpm);
    bool playing = true;
    while (playing) {
        playing = sequencer_render(&seq, block, BLOCK_FRAMES);
        esp_codec_dev_write(dev, block, sizeof(block));
    }
}

void app_main(void)
{
    i2c_init();
    speaker_amp_enable();
    i2s_init();
    esp_codec_dev_handle_t dev = codec_init();
    piano_synth_init();

    while (1) {
        ESP_LOGI(TAG, "Playing: The Imperial March");
        play_score(dev);
    }
}
