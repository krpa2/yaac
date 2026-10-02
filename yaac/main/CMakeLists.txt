# Gemini assistant: prompt and tools come straight from the Python POC (single source)
set(POC_DIR "${CMAKE_CURRENT_LIST_DIR}/../voice-coffee-assistant-poc-main")

idf_component_register(SRCS "main.c" "audio_io.c" "display.c" "scope_draw.c" "net_stream.c" "audio_post.c"
                            "gemini_live.c" "coffee_machine.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "${POC_DIR}/tools.json" "${POC_DIR}/system_prompt.md"
                       REQUIRES board_power esp_lcd esp_driver_spi esp_driver_i2s esp_driver_ledc
                                esp_driver_gpio esp_driver_i2c esp_timer esp_wifi esp_netif
                                esp_event nvs_flash lwip mbedtls esp_websocket_client)

# API key: read yaac/API_key.txt at configure time into build/.../gemini_key.h
# (the key never goes into the sources; API_key.txt is git-ignored)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(KEY_FILE "${CMAKE_CURRENT_LIST_DIR}/../API_key.txt")
    if(NOT EXISTS "${KEY_FILE}")
        message(FATAL_ERROR "Gemini API key missing: put it in ${KEY_FILE}")
    endif()
    file(READ "${KEY_FILE}" GEMINI_KEY)
    string(STRIP "${GEMINI_KEY}" GEMINI_KEY)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gemini_key.h"
         CONTENT "#pragma once\n/* Generated from yaac/API_key.txt - do not commit */\n#define GEMINI_API_KEY \"@GEMINI_KEY@\"\n" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KEY_FILE}")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
