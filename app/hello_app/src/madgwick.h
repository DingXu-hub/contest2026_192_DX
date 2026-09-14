/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * madgwick.h - Madgwick AHRS quaternion filter (world-frame heading).
 */

#ifndef HUANGSHAN_MADGWICK_H
#define HUANGSHAN_MADGWICK_H

void madgwick_reset(void);
void madgwick_update(float gx, float gy, float gz,
                     float ax, float ay, float az,
                     float mx, float my, float mz, float dt);
float madgwick_yaw_deg(void);
float madgwick_roll_deg(void);
float madgwick_pitch_deg(void);

#endif
