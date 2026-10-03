#pragma once

/* Gemini Live API client (raw protocol, same as voice-coffee-assistant-poc-main/
 * gemini_live_mic.py): TLS WebSocket to generativelanguage.googleapis.com,
 * "setup" message with the system prompt and tools.json, 16 kHz PCM uplink in
 * base64 32 ms chunks, transcriptions + toolCall handling on the downlink.
 *
 * The session is always open: it connects as soon as Wi-Fi is up, reconnects with
 * backoff (1..30 s) after any close, and on the server's goAway reconnects between
 * turns. Session resumption (sessionResumptionUpdate handles) keeps the conversation
 * context across reconnects; context window compression keeps long sessions going.
 * Replies are spoken on the speaker (playback.c, 24 -> 16 kHz) and their transcripts
 * printed to the log; an "interrupted" message from the server flushes the speaker. */
#include <stdbool.h>
#include <stdint.h>

#define GEMINI_MODEL            "gemini-2.5-flash-native-audio-preview-09-2025"

void gemini_init(void);

/* Wake word detected: start a user turn (audio is held while (re)connecting) */
void gemini_turn_begin(void);

/* Audio after the wake word (16 kHz mono); never blocks, drops when the queue is full */
void gemini_push_audio(const int16_t *pcm, int n);

/* Listening ended: tell Gemini the user's audio stream is over */
void gemini_turn_end(void);

bool gemini_session_ready(void);
