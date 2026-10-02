# AMY synthesizer (https://github.com/shorepine/amy), vendored from src/ — see VERSION.
# Only the portable engine + ESP32 platform layer (i2s.c) are built; desktop/web/pico
# front-ends (miniaudio, pyamy, amy-example, ...) are left out.
set(AMY_SRCS
    algorithms.c amy.c amy_midi.c api.c custom.c cv_trigger.c delay.c envelope.c
    examples.c filters.c i2s.c instrument.c interp_partials.c log2_exp2.c
    midi_mappings.c note_output.c oscillators.c parse.c patches.c pcm.c
    sequencer.c transfer.c)
list(TRANSFORM AMY_SRCS PREPEND "src/")

idf_component_register(SRCS ${AMY_SRCS}
                       INCLUDE_DIRS "src"
                       REQUIRES driver esp_timer freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-variable -Wno-unused-function
                       -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-format)
