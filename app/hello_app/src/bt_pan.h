/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * bt_pan.h - Bluetooth PAN client for GPX route download.
 *
 * The SF32LB52 radio is driven by the vendor BT stack that runs on the
 * small core (H4 HCI transport, CONFIG_BT_H4/CONFIG_UART_BTH4); the
 * NuttX BT host stack (CONFIG_BT + CONFIG_BT_GATT_CLIENT) runs on the
 * big core.  The BT-PAN profile itself (PANU over the PAN profile / BLE
 * PAN service) is exposed by the vendor stack; this module abstracts it
 * behind a transport vtable so the app compiles and runs with the
 * built-in mock transport, and the real vendor transport can be plugged
 * in without touching the rest of the app.
 */

#ifndef __BT_PAN_H
#define __BT_PAN_H

#include <stdint.h>
#include <stdbool.h>

#define BT_PAN_BUFFER_SIZE      8192
#define BT_PAN_URL_MAX_LEN       512
#define BT_PAN_RECONNECT_MS      5000
#define BT_PAN_MAX_RETRIES       3

typedef enum {
    BT_STATE_DISCONNECTED = 0,
    BT_STATE_SCANNING,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED,
    BT_STATE_FETCHING,
    BT_STATE_READY,
    BT_STATE_ERROR,
} bt_state_t;

typedef void (*bt_data_callback_t)(const char *data, uint16_t len,
                                   bool complete, void *user_data);

typedef struct bt_pan_s bt_pan_t;

/* Transport vtable: set to a vendor implementation, or NULL for the
 * built-in mock (returns a built-in sample route). */
typedef struct {
    int  (*connect)(bt_pan_t *bt, const char *addr);
    int  (*fetch_route)(bt_pan_t *bt, const char *url,
                        bt_data_callback_t cb, void *user_data);
    void (*poll)(bt_pan_t *bt);
    int  (*disconnect)(bt_pan_t *bt);
} bt_transport_t;

struct bt_pan_s {
    bt_state_t      state;
    char            remote_addr[18];
    char            remote_name[32];
    uint8_t         rx_buf[BT_PAN_BUFFER_SIZE];
    uint16_t        rx_len;
    uint16_t        rx_total;
    uint32_t        last_activity_ms;
    uint32_t        retry_count;
    bool            auto_reconnect;
    bool            use_mock;       /* default true until a real transport
                                     * is installed */
    const bt_transport_t *transport;
    void           *transport_ctx;  /* vendor handle */
};

int  bt_pan_init(bt_pan_t *bt);
int  bt_pan_connect_phone(bt_pan_t *bt, const char *addr);
int  bt_pan_fetch_route(bt_pan_t *bt, const char *url,
                        bt_data_callback_t cb, void *user_data);
int  bt_pan_disconnect(bt_pan_t *bt);
void bt_pan_poll(bt_pan_t *bt);
void bt_pan_deinit(bt_pan_t *bt);

#endif /* __BT_PAN_H */
