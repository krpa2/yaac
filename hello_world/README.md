# Hello World — ESP32-S3-Touch-LCD-3.49 (landscape)

Shows "Hello World!" (Montserrat 48, white on black) centered on the 640×172 landscape screen.

The AXS15231B panel is natively 172×640 portrait. LVGL 8 renders a 640×172 frame and
`lvgl_flush_cb` rotates it 90° in software before sending it over QSPI in 10 bands of 64 rows.

## Build & flash (ESP-IDF 5.5.x)
    . C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
    idf.py build
    idf.py -p COM15 flash monitor

Dependencies (`main/idf_component.yml`): `espressif/esp_lcd_axs15231b`, `lvgl/lvgl ~8.4.0`.

To flip the text 180°, change the rotation line in `lvgl_flush_cb` to
`rotated_buf[idx++] = src[LV_W * x + (LV_W - y - 1)];`
