#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"
#include "esp_log.h"
#include "display.h"

static const char *TAG = "display";

/* Pins (see docs/BOARD_REFERENCE.md) */
#define LCD_HOST            SPI3_HOST
#define PIN_LCD_CS          GPIO_NUM_9
#define PIN_LCD_PCLK        GPIO_NUM_10
#define PIN_LCD_DATA0       GPIO_NUM_11
#define PIN_LCD_DATA1       GPIO_NUM_12
#define PIN_LCD_DATA2       GPIO_NUM_13
#define PIN_LCD_DATA3       GPIO_NUM_14
#define PIN_LCD_RST         GPIO_NUM_21
#define PIN_LCD_BL          GPIO_NUM_8

#define DMA_ROWS            64                          /* panel rows per QSPI transfer */
#define DMA_BUF_PIXELS      (PANEL_W * DMA_ROWS)
#define DMA_CHUNKS          (PANEL_H / DMA_ROWS)

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t trans_done;
static uint16_t *framebuffer;   /* PSRAM */
static uint16_t *dma_buf;       /* internal, DMA-capable */

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},  /* sleep out */
    {0x29, (uint8_t[]){0x00}, 0, 100},  /* display on */
};

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(trans_done, &woken);
    return woken == pdTRUE;
}

static void backlight_init(void)
{
    /* Backlight is driven inverted: duty 0 = full brightness */
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 50 * 1000,
        .clk_cfg = LEDC_SLOW_CLK_RC_FAST,
    };
    ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_3,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void display_set_backlight(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 255 - level);    /* inverted drive */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

uint16_t *display_init(void)
{
    trans_done = xSemaphoreCreateBinary();
    framebuffer = heap_caps_calloc(PANEL_W * PANEL_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    dma_buf = heap_caps_malloc(DMA_BUF_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(trans_done && framebuffer && dma_buf);

    backlight_init();

    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << PIN_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_conf));

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_PCLK,
        .data0_io_num = PIN_LCD_DATA0,
        .data1_io_num = PIN_LCD_DATA1,
        .data2_io_num = PIN_LCD_DATA2,
        .data3_io_num = PIN_LCD_DATA3,
        .max_transfer_sz = DMA_BUF_PIXELS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    axs15231b_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &panel_config, &panel));

    /* Manual reset sequence, as in the Waveshare demos */
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI(TAG, "Panel ready (%dx%d landscape)", SCREEN_W, SCREEN_H);
    return framebuffer;
}

/* A band normally takes ~1 ms; never wait forever (a failed transfer would
 * otherwise freeze the UI for good) */
#define TRANS_TIMEOUT_MS    200

static bool wait_trans_done(void)
{
    if (xSemaphoreTake(trans_done, pdMS_TO_TICKS(TRANS_TIMEOUT_MS)) == pdTRUE) return true;
    static uint32_t timeouts;
    if (timeouts++ % 50 == 0) ESP_LOGW(TAG, "panel transfer timeout (%lu so far)", (unsigned long)timeouts);
    return false;
}

void display_present(void)
{
    /* The panel takes whole frames only; send them in DMA-sized row bands */
    const uint16_t *src = framebuffer;
    xSemaphoreGive(trans_done);
    for (int i = 0; i < DMA_CHUNKS; i++) {
        wait_trans_done();
        memcpy(dma_buf, src, DMA_BUF_PIXELS * sizeof(uint16_t));
        const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, i * DMA_ROWS, PANEL_W, (i + 1) * DMA_ROWS, dma_buf);
        if (err != ESP_OK) {
            static uint32_t failures;
            if (failures++ % 50 == 0) ESP_LOGW(TAG, "draw_bitmap failed: %s (%lu so far)", esp_err_to_name(err), (unsigned long)failures);
            xSemaphoreGive(trans_done);     /* no completion callback will come for this band */
        }
        src += DMA_BUF_PIXELS;
    }
    wait_trans_done();
}
