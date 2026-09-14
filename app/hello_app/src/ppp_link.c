/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ppp_link.c - PPP over the console UART for on-device AI network access.
 *
 * The board has a single Type-C port (CH340 UART), so cloud LLM traffic is
 * carried by PPP between the board (/dev/ttyS0) and a Linux host running
 * pppd (fed by a TCP byte-bridge from the PC serial port).
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#include <netutils/pppd.h>
#include <netutils/netlib.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>

#define PPP_TTY      "/dev/console"
#define PPP_DNS_IP   "223.5.5.5"

extern volatile bool g_net_silent;

/* static (not stack): pppd keeps the pointer for the whole session */
static struct pppd_settings_s g_ppp_settings;

/* read raw bytes from one device for a few seconds and report the count */
static void rxdiag_one(const char *path, int seconds)
{
    int fd = open(path, O_RDWR | O_NONBLOCK);
    uint8_t buf[256];
    uint32_t total = 0;
    int first = -1;
    int head = 0;
    uint8_t hb[8];

    if (fd < 0)
    {
        printf("[RXDIAG] %s open failed errno=%d\n", path, errno);
        return;
    }
    for (int t = 0; t < seconds * 10; t++)
    {
        int n = read(fd, buf, sizeof(buf));
        if (n > 0)
        {
            if (first < 0)
                first = t;
            total += (uint32_t)n;
            for (int i = 0; i < n && head < (int)sizeof(hb); i++)
                hb[head++] = buf[i];
        }
        usleep(100000);
    }
    close(fd);
    printf("[RXDIAG] %s total=%u first=%dms", path, total,
           first < 0 ? -1 : first * 100);
    for (int i = 0; i < head; i++)
        printf(" %02x", hb[i]);
    printf("\n");
}

void ppp_link_rxdiag(int seconds)
{
    rxdiag_one(PPP_TTY, seconds);
    rxdiag_one("/dev/console", seconds);
}

/* poll() test: does POLLIN ever fire on the console device?  (SLIP relies
 * on file_poll(); if this never fires, slip can never receive.) */
void ppp_link_polldiag(int seconds)
{
    struct pollfd pfd;
    int fd = open("/dev/console", O_RDWR | O_NONBLOCK);
    int events = 0;
    uint32_t bytes = 0;

    if (fd < 0)
    {
        printf("[POLDIAG] open failed errno=%d\n", errno);
        return;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (int t = 0; t < seconds * 4; t++)
    {
        int r = poll(&pfd, 1, 250);
        if (r > 0 && (pfd.revents & POLLIN))
        {
            uint8_t b[256];
            events++;
            int n = read(fd, b, sizeof(b));
            if (n > 0)
                bytes += (uint32_t)n;
            pfd.revents = 0;
        }
    }
    close(fd);
    printf("[POLDIAG] poll events=%d bytes=%u\n", events, bytes);
}

/* NuttX pppd does not touch termios; the console tty keeps its echo /
 * line-discipline settings which would buffer/echo the binary PPP frames
 * coming from the host.  Put the tty into raw mode first. */
static void ppp_tty_raw(void)
{
    struct termios tio;
    int fd = open(PPP_TTY, O_RDWR | O_NONBLOCK);

    if (fd < 0)
    {
        printf("[PPP] open %s failed errno=%d\n", PPP_TTY, errno);
        return;
    }
    if (tcgetattr(fd, &tio) == 0)
    {
        cfmakeraw(&tio);   /* keep VMIN=1: pppd needs blocking reads */
        printf("[PPP] tty raw ret=%d\n", tcsetattr(fd, TCSANOW, &tio));
    }
    else
        printf("[PPP] tcgetattr failed errno=%d\n", errno);
    close(fd);
}


/* probe the TUN device used by NuttX pppd */
void ppp_link_tundiag(void)
{
    int fd = open("/dev/tun", O_RDWR);
    printf("[TUNDIAG] open(/dev/tun) -> %d errno=%d\n", fd, errno);
    if (fd >= 0)
        close(fd);
}

void ppp_link_start(void)
{
    memset(&g_ppp_settings, 0, sizeof(g_ppp_settings));
    strlcpy(g_ppp_settings.ttyname, PPP_TTY,
            sizeof(g_ppp_settings.ttyname));
    g_ppp_settings.connect_script = NULL;      /* direct link, no modem */
    g_ppp_settings.disconnect_script = NULL;

    ppp_tty_raw();
    printf("[PPP] starting on %s (no chat script)\n", PPP_TTY);
    int ret = pppd(&g_ppp_settings);
    printf("[PPP] pppd exited ret=%d errno=%d\n", ret, errno);
}

/* after the link should be up: publish DNS and probe the LLM endpoint */
void ppp_link_watch(void)
{
    sleep(30);

    struct in_addr a;
    if (inet_aton(PPP_DNS_IP, &a))
        printf("[PPP] dns %s ret=%d\n", PPP_DNS_IP,
               netlib_set_ipv4dnsaddr(&a));

    struct hostent *he = gethostbyname("api.deepseek.com");
    if (!he)
    {
        printf("[PPP] DNS failed h_errno=%d errno=%d\n", h_errno, errno);
        return;
    }
    printf("[PPP] DNS ok api.deepseek.com -> %s\n",
           inet_ntoa(*(struct in_addr *)he->h_addr));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    memcpy(&sa.sin_addr, he->h_addr, sizeof(sa.sin_addr));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0)
    {
        int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        printf("[PPP] TCP/443 api.deepseek.com: %s (errno=%d)\n",
               r == 0 ? "OK" : "FAIL", errno);
        close(fd);
    }
}
