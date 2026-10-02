#pragma once

/* Wi-Fi station + UDP broadcast voice stream (local network only).
 *
 * Packet (little endian), sent to the subnet broadcast address (e.g.
 * 192.168.0.255 for a /24, derived from the board's IP and netmask) on
 * NET_STREAM_PORT. Routers do not forward broadcasts, so it stays local.
 *   0  char[4]  magic "YAAC"
 *   4  u8       version (1)
 *   5  u8       flags: bit0 = first packet of a session, bit1 = last (end marker)
 *   6  u16      sample count in this packet (0..NET_STREAM_MAX_SAMPLES)
 *   8  u32      session id (increments per wake word)
 *  12  u32      packet sequence number within the session
 *  16  u32      sample rate (Hz)
 *  20  i16[]    mono PCM samples
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_STREAM_PORT         5577
#define NET_STREAM_MAX_SAMPLES  512

#define NET_FLAG_START          0x01
#define NET_FLAG_END            0x02

/* Start Wi-Fi (non-blocking: connects and reconnects in the background) */
void net_stream_init(void);

bool net_stream_connected(void);
const char *net_stream_ip(void);        /* "" until connected */

/* Called from the audio path; never blocks (packets are dropped if the queue is full) */
void net_stream_begin(uint32_t sample_rate);
void net_stream_push(const int16_t *samples, size_t n);
void net_stream_end(void);
bool net_stream_active(void);
