/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * link.c - reliable framed link over the USB serial console (see link.h).
 *
 * The link task is the ONLY reader of /dev/console.  Bytes that are not
 * part of a valid frame are handed to the legacy text path (the AI agent's
 * line parser), so the old "@LLMREQ ..." / "!status" workflow keeps
 * working whether or not the PC gateway is running.
 *
 * TX:  link_send() pads every frame with 0x0A bytes and waits for an ACK
 *      (matched by sequence number), retrying a few times.  That is the
 *      fix for the CH340 behaviour where short writes get dropped.
 * RX:  a resynchronising state machine; the framer reports whether a byte
 *      belonged to a frame so that non-frame bytes alone feed the legacy
 *      line parser (no double delivery).
 */

#include <nuttx/config.h>

#include "link.h"
#include "app_diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>

#define LINK_DEV         "/dev/console"
#define LINK_RETRIES     3
#define LINK_ACK_WAIT_MS 200
#define LINK_QLEN        4
#define LINK_POLL_MS     20

static int  g_fd = -1;
static volatile bool g_up;
static void (*g_text_sink)(const char *line);
static void (*g_http_sink)(const char *text);

/* ---------------- helpers ---------------- */

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF) - matches tools/gateway.py */
static uint16_t crc16(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    size_t   i;
    int      b;

    for (i = 0; i < n; i++)
    {
        crc ^= (uint16_t)d[i] << 8;
        for (b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

static void wire_write(const void *buf, size_t len)
{
    ssize_t n;
    if (g_fd < 0 || len == 0)
        return;
    n = write(g_fd, buf, len);
    (void)n;
}

/* ---------------- TX ---------------- */

typedef struct {
    uint8_t type;
    uint8_t ch;
    uint8_t seq;
    uint16_t len;
    uint8_t payload[LINK_PAYLOAD_MAX];
} tx_slot_t;

static tx_slot_t       g_tx[LINK_QLEN];
static int             g_tx_head;
static int             g_tx_tail;
static pthread_mutex_t g_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         g_seq_counter;
static volatile int    g_ack_seq = -1;

static void send_frame_raw(uint8_t type, uint8_t ch, uint8_t seq,
                           const uint8_t *payload, uint16_t len)
{
    static const uint8_t pad[LINK_PAD_BYTES] = {
        0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
        0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
        0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
        0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a
    };
    uint8_t  head[7];
    uint8_t  cb[2];
    uint8_t  crcbuf[5 + LINK_PAYLOAD_MAX];
    uint16_t crc;

    head[0] = LINK_MAGIC0;
    head[1] = LINK_MAGIC1;
    head[2] = type;
    head[3] = ch;
    head[4] = seq;
    head[5] = (uint8_t)(len & 0xff);
    head[6] = (uint8_t)((len >> 8) & 0xff);

    crcbuf[0] = type;
    crcbuf[1] = ch;
    crcbuf[2] = seq;
    crcbuf[3] = head[5];
    crcbuf[4] = head[6];
    if (len)
        memcpy(crcbuf + 5, payload, len);
    crc = crc16(crcbuf, 5 + len);
    cb[0] = (uint8_t)(crc & 0xff);
    cb[1] = (uint8_t)((crc >> 8) & 0xff);

    /* the CH340 eats the first byte of a burst after an idle gap; make
     * that byte a filler so the magic always survives */
    {
        static const uint8_t lead[2] = { 0x00, 0x00 };
        wire_write(lead, sizeof(lead));
    }
    wire_write(head, sizeof(head));
    wire_write(cb, sizeof(cb));
    if (len)
        wire_write(payload, len);
    wire_write(pad, sizeof(pad));
}

int link_send(uint8_t type, uint8_t ch, const void *payload, size_t len)
{
    if (g_fd < 0 || len > LINK_PAYLOAD_MAX)
        return -1;

    if (type == LINK_T_ACK || !g_up)
    {
        /* ACKs are never queued (and never acked), and while no gateway is
         * listening we keep the legacy raw-text behaviour */
        if (type != LINK_T_ACK)
            return -1;
        send_frame_raw(type, ch, g_seq_counter++, NULL, 0);
        return 0;
    }

    pthread_mutex_lock(&g_tx_lock);
    if (((g_tx_head + 1) % LINK_QLEN) == g_tx_tail)
    {
        pthread_mutex_unlock(&g_tx_lock);
        return -1;                        /* queue full */
    }
    {
        tx_slot_t *s = &g_tx[g_tx_head];
        s->type = type;
        s->ch = ch;
        s->seq = g_seq_counter++;
        s->len = (uint16_t)len;
        if (len)
            memcpy(s->payload, payload, len);
        g_tx_head = (g_tx_head + 1) % LINK_QLEN;
    }
    pthread_mutex_unlock(&g_tx_lock);
    return 0;
}

int link_send_text(const char *s)
{
    if (!s)
        return -1;
    if (!g_up)
    {
        printf("%s\n", s);                /* legacy: raw line */
        return 0;
    }
    return link_send(LINK_T_TEXT, LINK_CH_TEXT, s, strlen(s));
}

int link_send_ctrl(uint8_t sub, const void *extra, size_t extra_len)
{
    uint8_t buf[64];

    if (!g_up || 1 + extra_len > sizeof(buf))
        return -1;
    buf[0] = sub;
    if (extra_len)
        memcpy(buf + 1, extra, extra_len);
    return link_send(LINK_T_CTRL, LINK_CH_CTRL, buf, 1 + extra_len);
}

int link_http_get(const char *url)
{
    char req[300];

    if (!g_up || !url)
        return -1;
    snprintf(req, sizeof(req), "GET %s", url);
    return link_send(LINK_T_DATA, LINK_CH_HTTP, req, strlen(req));
}

/* ---------------- RX ---------------- */

typedef enum {
    RX_MAGIC0 = 0, RX_MAGIC1, RX_TYPE, RX_CH, RX_SEQ,
    RX_LEN0, RX_LEN1, RX_CRC0, RX_CRC1, RX_PAYLOAD
} rx_state_t;

static rx_state_t g_rx;
static uint8_t    g_rx_type, g_rx_ch, g_rx_seq;
static uint16_t   g_rx_len, g_rx_crc, g_rx_got;
static uint8_t    g_rx_payload[LINK_PAYLOAD_MAX];
static uint32_t   g_rx_frames, g_rx_bad;
static uint32_t   g_rx_bytes;

static void handle_frame(uint8_t type, uint8_t ch, uint8_t seq,
                         const uint8_t *pl, uint16_t len)
{
    switch (type)
    {
    case LINK_T_ACK:
        g_ack_seq = seq;
        break;

    case LINK_T_CTRL:
        if (len >= 1)
        {
            if (pl[0] == LINK_C_PING)
            {
                uint8_t reply[5];
                uint32_t up = now_ms();
                reply[0] = LINK_C_PONG;
                reply[1] = (uint8_t)(up >> 24);
                reply[2] = (uint8_t)(up >> 16);
                reply[3] = (uint8_t)(up >> 8);
                reply[4] = (uint8_t)(up);
                if (!g_up)
                {
                    g_up = true;
                    printf("[Link] gateway online\n");
                }
                link_send(LINK_T_CTRL, LINK_CH_CTRL, reply, sizeof(reply));
            }
            else if (pl[0] == LINK_C_WHOAMI)
            {
                if (!g_up)
                {
                    g_up = true;
                    printf("[Link] gateway online (hello)\n");
                }
            }
            else if (pl[0] == LINK_C_SET_TIME && len >= 9)
            {
                struct timeval tv;
                tv.tv_sec = ((uint32_t)pl[1] << 24) | ((uint32_t)pl[2] << 16) |
                            ((uint32_t)pl[3] << 8) | pl[4];
                tv.tv_usec = ((uint32_t)pl[5] << 24) | ((uint32_t)pl[6] << 16) |
                             ((uint32_t)pl[7] << 8) | pl[8];

                /* optional timezone: the PC sends its UTC offset in minutes
                 * (e.g. +480 for UTC+8).  Without it the RTC-free watch
                 * would display UTC.  POSIX TZ wants the offset with the
                 * opposite sign ("CST-8" == UTC+8). */
                if (len >= 11)
                {
                    int16_t off = (int16_t)(((uint16_t)pl[9] << 8) | pl[10]);
                    char tz[16];
                    int hh = off / 60;
                    int mm = off % 60;
                    int sign = (hh < 0 || mm < 0) ? -1 : 1;
                    int ah = hh < 0 ? -hh : hh;
                    int am = mm < 0 ? -mm : mm;

                    snprintf(tz, sizeof(tz), "UTC%s%d:%02d",
                             sign > 0 ? "-" : "+", ah, am);
                    setenv("TZ", tz, 1);
                    tzset();
                }

                if (settimeofday(&tv, NULL) == 0)
                {
                    time_t lt = tv.tv_sec;
                    struct tm tmv;
                    char buf[32];
                    localtime_r(&lt, &tmv);
                    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
                    printf("[Link] clock set to %s (tz=%s)\n", buf,
                           getenv("TZ") ? getenv("TZ") : "(unset)");
                }
                else
                    printf("[Link] settimeofday failed errno=%d\n", errno);
                if (g_up)
                    link_send_ctrl(LINK_C_TIME_ACK, NULL, 0);
            }
        }
        break;

    case LINK_T_TEXT:
    {
        char line[LINK_PAYLOAD_MAX + 1];
        memcpy(line, pl, len);
        line[len] = '\0';
        if (g_text_sink)
            g_text_sink(line);
        break;
    }

    case LINK_T_DATA:
        if (ch == LINK_CH_HTTP)
        {
            char body[LINK_PAYLOAD_MAX + 1];
            memcpy(body, pl, len);
            body[len] = '\0';
            printf("[HTTP] %s\n", body);
            if (g_http_sink)
                g_http_sink(body);
        }
        else if (ch == LINK_CH_FILE)
        {
            printf("[Link] file chunk %u bytes\n", (unsigned)len);
        }
        break;

    default:
        break;
    }
}

/* returns true when the byte was consumed by the framer */
static bool rx_byte(uint8_t b)
{
    switch (g_rx)
    {
    case RX_MAGIC0:
        if (b == LINK_MAGIC0)
        {
#if APP_DIAG_VERBOSE
            printf("[LinkRX] magic0 seen\n");
#endif
            g_rx = RX_MAGIC1;
            return true;
        }
        return false;

    case RX_MAGIC1:
        if (b == LINK_MAGIC1)
            g_rx = RX_TYPE;
        else
            g_rx = (b == LINK_MAGIC0) ? RX_MAGIC1 : RX_MAGIC0;
        return true;

    case RX_TYPE: g_rx_type = b; g_rx = RX_CH;    return true;
    case RX_CH:   g_rx_ch = b;   g_rx = RX_SEQ;   return true;
    case RX_SEQ:  g_rx_seq = b;  g_rx = RX_LEN0;  return true;
    case RX_LEN0: g_rx_len = b;  g_rx = RX_LEN1;  return true;
    case RX_LEN1:
        g_rx_len |= (uint16_t)b << 8;
        g_rx = (g_rx_len > LINK_PAYLOAD_MAX) ? RX_MAGIC0 : RX_CRC0;
        return true;
    case RX_CRC0: g_rx_crc = b; g_rx = RX_CRC1; return true;
    case RX_CRC1:
        g_rx_crc |= (uint16_t)b << 8;
        g_rx_got = 0;
        if (g_rx_len == 0)
        {
            uint8_t t[5] = { g_rx_type, g_rx_ch, g_rx_seq, 0, 0 };
            g_rx = RX_MAGIC0;
            if (crc16(t, 5) == g_rx_crc)
                handle_frame(g_rx_type, g_rx_ch, g_rx_seq, NULL, 0);
            else
                g_rx_bad++;
        }
        else
            g_rx = RX_PAYLOAD;
#if APP_DIAG_VERBOSE
        printf("[LinkRX] hdr type=%u ch=%u seq=%u len=%u crc=0x%04x\n",
               g_rx_type, g_rx_ch, g_rx_seq, g_rx_len, g_rx_crc);
#endif
        return true;

    case RX_PAYLOAD:
        g_rx_payload[g_rx_got++] = b;
        if (g_rx_got == g_rx_len)
        {
            uint8_t t[5 + LINK_PAYLOAD_MAX];
            t[0] = g_rx_type;
            t[1] = g_rx_ch;
            t[2] = g_rx_seq;
            t[3] = (uint8_t)(g_rx_len & 0xff);
            t[4] = (uint8_t)((g_rx_len >> 8) & 0xff);
            memcpy(t + 5, g_rx_payload, g_rx_len);
            g_rx = RX_MAGIC0;
#if APP_DIAG_VERBOSE
            printf("[LinkRX] payload %u bytes crc=%s\n", g_rx_len,
                   crc16(t, 5 + g_rx_len) == g_rx_crc ? "OK" : "BAD");
#endif
            if (crc16(t, 5 + g_rx_len) == g_rx_crc)
            {
                g_rx_frames++;
                if (g_rx_type != LINK_T_ACK)      /* ack every good frame */
                    send_frame_raw(LINK_T_ACK, g_rx_ch, g_rx_seq, NULL, 0);
                handle_frame(g_rx_type, g_rx_ch, g_rx_seq, g_rx_payload,
                             g_rx_len);
            }
            else
                g_rx_bad++;
        }
        return true;

    default:
        g_rx = RX_MAGIC0;
        return false;
    }
}

/* ---------------- legacy line path ---------------- */

static char   g_line[256];
static size_t g_line_len;

static void legacy_byte(uint8_t b)
{
    if (b == '\n' || b == '\r' || b == 0x0a)
    {
        if (g_line_len)
        {
            g_line[g_line_len] = '\0';
            if (g_text_sink)
                g_text_sink(g_line);
            g_line_len = 0;
        }
    }
    else if (b != 0x00 && g_line_len < sizeof(g_line) - 1)
        g_line[g_line_len++] = (char)b;
}

/* ---------------- TX service ---------------- */

static void tx_service(void)
{
    static uint32_t last_try_ms;
    static int      tries;

    pthread_mutex_lock(&g_tx_lock);
    if (g_tx_tail == g_tx_head)
    {
        tries = 0;
        pthread_mutex_unlock(&g_tx_lock);
        return;
    }

    {
        tx_slot_t *s = &g_tx[g_tx_tail];

        if (tries > 0 && g_ack_seq == (int)s->seq)
        {
            g_tx_tail = (g_tx_tail + 1) % LINK_QLEN;
            tries = 0;
        }
        else if (tries == 0 || (now_ms() - last_try_ms) >= LINK_ACK_WAIT_MS)
        {
            last_try_ms = now_ms();
            tries++;
            send_frame_raw(s->type, s->ch, s->seq, s->payload, s->len);
            if (tries > LINK_RETRIES)
            {
                printf("[Link] drop unacked type=%u ch=%u seq=%u\n",
                       s->type, s->ch, s->seq);
                if (s->type == LINK_T_CTRL || s->type == LINK_T_TEXT)
                    g_up = false;         /* gateway probably gone */
                g_tx_tail = (g_tx_tail + 1) % LINK_QLEN;
                tries = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_tx_lock);
}

/* ---------------- task ---------------- */

static void (*g_tick)(void);

int link_task(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* O_NONBLOCK + poll (the pattern the AI CLI used successfully): with a
     * blocking read the NuttX console driver returns after the first byte
     * and leaves the RX interrupt disabled between calls, so bytes that
     * arrive while we are not inside read() are dropped (measured: only
     * 2..14 of 63 sent characters arrived, scattered). */
    g_fd = open(LINK_DEV, O_RDWR | O_NONBLOCK);
    if (g_fd < 0)
    {
        printf("[Link] open %s failed errno=%d\n", LINK_DEV, errno);
        return -1;
    }

    printf("[Link] framed link ready\n");

    for (;;)
    {
        struct pollfd pfd;
        pfd.fd = g_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, LINK_POLL_MS) > 0 && (pfd.revents & POLLIN))
        {
            for (;;)
            {
                uint8_t buf[256];
                ssize_t n = read(g_fd, buf, sizeof(buf));
                ssize_t i;

                if (n <= 0)
                    break;

#if APP_DIAG_VERBOSE
                {
                    static uint32_t last_ms;
                    g_rx_bytes += (uint32_t)n;
                    if (now_ms() - last_ms >= 2000)
                    {
                        last_ms = now_ms();
                        printf("[LinkRX] rxbytes=%lu frames=%lu bad=%lu\n",
                               (unsigned long)g_rx_bytes,
                               (unsigned long)g_rx_frames,
                               (unsigned long)g_rx_bad);
                    }
                }
#endif
                for (i = 0; i < n; i++)
                {
                    if (!rx_byte(buf[i]))
                        legacy_byte(buf[i]);
                }
            }
        }

        tx_service();
        if (g_tick)
            g_tick();
    }
    return 0;
}

/* ---------------- public helpers ---------------- */

bool link_gateway_present(void)
{
    return g_up;
}

void link_print_stats(void)
{
    printf("[Link] gateway=%s rx_frames=%lu rx_bad=%lu txq=%d\n",
           g_up ? "present" : "absent",
           (unsigned long)g_rx_frames, (unsigned long)g_rx_bad,
           (g_tx_head - g_tx_tail + LINK_QLEN) % LINK_QLEN);
}

void link_set_text_sink(void (*sink)(const char *line))
{
    g_text_sink = sink;
}

void link_set_http_sink(void (*sink)(const char *text))
{
    g_http_sink = sink;
}

void link_set_tick(void (*tick)(void))
{
    g_tick = tick;
}

int link_start(void)
{
    return 0;
}
