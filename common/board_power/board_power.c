#include "board_power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "board_power";

#define PIN_PWR_KEY     GPIO_NUM_16                 /* SYS_OUT: 0 = PWR pressed */
#define EXIO_SYS_EN     IO_EXPANDER_PIN_NUM_6       /* 1 = keep battery power on */
#define POLL_MS         20

static esp_io_expander_handle_t s_io;
static board_power_off_cb_t s_off_cb;

esp_io_expander_handle_t board_power_io_expander(void)
{
    return s_io;
}

static bool pwr_pressed(void)
{
    return gpio_get_level(PIN_PWR_KEY) == 0;
}

void board_power_off(void)
{
    ESP_LOGW(TAG, "Powering off");
    if (s_off_cb) s_off_cb();

    /* Release the latch. On battery the board dies once PWR is let go (the
     * button itself still holds Q2 on while pressed). */
    esp_io_expander_set_level(s_io, EXIO_SYS_EN, 0);
    while (pwr_pressed()) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Still alive: we're on USB power, which bypasses the latch. Sleep instead
     * and wake (= reboot) on the next PWR press. */
    ESP_LOGW(TAG, "Still powered (USB) - deep sleep until PWR is pressed");
    rtc_gpio_pullup_en(PIN_PWR_KEY);
    rtc_gpio_pulldown_dis(PIN_PWR_KEY);
    esp_sleep_enable_ext0_wakeup(PIN_PWR_KEY, 0);
    esp_deep_sleep_start();
}

static void button_task(void *arg)
{
    /* Ignore the press that switched the board on until it is released */
    bool armed = !pwr_pressed();
    uint32_t held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (!pwr_pressed()) {
            armed = true;
            held_ms = 0;
            continue;
        }
        if (!armed) continue;
        held_ms += POLL_MS;
        if (held_ms >= BOARD_POWER_LONG_PRESS_MS) {
            board_power_off();      /* does not return */
        }
    }
}

esp_err_t board_power_init(i2c_master_bus_handle_t i2c_bus, board_power_off_cb_t on_power_off)
{
    s_off_cb = on_power_off;

    esp_err_t err = esp_io_expander_new_i2c_tca9554(i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_io);
    if (err != ESP_OK) return err;
    err = esp_io_expander_set_dir(s_io, EXIO_SYS_EN, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) err = esp_io_expander_set_level(s_io, EXIO_SYS_EN, 1);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Power latched on");

    if (rtc_gpio_is_valid_gpio(PIN_PWR_KEY)) {
        rtc_gpio_deinit(PIN_PWR_KEY);   /* back to a normal GPIO after deep-sleep wake */
    }
    const gpio_config_t key = {
        .pin_bit_mask = 1ULL << PIN_PWR_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    err = gpio_config(&key);
    if (err != ESP_OK) return err;

    if (xTaskCreate(button_task, "pwr_key", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
