#pragma once

/* Wi-Fi station + UDP broadcast voice stream (local network only).
 *
 * Packet (little endian), sent to the subnet broadcast address (e.g.
 * 192.168.0.255 for a /24, derived from the board's IP and netmask) on
 * NET_STREAM_PORT. Routers do not forward broadcasts, so it stays local.
 *   0  char[4]  magic "YAAC"
 *   4  u8       version (1)
 *   5  u8       flags: bit0 = first packet of a session, bit1 = last (end marker),
 *                      bit2 = EVENT packet (see below)
 *   6  u16      audio: sample count (0..NET_STREAM_MAX_SAMPLES); event: payload bytes
 *   8  u32      session id (increments per wake word)
 *  12  u32      audio: packet sequence number within the session; event: 0
 *  16  u32      sample rate (Hz)
 *  20  audio:   i16[] mono PCM samples
 *      event:   u8 event type (net_event_t), then UTF-8 text (not NUL terminated)
 * Event packets can arrive at any time, also between audio sessions; receivers
 * that only want audio must skip packets with the EVENT flag.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_STREAM_PORT         5577
#define NET_STREAM_MAX_SAMPLES  512

#define NET_FLAG_START          0x01
#define NET_FLAG_END            0x02
#define NET_FLAG_EVENT          0x04

typedef enum {
    NET_EV_WAKE = 1,            /* wake word detected */
    NET_EV_GEMINI_STREAM = 2,   /* the turn's audio started flowing to Gemini */
    NET_EV_TOOL_CALL = 3,       /* Gemini called a machine function: "name(args) -> status" */
    NET_EV_REPLY = 4,           /* Gemini finished a spoken reply: its transcript */
    /* End of a listening turn: exactly one of these, also the end of the Gemini stream */
    NET_EV_NO_COMMAND = 5,      /* wake word, but no command started within LISTEN_START_MS */
    NET_EV_STREAM_STOP = 6,     /* command ended (silence): streaming to Gemini stopped */
    NET_EV_TIME_LIMIT = 7,      /* still talking at LISTEN_TIMEOUT_MS: streaming stopped */
} net_event_t;

/* Start Wi-Fi (non-blocking: connects and reconnects in the background) */
void net_stream_init(void);

bool net_stream_connected(void);
const char *net_stream_ip(void);        /* "" until connected */

/* Called from the audio path; never blocks (packets are dropped if the queue is full) */
void net_stream_begin(uint32_t sample_rate);
void net_stream_push(const int16_t *samples, size_t n);
void net_stream_end(void);
bool net_stream_active(void);

/* Broadcast an event (any task, printf-style text, truncated to fit one packet) */
void net_stream_event(net_event_t type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
