#pragma once

/* Simulated coffee machine controller + schema-driven validation of Gemini tool
 * calls: a C port of validate() and FakeMachine from
 * voice-coffee-assistant-poc-main/gemini_live_mic.py. Levels (short/long,
 * less/more...) are resolved into physical values (ml, milk seconds, degrees C)
 * here on the device, never by the model. */
#include "cJSON.h"

/* decls: the "functionDeclarations" array of tools.json (with x-range keys) */
void coffee_machine_init(const cJSON *decls);

/* Validate and run one function call; returns a new JSON object to send back
 * as the functionResponse "response" (caller frees it) */
cJSON *coffee_machine_handle(const char *name, const cJSON *args);
