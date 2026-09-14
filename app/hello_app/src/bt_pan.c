/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * bt_pan.c - see bt_pan.h.
 *
 * MOCK TRANSPORT
 * --------------
 * Until the vendor BT-PAN service is wired into the openvela port, the
 * built-in transport feeds a built-in sample GPX (Huangshan North Gate
 * climb) so the whole rendering pipeline can be validated without a
 * phone.  The mock is clearly labeled in the logs.
 *
 * WIRING THE REAL TRANSPORT
 * -------------------------
 * 1. Implement the bt_transport_t vtable using the vendor stack entry
 *    points (e.g. sifli_bt_pan_connect / sifli_bt_pan_recv on the small
 *    core, forwarded to the big core via the IPC queue in dual_core.c).
 * 2. Set bt->use_mock = false and bt->transport = &vendor_transport in
 *    bt_pan_init().
 * 3. The app code (main.c) is transport-agnostic.
 */

#include <nuttx/config.h>

#include "bt_pan.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static uint32_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* ------------------------------------------------------------------ *
 * Mock transport
 * ------------------------------------------------------------------ */

/* A 56-point climb loop around Huangshan North Gate with realistic
 * elevation (180 m -> 1864 m).  Enough to exercise zoom/pan/rotation. */
static const char mock_gpx[] =
    "<?xml version=\"1.0\"?>\n"
    "<gpx version=\"1.1\" creator=\"Huangshan Running Mock\">\n"
    "<trk><name>MOCK: Beimen Climb Loop</name><trkseg>\n"
    "<trkpt lat=\"30.1320\" lon=\"118.1630\"><ele>180</ele></trkpt>\n"
    "<trkpt lat=\"30.1330\" lon=\"118.1640\"><ele>220</ele></trkpt>\n"
    "<trkpt lat=\"30.1340\" lon=\"118.1650\"><ele>270</ele></trkpt>\n"
    "<trkpt lat=\"30.1350\" lon=\"118.1660\"><ele>320</ele></trkpt>\n"
    "<trkpt lat=\"30.1360\" lon=\"118.1670\"><ele>380</ele></trkpt>\n"
    "<trkpt lat=\"30.1370\" lon=\"118.1680\"><ele>450</ele></trkpt>\n"
    "<trkpt lat=\"30.1380\" lon=\"118.1690\"><ele>520</ele></trkpt>\n"
    "<trkpt lat=\"30.1390\" lon=\"118.1700\"><ele>600</ele></trkpt>\n"
    "<trkpt lat=\"30.1400\" lon=\"118.1710\"><ele>690</ele></trkpt>\n"
    "<trkpt lat=\"30.1410\" lon=\"118.1720\"><ele>780</ele></trkpt>\n"
    "<trkpt lat=\"30.1420\" lon=\"118.1730\"><ele>880</ele></trkpt>\n"
    "<trkpt lat=\"30.1430\" lon=\"118.1740\"><ele>980</ele></trkpt>\n"
    "<trkpt lat=\"30.1440\" lon=\"118.1750\"><ele>1080</ele></trkpt>\n"
    "<trkpt lat=\"30.1450\" lon=\"118.1760\"><ele>1180</ele></trkpt>\n"
    "<trkpt lat=\"30.1460\" lon=\"118.1770\"><ele>1280</ele></trkpt>\n"
    "<trkpt lat=\"30.1470\" lon=\"118.1780\"><ele>1380</ele></trkpt>\n"
    "<trkpt lat=\"30.1480\" lon=\"118.1790\"><ele>1480</ele></trkpt>\n"
    "<trkpt lat=\"30.1490\" lon=\"118.1800\"><ele>1560</ele></trkpt>\n"
    "<trkpt lat=\"30.1500\" lon=\"118.1810\"><ele>1620</ele></trkpt>\n"
    "<trkpt lat=\"30.1510\" lon=\"118.1820\"><ele>1660</ele></trkpt>\n"
    "<trkpt lat=\"30.1520\" lon=\"118.1830\"><ele>1700</ele></trkpt>\n"
    "<trkpt lat=\"30.1530\" lon=\"118.1840\"><ele>1740</ele></trkpt>\n"
    "<trkpt lat=\"30.1540\" lon=\"118.1850\"><ele>1780</ele></trkpt>\n"
    "<trkpt lat=\"30.1550\" lon=\"118.1860\"><ele>1820</ele></trkpt>\n"
    "<trkpt lat=\"30.1560\" lon=\"118.1870\"><ele>1864</ele></trkpt>\n"
    "<trkpt lat=\"30.1570\" lon=\"118.1880\"><ele>1840</ele></trkpt>\n"
    "<trkpt lat=\"30.1580\" lon=\"118.1890\"><ele>1800</ele></trkpt>\n"
    "<trkpt lat=\"30.1590\" lon=\"118.1900\"><ele>1750</ele></trkpt>\n"
    "<trkpt lat=\"30.1600\" lon=\"118.1910\"><ele>1680</ele></trkpt>\n"
    "<trkpt lat=\"30.1610\" lon=\"118.1920\"><ele>1600</ele></trkpt>\n"
    "<trkpt lat=\"30.1620\" lon=\"118.1930\"><ele>1520</ele></trkpt>\n"
    "<trkpt lat=\"30.1630\" lon=\"118.1940\"><ele>1450</ele></trkpt>\n"
    "<trkpt lat=\"30.1640\" lon=\"118.1950\"><ele>1400</ele></trkpt>\n"
    "<trkpt lat=\"30.1650\" lon=\"118.1960\"><ele>1350</ele></trkpt>\n"
    "<trkpt lat=\"30.1660\" lon=\"118.1970\"><ele>1300</ele></trkpt>\n"
    "<trkpt lat=\"30.1670\" lon=\"118.1980\"><ele>1250</ele></trkpt>\n"
    "<trkpt lat=\"30.1680\" lon=\"118.1990\"><ele>1200</ele></trkpt>\n"
    "<trkpt lat=\"30.1690\" lon=\"118.2000\"><ele>1150</ele></trkpt>\n"
    "<trkpt lat=\"30.1700\" lon=\"118.2010\"><ele>1100</ele></trkpt>\n"
    "<trkpt lat=\"30.1710\" lon=\"118.2020\"><ele>1050</ele></trkpt>\n"
    "<trkpt lat=\"30.1720\" lon=\"118.2030\"><ele>1000</ele></trkpt>\n"
    "<trkpt lat=\"30.1730\" lon=\"118.2040\"><ele>950</ele></trkpt>\n"
    "<trkpt lat=\"30.1740\" lon=\"118.2050\"><ele>900</ele></trkpt>\n"
    "<trkpt lat=\"30.1750\" lon=\"118.2060\"><ele>850</ele></trkpt>\n"
    "<trkpt lat=\"30.1760\" lon=\"118.2070\"><ele>800</ele></trkpt>\n"
    "<trkpt lat=\"30.1770\" lon=\"118.2080\"><ele>750</ele></trkpt>\n"
    "<trkpt lat=\"30.1780\" lon=\"118.2090\"><ele>700</ele></trkpt>\n"
    "<trkpt lat=\"30.1790\" lon=\"118.2100\"><ele>650</ele></trkpt>\n"
    "<trkpt lat=\"30.1800\" lon=\"118.2110\"><ele>600</ele></trkpt>\n"
    "<trkpt lat=\"30.1810\" lon=\"118.2120\"><ele>550</ele></trkpt>\n"
    "<trkpt lat=\"30.1820\" lon=\"118.2130\"><ele>500</ele></trkpt>\n"
    "<trkpt lat=\"30.1830\" lon=\"118.2140\"><ele>450</ele></trkpt>\n"
    "<trkpt lat=\"30.1840\" lon=\"118.2150\"><ele>400</ele></trkpt>\n"
    "<trkpt lat=\"30.1850\" lon=\"118.2160\"><ele>350</ele></trkpt>\n"
    "<trkpt lat=\"30.1860\" lon=\"118.2170\"><ele>300</ele></trkpt>\n"
    "<trkpt lat=\"30.1870\" lon=\"118.2180\"><ele>250</ele></trkpt>\n"
    "<trkpt lat=\"30.1880\" lon=\"118.2190\"><ele>200</ele></trkpt>\n"
    "<trkpt lat=\"30.1320\" lon=\"118.1630\"><ele>180</ele></trkpt>\n"
    "</trkseg></trk>\n"
    "</gpx>\n";

static int mock_connect(bt_pan_t *bt, const char *addr)
{
    (void)addr;
    bt->state = BT_STATE_CONNECTING;
    printf("[BT-PAN] MOCK: connecting to phone...\n");
    usleep(100 * 1000);
    bt->state = BT_STATE_READY;
    bt->last_activity_ms = get_time_ms();
    printf("[BT-PAN] MOCK: connected, PANU up (mock)\n");
    return 0;
}

static int mock_fetch_route(bt_pan_t *bt, const char *url,
                            bt_data_callback_t cb, void *user_data)
{
    (void)url;
    size_t len;

    if (bt->state != BT_STATE_READY && bt->state != BT_STATE_CONNECTED)
        return -1;

    bt->state = BT_STATE_FETCHING;
    /* verification/development: prefer a route dropped at
     * /data/huangshan.gpx (the sim maps /data -> /tmp), so a custom
     * track (e.g. a circle) can be fed to the whole pipeline.  Falls
     * back to the built-in mock GPX on real hardware. */
    len = 0;
    {
        FILE *f = fopen("/data/huangshan.gpx", "r");
        if (f)
        {
            len = (size_t)fread(bt->rx_buf, 1, BT_PAN_BUFFER_SIZE - 1, f);
            fclose(f);
            if (len > 0)
                bt->rx_buf[len] = '\0';
        }
    }
    if (len == 0)
    {
        len = strlen(mock_gpx);
        memcpy(bt->rx_buf, mock_gpx, len);
    }
    bt->rx_len = (uint16_t)len;
    bt->rx_total = (uint16_t)len;

    bt->state = BT_STATE_READY;
    bt->last_activity_ms = get_time_ms();

    if (cb)
        cb((const char *)bt->rx_buf, bt->rx_len, true, user_data);

    printf("[BT-PAN] MOCK: served %u bytes\n", bt->rx_len);
    return (int)bt->rx_len;
}

static void mock_poll(bt_pan_t *bt)
{
    uint32_t now = get_time_ms();

    if (bt->state == BT_STATE_DISCONNECTED && bt->auto_reconnect)
    {
        if (now - bt->last_activity_ms > BT_PAN_RECONNECT_MS)
        {
            bt->retry_count++;
            if (bt->retry_count <= BT_PAN_MAX_RETRIES)
            {
                printf("[BT-PAN] MOCK: reconnecting (%u)...\n",
                       (unsigned)bt->retry_count);
                mock_connect(bt, NULL);
            }
        }
    }
}

static int mock_disconnect(bt_pan_t *bt)
{
    bt->state = BT_STATE_DISCONNECTED;
    printf("[BT-PAN] MOCK: disconnected\n");
    return 0;
}

static const bt_transport_t mock_transport = {
    mock_connect,
    mock_fetch_route,
    mock_poll,
    mock_disconnect,
};

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */

int bt_pan_init(bt_pan_t *bt)
{
    memset(bt, 0, sizeof(*bt));
    bt->state = BT_STATE_DISCONNECTED;
    bt->auto_reconnect = true;
    bt->use_mock = true;
    bt->transport = &mock_transport;
    bt->last_activity_ms = get_time_ms();

    printf("[BT-PAN] initialized (mock transport; vendor PAN hooks in "
           "bt_pan.c)\n");
    return 0;
}

int bt_pan_connect_phone(bt_pan_t *bt, const char *addr)
{
    if (bt->state == BT_STATE_CONNECTED || bt->state == BT_STATE_READY)
        return 0;

    if (addr)
        strncpy(bt->remote_addr, addr, sizeof(bt->remote_addr) - 1);

    if (bt->transport && bt->transport->connect)
        return bt->transport->connect(bt, addr);

    return -1;
}

int bt_pan_fetch_route(bt_pan_t *bt, const char *url,
                       bt_data_callback_t cb, void *user_data)
{
    if (bt->transport && bt->transport->fetch_route)
        return bt->transport->fetch_route(bt, url, cb, user_data);

    return -1;
}

int bt_pan_disconnect(bt_pan_t *bt)
{
    int ret = 0;

    if (bt->transport && bt->transport->disconnect)
        ret = bt->transport->disconnect(bt);

    bt->auto_reconnect = false;
    return ret;
}

void bt_pan_poll(bt_pan_t *bt)
{
    if (bt->transport && bt->transport->poll)
        bt->transport->poll(bt);
}

void bt_pan_deinit(bt_pan_t *bt)
{
    bt_pan_disconnect(bt);
    printf("[BT-PAN] deinitialized\n");
}
