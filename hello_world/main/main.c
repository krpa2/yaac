/*
 * Hello World for Waveshare ESP32-S3-Touch-LCD-3.49 (landscape, 640x172).
 *
 * The AXS15231B panel is natively 172x640 (portrait) and has no usable
 * hardware rotation over QSPI, so LVGL renders a 640x172 frame and the flush
 * callback rotates it 90 degrees in software before sending it to the panel
 * (same approach as Waveshare's 09_LVGL_V8_Test example).
 */
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
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "hello";

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

/* Native panel geometry (portrait) */
#define PANEL_W             172
#define PANEL_H             640
/* LVGL geometry (landscape) */
#define LV_W                PANEL_H
#define LV_H                PANEL_W

#define FRAME_PIXELS        (PANEL_W * PANEL_H)
#define DMA_ROWS            64                            /* panel rows per QSPI transfer */
#define DMA_BUF_PIXELS      (PANEL_W * DMA_ROWS)
#define DMA_CHUNKS          (PANEL_H / DMA_ROWS)          /* 640 / 64 = 10 */

#define LVGL_TICK_MS        5

static SemaphoreHandle_t flush_done_sem;
static uint16_t *dma_buf;       /* internal DMA-capable RAM */
static uint16_t *rotated_buf;   /* full portrait frame in PSRAM */

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},  /* sleep out */
    {0x29, (uint8_t[]){0x00}, 0, 100},  /* display on */
};

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(flush_done_sem, &woken);
    return woken == pdTRUE;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    const uint16_t *src = (const uint16_t *)color_map;

    /* Rotate the 640x172 landscape frame into the 172x640 portrait panel layout */
    uint32_t idx = 0;
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            rotated_buf[idx++] = src[LV_W * (LV_H - x - 1) + y];
        }
    }

    /* The panel needs the whole frame; send it in DMA-sized row bands */
    const uint16_t *map = rotated_buf;
    xSemaphoreGive(flush_done_sem);
    for (int i = 0; i < DMA_CHUNKS; i++) {
        xSemaphoreTake(flush_done_sem, portMAX_DELAY);
        memcpy(dma_buf, map, DMA_BUF_PIXELS * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(panel, 0, i * DMA_ROWS, PANEL_W, (i + 1) * DMA_ROWS, dma_buf);
        map += DMA_BUF_PIXELS;
    }
    xSemaphoreTake(flush_done_sem, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
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
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static esp_lcd_panel_handle_t panel_init(void)
{
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << PIN_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_conf));

    ESP_LOGI(TAG, "Initialize QSPI bus");
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
    esp_lcd_panel_handle_t panel = NULL;
    ESP_LOGI(TAG, "Install AXS15231B panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &panel_config, &panel));

    /* Manual reset sequence, as in the Waveshare demos */
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello World!");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

void app_main(void)
{
    flush_done_sem = xSemaphoreCreateBinary();
    assert(flush_done_sem);

    backlight_init();
    esp_lcd_panel_handle_t panel = panel_init();

    lv_init();
    dma_buf = heap_caps_malloc(DMA_BUF_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
    rotated_buf = heap_caps_malloc(FRAME_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    lv_color_t *draw_buf = heap_caps_malloc(FRAME_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(dma_buf && rotated_buf && draw_buf);

    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, draw_buf, NULL, FRAME_PIXELS);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LV_W;
    disp_drv.ver_res = LV_H;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.full_refresh = 1;  /* flush_cb rotates whole frames only */
    disp_drv.user_data = panel;
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    create_ui();
    ESP_LOGI(TAG, "Hello World! displayed");

    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < 5) delay_ms = 5;
        if (delay_ms > 500) delay_ms = 500;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
