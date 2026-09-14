/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * dual_core.h - big core / small core IPC abstraction.
 *
 * On the SF32LB52 the big core (CM33, NuttX + rendering + interaction)
 * and the small core (CM0+, vendor BT stack, optional sensor hub)
 * exchange messages.  This module provides a message queue that is
 * drained by the render task; with the real vendor mailbox/RPMsg it
 * becomes a transport to the LCPU (see dual_core.c notes).
 */

#ifndef __DUAL_CORE_H
#define __DUAL_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define RPMsg_CHANNEL_SENSOR  0
#define RPMsg_CHANNEL_BT      1
#define RPMsg_CHANNEL_CMD     2

#define SENSOR_MSG_MAG        0x01
#define SENSOR_MSG_IMU        0x02
#define SENSOR_MSG_ALS        0x03
#define SENSOR_MSG_WAKE       0x04

#define BT_MSG_GPX_DATA       0x10
#define BT_MSG_GPX_COMPLETE   0x11
#define BT_MSG_GPX_ERROR      0x12
#define BT_MSG_STATUS         0x13

#define CMD_MSG_RESUME         0x20
#define CMD_MSG_SUSPEND        0x21
#define CMD_MSG_SET_BRIGHTNESS 0x22
#define CMD_MSG_REQ_SENSORS    0x23
#define CMD_MSG_CONNECT_PHONE  0x24
#define CMD_MSG_FETCH_ROUTE    0x25

typedef struct {
    uint8_t channel;
    uint8_t type;
    uint16_t length;
    uint8_t data[];
} ipc_message_t;

typedef struct {
    float heading_deg;
    float pitch_deg;
    float roll_deg;
    int32_t mag_x;
    int32_t mag_y;
    int32_t mag_z;
} sensor_mag_data_t;

typedef struct {
    float accel_x;
    float accel_y;
    float accel_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
    bool raise_detected;
    bool view_locked;
} sensor_imu_data_t;

typedef struct {
    uint16_t lux;
    uint8_t backlight_pct;
} sensor_als_data_t;

int  dual_core_init(void);
int  dual_core_send_cmd(uint8_t channel, uint8_t type, void *data,
                        uint16_t len);
int  dual_core_send_sensor(uint8_t type, void *data, uint16_t len);
void dual_core_set_sensor_callback(void (*cb)(uint8_t type, void *data,
                                              uint16_t len));
void dual_core_set_bt_callback(void (*cb)(uint8_t type, void *data,
                                          uint16_t len));
void dual_core_process_queue(void);

#endif /* __DUAL_CORE_H */
