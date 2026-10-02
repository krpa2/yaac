#pragma once

/*
 * Battery power latch + PWR button for the Waveshare ESP32-S3-Touch-LCD-3.49.
 *
 * Hardware (schematic "KEY&POWER"): the battery reaches the system rail through
 * P-MOSFET Q2. Pressing PWR turns Q2 on; the firmware must then drive SYS_EN
 * (TCA9554 EXIO6) high to keep it on after the button is released. SYS_OUT
 * (GPIO16, 0 = pressed) reads the button. USB power bypasses Q2 through D1, so
 * on USB the board stays powered whatever EXIO6 does.
 *
 * The component owns the board's only TCA9554 handle: creating a second handle
 * resets the expander's output register, which would drop the latch. Use
 * board_power_io_expander() for the other expander pins (e.g. EXIO7 speaker amp).
 */
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_POWER_LONG_PRESS_MS   1500

/* Called from the button task just before power is cut (e.g. mute audio). */
typedef void (*board_power_off_cb_t)(void);

/* Latch battery power on (EXIO6 high) and start watching the PWR button.
 * Call as early as possible in app_main: on battery the board only stays on
 * while PWR is held until this runs. A long press (BOARD_POWER_LONG_PRESS_MS)
 * powers off; the press that switched the board on is ignored until released. */
esp_err_t board_power_init(i2c_master_bus_handle_t i2c_bus, board_power_off_cb_t on_power_off);

/* Shared TCA9554 handle (valid after board_power_init) */
esp_io_expander_handle_t board_power_io_expander(void);

/* Switch off now: runs the callback, releases the latch and, if the board is
 * still running on USB power, enters deep sleep until PWR is pressed. */
void board_power_off(void);

#ifdef __cplusplus
}
#endif
