#include "freertos/FreeRTOS.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "board_power.h"
#include "audio_io.h"

static const char *TAG = "audio_io";

/* Pins (see docs/BOARD_REFERENCE.md) */
#define PIN_I2S_MCLK    GPIO_NUM_7
#define PIN_I2S_BCLK    GPIO_NUM_15
#define PIN_I2S_WS      GPIO_NUM_46
#define PIN_I2S_DIN     GPIO_NUM_6
#define PIN_I2S_DOUT    GPIO_NUM_45
#define EXIO_SPK_AMP    IO_EXPANDER_PIN_NUM_7

#define MIC_GAIN_DB     30.0f   /* ES7210 PGA */
#define SPK_VOLUME      100     /* ES8311 0..100 */
#define OUT_CHUNK       256     /* mono samples per write */

static esp_codec_dev_handle_t mic_dev, spk_dev;

void audio_io_init(i2c_master_bus_handle_t i2c_bus)
{
    /* One I2S port, both directions, identical TDM layout (shared clocks) */
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;     /* silence when nothing is played */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, &rx));
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(tx, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm_cfg));

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    /* ES7210: selecting 3+ inputs enables its TDM output */
    audio_codec_i2c_cfg_t in_i2c = {.port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus};
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&in_i2c),
        .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_codec_new(&es7210_cfg),
        .data_if = data_if,
    };
    mic_dev = esp_codec_dev_new(&in_cfg);

    audio_codec_i2c_cfg_t out_i2c = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus};
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&out_i2c),
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,               /* amp enable is on the IO expander */
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_codec_new(&es8311_cfg),
        .data_if = data_if,
    };
    spk_dev = esp_codec_dev_new(&out_cfg);
    assert(mic_dev && spk_dev);

    /* Same layout both ways: 4 x 16-bit slots at 16 kHz */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = AUDIO_IN_CHANNELS,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(mic_dev, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_codec_dev_open(spk_dev, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_in_gain(mic_dev, MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(spk_dev, SPK_VOLUME);

    esp_io_expander_handle_t io = board_power_io_expander();
    esp_io_expander_set_dir(io, EXIO_SPK_AMP, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io, EXIO_SPK_AMP, 1);
    ESP_LOGI(TAG, "I2S0 TDM full duplex %d Hz; in %s, mic gain %.0f dB; speaker vol %d",
             AUDIO_SAMPLE_RATE, AUDIO_IN_FORMAT, MIC_GAIN_DB, SPK_VOLUME);
}

int audio_read(int16_t *buf, size_t frames)
{
    return esp_codec_dev_read(mic_dev, buf, frames * AUDIO_IN_CHANNELS * sizeof(int16_t));
}

void audio_play(const int16_t *mono, size_t n)
{
    /* The ES8311 picks its L/R samples out of the 4-slot TDM frame; write the
     * sample into every slot so it plays whichever slots it reads. */
    static int16_t frame_buf[OUT_CHUNK * AUDIO_IN_CHANNELS];
    while (n) {
        const size_t chunk = n < OUT_CHUNK ? n : OUT_CHUNK;
        for (size_t i = 0; i < chunk; i++) {
            for (int c = 0; c < AUDIO_IN_CHANNELS; c++) frame_buf[i * AUDIO_IN_CHANNELS + c] = mono[i];
        }
        esp_codec_dev_write(spk_dev, frame_buf, chunk * AUDIO_IN_CHANNELS * sizeof(int16_t));
        mono += chunk;
        n -= chunk;
    }
}

void audio_mute(void)
{
    esp_io_expander_set_level(board_power_io_expander(), EXIO_SPK_AMP, 0);
    if (spk_dev) esp_codec_dev_set_out_vol(spk_dev, 0);
}
