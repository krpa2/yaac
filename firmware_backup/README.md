# Demo firmware backup — ESP32-S3-Touch-LCD-3.49

Taken 2026-09-30 from the unit on COM15 (MAC 28:84:85:92:c1:28).

- `demo_firmware_full_16MB.bin` — full 16 MB flash image (0x0–0x1000000), checked with `esptool verify_flash` (digest matched)
- `demo_firmware_full_16MB.bin.sha256` — SHA-256 checksum
- `partition_table_0x8000.bin` / `partition_table.csv` — decoded partition table (nvs @0x9000, phy_init @0xF000, factory app 8 MB @0x10000)

## Restore
    esptool --chip esp32s3 --port COM15 --baud 921600 write_flash 0 demo_firmware_full_16MB.bin

If the port does not connect, hold BOOT, tap RESET/PWR, then release BOOT to enter download mode.
