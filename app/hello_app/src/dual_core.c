/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * dual_core.c - local message queue modelling the big/small core IPC.
 *
 * REAL IPC HOOK
 * -------------
 * When the vendor small-core firmware and its IPC transport are
 * available in a build, replace the in-RAM queue with the mailbox HAL
 * (bf0_hal_mailbox.h): dual_core_send_cmd() would push a message into
 * the LCPU mailbox and the LCPU IRQ would feed
 * dual_core_process_queue().  The message layout above (channel/type/
 * length/data) is already the wire format, so the rest of the app is
 * unaffected.
 */

#include "dual_core.h"
#include <stdio.h>
#include <string.h>

#define RPMsg_MAX_MSG_SIZE  512
#define RPMsg_QUEUE_SIZE     32

typedef struct {
    ipc_message_t msg;
    uint8_t payload[RPMsg_MAX_MSG_SIZE];
} ipc_queue_item_t;

static struct {
    ipc_queue_item_t queue[RPMsg_QUEUE_SIZE];
    volatile uint8_t head;
    volatile uint8_t tail;
    void (*sensor_cb)(uint8_t type, void *data, uint16_t len);
    void (*bt_cb)(uint8_t type, void *data, uint16_t len);
    bool initialized;
} g_ipc;

int dual_core_init(void)
{
    memset(&g_ipc, 0, sizeof(g_ipc));
    g_ipc.initialized = true;
    printf("[DualCore] IPC queue ready (local; LCPU mailbox hook in "
           "dual_core.c)\n");
    return 0;
}

int dual_core_send_cmd(uint8_t channel, uint8_t type, void *data,
                       uint16_t len)
{
    uint8_t next;

    if (!g_ipc.initialized)
        return -1;
    if (len > RPMsg_MAX_MSG_SIZE)
        return -1;

    next = (g_ipc.head + 1) % RPMsg_QUEUE_SIZE;
    if (next == g_ipc.tail)
        return -1; /* full */

    ipc_queue_item_t *item = &g_ipc.queue[g_ipc.head];
    item->msg.channel = channel;
    item->msg.type = type;
    item->msg.length = len;
    if (data && len > 0)
        memcpy(item->payload, data, len);

    g_ipc.head = next;
    return 0;
}

int dual_core_send_sensor(uint8_t type, void *data, uint16_t len)
{
    return dual_core_send_cmd(RPMsg_CHANNEL_SENSOR, type, data, len);
}

void dual_core_set_sensor_callback(void (*cb)(uint8_t type, void *data,
                                              uint16_t len))
{
    g_ipc.sensor_cb = cb;
}

void dual_core_set_bt_callback(void (*cb)(uint8_t type, void *data,
                                          uint16_t len))
{
    g_ipc.bt_cb = cb;
}

void dual_core_process_queue(void)
{
    while (g_ipc.tail != g_ipc.head)
    {
        ipc_queue_item_t *item = &g_ipc.queue[g_ipc.tail];
        ipc_message_t *msg = &item->msg;

        switch (msg->channel)
        {
        case RPMsg_CHANNEL_SENSOR:
            if (g_ipc.sensor_cb)
                g_ipc.sensor_cb(msg->type, item->payload, msg->length);
            break;
        case RPMsg_CHANNEL_BT:
            if (g_ipc.bt_cb)
                g_ipc.bt_cb(msg->type, item->payload, msg->length);
            break;
        default:
            break;
        }

        g_ipc.tail = (g_ipc.tail + 1) % RPMsg_QUEUE_SIZE;
    }
}
