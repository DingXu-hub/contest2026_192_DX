/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * link.h - reliable framed link over the USB serial console.
 *
 * Why this exists: the CH340 link to the PC is *lossy for short writes*
 * (a bare "!status\n" often vanishes; a burst may lose its first byte) but
 * carries large blocks fine.  Everything above this layer therefore talks
 * in frames that are always padded, CRC-checked and acknowledged, so an
 * application never sees a dropped or truncated message.
 *
 * Framing (little endian):
 *   AA 55 | TYPE | CH | SEQ | LEN(2) | CRC16(2) | PAYLOAD(LEN) | PAD(0x0A * N)
 * CRC16/CCITT-FALSE covers TYPE..PAYLOAD.  Receivers resync on AA 55 and
 * ignore stray 0x0A/0x00 bytes between frames (the padding).
 *
 * Channels multiplex the same wire:
 *   CH_CTRL  : ping / time sync / info
 *   CH_TEXT  : the existing human AI protocol ("?question", "!status",
 *              "@LLMREQ ..."), so the legacy text mode keeps working
 *   CH_HTTP  : request/response proxy through the PC gateway
 *   CH_FILE  : file transfer (GPX / firmware / screenshots)
 *   CH_IP    : raw IPv4 datagrams (SLIP-style) for real sockets later
 */

#ifndef __HUANGSHAN_LINK_H
#define __HUANGSHAN_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LINK_MAGIC0   0xAA
#define LINK_MAGIC1   0x55

/* frame types */
#define LINK_T_CTRL   0
#define LINK_T_TEXT   1
#define LINK_T_DATA   2
#define LINK_T_ACK    3

/* channels */
#define LINK_CH_CTRL  0
#define LINK_CH_TEXT  1
#define LINK_CH_HTTP  2
#define LINK_CH_FILE  3
#define LINK_CH_IP    4

/* control sub-commands (CH_CTRL payload[0]) */
#define LINK_C_PING     0x01   /* reply: PONG + uptime ms                     */
#define LINK_C_PONG     0x82
#define LINK_C_SET_TIME 0x02   /* payload: be32 sec + be32 usec + be16 tz minutes
                                 * (11 bytes; a 9-byte legacy payload means
                                 *  "no timezone known") */
#define LINK_C_TIME_ACK 0x83
#define LINK_C_INFO     0x03   /* reply: PONG-like status string              */
#define LINK_C_WHOAMI   0x04   /* gateway -> device hello                     */

#define LINK_PAYLOAD_MAX 1024
#define LINK_PAD_BYTES   32     /* trailing 0x0A: defeats the short-write loss */

/* register the consumer of CH_TEXT lines (the AI agent) */
void link_set_text_sink(void (*sink)(const char *line));

/* register the consumer of HTTP proxy replies (shown as an AI card) */
void link_set_http_sink(void (*sink)(const char *text));

/* register a periodic callback (AI agent notify timeout + proactive engine) */
void link_set_tick(void (*tick)(void));

/* start the link (creates its own task; owns /dev/console input) */
int  link_start(void);
int  link_task(int argc, char *argv[]);

/* send one frame; payload <= LINK_PAYLOAD_MAX.  Returns 0 on success. */
int  link_send(uint8_t type, uint8_t ch, const void *payload, size_t len);

/* convenience: send a NUL-terminated string on CH_TEXT (no newline added) */
int  link_send_text(const char *s);

/* send a control request that expects a reply (fire and forget, padded) */
int  link_send_ctrl(uint8_t sub, const void *extra, size_t extra_len);

/* ask the gateway for a URL; the reply arrives on CH_HTTP and is printed.
 * Returns 0 if the request was queued. */
int  link_http_get(const char *url);

/* true once a gateway has been heard from (frames received + acked) */
bool link_gateway_present(void);
void link_print_stats(void);

#endif /* __HUANGSHAN_LINK_H */
