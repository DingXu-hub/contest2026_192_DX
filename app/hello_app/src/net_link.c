/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * net_link.c - SLIP link over the console UART + static IP setup.
 *
 * The board has a single Type-C port wired to a CH340 USB-UART bridge, so
 * the SoC USB (RNDIS) is unreachable.  Network access for the on-device AI
 * agent is therefore provided by SLIP over the same UART: the host (a
 * Linux VM) runs slattach on a PTY fed by a TCP byte-bridge, and acts as
 * the router/NAT for the board.
 *
 *   board 192.168.200.2  <--SLIP/1Mbps-->  192.168.200.1 host (NAT)
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <net/route.h>

#include <nuttx/net/ioctl.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/slip.h>
#include <netutils/netlib.h>

#define SLIP_TTY      "/dev/console"
#define SLIP_IF       "sl0"
#define SLIP_LOCAL_IP "192.168.200.2"
#define SLIP_PEER_IP  "192.168.200.1"
#define SLIP_MASK_IP  "0.0.0.0"   /* ptp: everything is on-link */
#define SLIP_DNS_IP   "223.5.5.5"

/* last probe results + host report helper (definitions are later in the
 * file; declared here because net_probe uses them) */
static int g_last_tcp_ret = -99;
static int g_last_tcp_errno;
static int g_last_dns_ok = -1;
static void report_probe(const char *text);

static bool inet4(const char *s, struct in_addr *out)
{
    return inet_aton(s, out) != 0;
}

/* put the UART into raw 1 Mbps mode (slip.c does not touch termios) */
static void slip_tty_raw(void)
{
    struct termios tio;
    int fd = open(SLIP_TTY, O_RDWR | O_NONBLOCK);

    if (fd < 0)
    {
        printf("[NET] open %s failed\n", SLIP_TTY);
        return;
    }
    if (tcgetattr(fd, &tio) == 0)
    {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B1000000);
        cfsetospeed(&tio, B1000000);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    close(fd);
}

/* dump a small text file (procfs diagnostics) */
static void dump_file(const char *path, const char *tag)
{
    FILE *f = fopen(path, "r");
    char line[200];

    if (!f)
    {
        printf("[NET] %s: open %s failed\n", tag, path);
        return;
    }
    while (fgets(line, sizeof(line), f))
        printf("[NET] %s %s", tag, line);
    fclose(f);
}

/* connectivity probe: DNS + TCP/443 to the LLM endpoint */
static void net_probe(void)
{
    struct hostent *he;
    struct sockaddr_in sa;
    int fd, ret;

    /* mount procfs so /proc/net/* is available for diagnostics
     * (NuttX needs the mount point directory to exist first) */
    mkdir("/proc", 0755);
    mount(NULL, "/proc", "procfs", 0, NULL);

    /* read back what the stack thinks the interface state is */
    {
        struct in_addr got;
        uint8_t fl = 0;
        if (netlib_get_ipv4addr(SLIP_IF, &got) == 0)
            printf("[NET] sl0 addr=%s\n", inet_ntoa(got));
        if (netlib_get_ipv4netmask(SLIP_IF, &got) == 0)
            printf("[NET] sl0 mask=%s\n", inet_ntoa(got));
        if (netlib_get_dripv4addr(SLIP_IF, &got) == 0)
            printf("[NET] sl0 peer=%s\n", inet_ntoa(got));
        if (netlib_getifstatus(SLIP_IF, &fl) == 0)
            printf("[NET] sl0 ifflags=0x%02x (IFF_UP=0x%02x)\n", fl,
                   IFF_UP);
    }

    dump_file("/proc/net/route/ipv4", "route:");
    dump_file("/proc/net/dev", "dev:");

    /* raw-IP test: on-link peer first, then the external DNS server */
    {
        const char *tests[2] = { SLIP_PEER_IP, SLIP_DNS_IP };
        for (int t = 0; t < 2; t++)
        {
            fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0)
                continue;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(53);
            sa.sin_addr.s_addr = inet_addr(tests[t]);
            uint8_t q[4] = { 0x12, 0x34, 0x01, 0x00 };
            ret = sendto(fd, q, sizeof(q), 0, (struct sockaddr *)&sa,
                         sizeof(sa));
            printf("[NET] raw UDP to %s:53 ret=%d errno=%d\n", tests[t],
                   ret, errno);
            close(fd);
        }
    }

    /* raw TCP test to a numeric IP: proves the reply path end-to-end
     * without involving DNS */
    {
        struct sockaddr_in ta;
        memset(&ta, 0, sizeof(ta));
        ta.sin_family = AF_INET;
        ta.sin_port = htons(9999);
        ta.sin_addr.s_addr = inet_addr("192.168.200.1");
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0)
        {
            struct timeval tv = { 5, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            ret = connect(fd, (struct sockaddr *)&ta, sizeof(ta));
            g_last_tcp_ret = ret;
            g_last_tcp_errno = errno;
            printf("[NET] TCP 192.168.200.1:9999 ret=%d errno=%d\n",
                   ret, errno);
            {
                char m[64];
                snprintf(m, sizeof(m), "tcp_ret=%d errno=%d", ret, errno);
                report_probe(m);
            }
            close(fd);
        }
    }

    dump_file("/proc/net/dev", "dev-a:");
    he = gethostbyname("api.deepseek.com");
    g_last_dns_ok = he ? 1 : 0;
    {
        char m[32];
        snprintf(m, sizeof(m), "dns_ok=%d", g_last_dns_ok);
        report_probe(m);
    }
    if (!he)
    {
        printf("[NET] DNS failed (api.deepseek.com) h_errno=%d errno=%d\n",
               h_errno, errno);
        dump_file("/proc/net/dev", "dev:");
        return;
    }
    printf("[NET] DNS ok api.deepseek.com -> %s\n",
           inet_ntoa(*(struct in_addr *)he->h_addr));

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        printf("[NET] socket failed\n");
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    memcpy(&sa.sin_addr, he->h_addr, sizeof(sa.sin_addr));

    ret = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    printf("[NET] TCP/443 to api.deepseek.com: %s\n",
           ret == 0 ? "OK" : "FAIL");
    close(fd);
}

/* add a route entry (libc addroute() style: only sockaddrs are filled) */
static int add_route(const char *target, const char *netmask,
                     const char *router)
{
    struct rtentry entry;
    struct sockaddr_in t, m, r;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int ret;

    if (fd < 0)
        return -1;
    memset(&t, 0, sizeof(t)); t.sin_family = AF_INET;
    t.sin_addr.s_addr = inet_addr(target);
    memset(&m, 0, sizeof(m)); m.sin_family = AF_INET;
    m.sin_addr.s_addr = inet_addr(netmask);
    memset(&r, 0, sizeof(r)); r.sin_family = AF_INET;
    r.sin_addr.s_addr = inet_addr(router);

    memset(&entry, 0, sizeof(entry));
    memcpy(&entry.rt_dst, &t, sizeof(t));
    memcpy(&entry.rt_genmask, &m, sizeof(m));
    memcpy(&entry.rt_gateway, &r, sizeof(r));
    ret = ioctl(fd, SIOCADDRT, (unsigned long)(uintptr_t)&entry);
    close(fd);
    return ret;
}

/* SLIP is a point-to-point interface: besides the peer address a default
 * route via the peer must be installed explicitly, otherwise nothing
 * (DNS, TCP) can ever leave the board. */
static int add_default_route(const char *gw)
{
    return add_route("0.0.0.0", "0.0.0.0", gw);
}

/* dump sl0 counters (useful when the console is busy with SLIP frames) */
/* last probe results (survive until the next probe) so they can be read
 * from the console after the SLIP link is stopped */

/* send the probe result to the host as a UDP datagram: proves the full IP
 * path and lets the host read the result while SLIP owns the console */
static void report_probe(const char *text)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9999);
    sa.sin_addr.s_addr = inet_addr(SLIP_PEER_IP);
    sendto(fd, text, strlen(text), 0, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
}

void net_link_dump_stats(void)
{
    printf("[NET] last-probe tcp_ret=%d errno=%d dns_ok=%d\n",
           g_last_tcp_ret, g_last_tcp_errno, g_last_dns_ok);
    dump_file("/proc/net/dev", "stat:");
}

void net_link_start(void)
{
    struct in_addr a;

    printf("[NET] SLIP link up on %s\n", SLIP_TTY);
    slip_tty_raw();

    int ret = slip_initialize(0, SLIP_TTY);
    printf("[NET] slip_initialize ret=%d\n", ret);

    ret = netlib_ifup(SLIP_IF);
    printf("[NET] ifup %s ret=%d\n", SLIP_IF, ret);

    /* addresses first, then bring the interface up (canonical order for
     * point-to-point devices), then routes */
    if (inet4(SLIP_LOCAL_IP, &a))
        printf("[NET] set ip %s ret=%d\n", SLIP_LOCAL_IP,
               netlib_set_ipv4addr(SLIP_IF, &a));
    if (inet4(SLIP_MASK_IP, &a))
        printf("[NET] set mask ret=%d\n",
               netlib_set_ipv4netmask(SLIP_IF, &a));
    if (inet4(SLIP_PEER_IP, &a))
        printf("[NET] set gw %s ret=%d\n", SLIP_PEER_IP,
               netlib_set_dripv4addr(SLIP_IF, &a));
    if (inet4(SLIP_DNS_IP, &a))
        printf("[NET] set dns %s ret=%d\n", SLIP_DNS_IP,
               netlib_set_ipv4dnsaddr(&a));

    printf("[NET] ifup #2 ret=%d\n", netlib_ifup(SLIP_IF));

    /* Force the driver's own ifup: the netdev layer did not appear to call
     * it (slip's "Bringing up" log never showed), and without it the SLIP
     * receive side is never armed. */
    {
        FAR struct net_driver_s *dev = netdev_findbyname(SLIP_IF);
        if (dev && dev->d_ifup)
        {
            int r = dev->d_ifup(dev);
            printf("[NET] direct d_ifup ret=%d\n", r);
        }
    }

    /* The SLIP driver never signals carrier; NuttX requires IFF_RUNNING
     * (0x04) or every send fails with ENETUNREACH in udp/tcp send path. */
    {
        FAR struct net_driver_s *dev = netdev_findbyname(SLIP_IF);
        if (dev)
        {
            netdev_carrier_on(dev);
            printf("[NET] carrier on, d_flags=0x%02x\n", dev->d_flags);
        }
        else
            printf("[NET] netdev_findbyname(%s) failed\n", SLIP_IF);
    }
    printf("[NET] peer host route ret=%d\n",
           add_route(SLIP_PEER_IP, "255.255.255.255", SLIP_PEER_IP));
    printf("[NET] default route via %s ret=%d\n", SLIP_PEER_IP,
           add_default_route(SLIP_PEER_IP));
    /* give the link a moment, then probe periodically so the host can
     * always observe the result (and so timing races cannot hide it) */
    sleep(2);
    for (;;)
    {
        net_probe();
        sleep(20);
    }
}
