# Waveshare ESP32-S3-Touch-LCD-3.49 — Board Reference

Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49
Demo code: https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49 (cloned to `waveshare-demo-repo/`)

## Detected on this unit (esptool, 2026-09-30)
- ESP32-S3 (QFN56) rev v0.2, embedded PSRAM 8 MB (AP_3v3), 40 MHz crystal
- Flash: 16 MB quad, 3.3 V (manufacturer 0x20, device 0x4018)
- USB mode: native USB-Serial/JTAG (VID 303A / PID 1001) — shows up as COM15
- MAC: 28:84:85:92:c1:28

## Hardware
| Item | Part | Notes |
|---|---|---|
| Display | 3.49" IPS, 172×640, AXS15231B | QSPI, SPI3_HOST |
| Touch | AXS15231B (integrated) | I2C addr 0x3B |
| IMU | QMI8658 | I2C addr 0x6B |
| RTC | PCF85063 | I2C addr 0x51 |
| IO expander | TCA9554 | on I2C bus 47/48 |
| Audio out | ES8311 DAC | MX1.25 speaker header |
| Audio in | ES7210 ADC | dual mic array, echo cancellation |
| Storage | TF card | SDMMC 1-bit |
| Power | Li-ion charger, MX1.25 battery header | PWR + BOOT buttons |

## Pin map (from Waveshare example code)
| Function | GPIO |
|---|---|
| LCD CS | 9 |
| LCD PCLK | 10 |
| LCD DATA0..3 | 11, 12, 13, 14 |
| LCD RST | 21 |
| LCD backlight (PWM) | 8 |
| Touch I2C SDA / SCL | 17 / 18 |
| Main I2C SDA / SCL (RTC, IMU, codecs, TCA9554) | 47 / 48 |
| I2S MCLK / BCLK / WS | 7 / 15 / 46 |
| I2S DIN (mic) / DOUT (speaker) | 6 / 45 |
| SD CLK / CMD / D0 | 41 / 39 / 40 |
| Battery ADC | ADC1_CH3 (GPIO4) |
| PWR button sense | 16 |

TCA9554 expander pins used by the demos: EXIO1 (driven low in the ADC test), EXIO6 (power latch, held high in the battery/power test; setting it low powers off), EXIO7 (audio PA enable).

## Development
- Arduino: esp32 core ≥ 3.1.0, LVGL 8.3.11 or 9.3.0, SensorLib 0.3.1 (bundled in `waveshare-demo-repo/Arduino_Libraries`)
- ESP-IDF: examples in `waveshare-demo-repo/Examples/ESP-IDF` (01–11; 11 = FactoryProgram source)
- Factory partition table: nvs 0x6000, phy_init 0x1000, factory app 8 MB

## Local files
- `datasheets/` — board schematic, ESP32-S3 datasheet + TRM, AXS15231B, QMI8658, PCF85063, ES8311 datasheet + user guide
- `waveshare-demo-repo/Firmware/` — official prebuilt `FactoryProgram.bin` and `xiaozhi-v1.9.1.bin`
- `../firmware_backup/` — full flash dump of the firmware shipped on this unit
