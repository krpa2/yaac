# esp_websocket_client (vendored)

Copy of `espressif/esp_websocket_client` **1.8.0** from the ESP Component Registry
(esp-protocols commit 70bf122fc8bc74622dcf7e15233b5b2af9088df5, Apache-2.0, see LICENSE).
Being a project component, it replaces the registry version.

## Local changes (all marked `/* YAAC */` in `esp_websocket_client.c`)
YAAC's internal RAM is almost fully used by Wi-Fi and the ESP-SR AFE, so the client's
own allocations go to PSRAM:
- client task created with `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`
  and deleted with `vTaskDeleteWithCaps(NULL)` (stack in PSRAM; the task never writes flash)
- rx/tx buffers allocated with `heap_caps_malloc/calloc(..., MALLOC_CAP_SPIRAM)`
  (both the static and the dynamic-buffer code paths)

To update: copy the new upstream `esp_websocket_client.c`, `include/`, `CMakeLists.txt`,
`Kconfig` and re-apply the changes above.
