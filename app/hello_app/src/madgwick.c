/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * madgwick.c - Madgwick AHRS quaternion filter (standard implementation,
 * Sebastian Madgwick). Fuses gyro + accelerometer + magnetometer into a
 * world-frame orientation; yaw comes out as heading from magnetic north
 * regardless of how the device is tilted/rotated (phone-grade compass).
 */

#include "madgwick.h"

#include <math.h>

#define BETA 0.1f   /* filter gain: larger = trust accel/mag more */

static float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;

static float inv_sqrt(float x)
{
    return 1.0f / sqrtf(x);
}

void madgwick_reset(void)
{
    q0 = 1.0f; q1 = 0.0f; q2 = 0.0f; q3 = 0.0f;
}

void madgwick_update(float gx, float gy, float gz,
                     float ax, float ay, float az,
                     float mx, float my, float mz, float dt)
{
    float norm;
    float hx, hy, _2bx, _2bz;
    float s0, s1, s2, s3;
    float qDot1, qDot2, qDot3, qDot4;
    float q0q0, q0q1, q0q2, q0q3;
    float q1q1, q1q2, q1q3, q2q2, q2q3, q3q3;

    /* rate of change of quaternion from gyroscope */
    qDot1 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
    qDot2 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy);
    qDot3 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx);
    qDot4 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx);

    /* auxiliary variables to avoid repeated arithmetic */
    q0q0 = q0 * q0; q0q1 = q0 * q1; q0q2 = q0 * q2; q0q3 = q0 * q3;
    q1q1 = q1 * q1; q1q2 = q1 * q2; q1q3 = q1 * q3;
    q2q2 = q2 * q2; q2q3 = q2 * q3; q3q3 = q3 * q3;

    /* normalise accelerometer measurement */
    norm = inv_sqrt(ax * ax + ay * ay + az * az);
    ax *= norm; ay *= norm; az *= norm;

    /* normalise magnetometer measurement */
    norm = inv_sqrt(mx * mx + my * my + mz * mz);
    mx *= norm; my *= norm; mz *= norm;

    /* reference direction of Earth's magnetic field */
    hx = 2.0f * mx * (0.5f - q2q2 - q3q3)
       + 2.0f * my * (q1q2 - q0q3)
       + 2.0f * mz * (q1q3 + q0q2);
    hy = 2.0f * mx * (q1q2 + q0q3)
       + 2.0f * my * (0.5f - q1q1 - q3q3)
       + 2.0f * mz * (q2q3 - q0q1);
    _2bx = sqrtf(hx * hx + hy * hy);
    _2bz = -2.0f * mx * (q1q3 - q0q2)
         + 2.0f * my * (q2q3 + q0q1)
         + 2.0f * mz * (0.5f - q1q1 - q2q2);
    if (_2bx < 0.0001f)
        _2bx = 0.0001f;

    /* gradient descent algorithm corrective step */
    s0 = -2.0f * q2 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
       - 2.0f * q3 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
       + 2.0f * q0 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - az)
       + _2bz * q2 * (_2bx * (0.5f - q2q2 - q3q3) - mx)
       + (_2bx * q3 - _2bz * q1) * (_2bx * (q1q2 - q0q3) - my)
       + _2bx * q0 * (_2bx * (q1q3 + q0q2) - mz);
    s1 =  2.0f * q3 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
       + 2.0f * q0 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
       + 2.0f * q1 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - az)
       + _2bz * q3 * (_2bx * (0.5f - q2q2 - q3q3) - mx)
       + (_2bx * q2 - _2bz * q0) * (_2bx * (q1q2 - q0q3) - my)
       + _2bx * q1 * (_2bx * (q1q3 + q0q2) - mz);
    s2 = -2.0f * q0 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
       + 2.0f * q1 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
       + 2.0f * q2 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - az)
       + _2bz * q0 * (_2bx * (0.5f - q2q2 - q3q3) - mx)
       + (_2bx * q1 + _2bz * q3) * (_2bx * (q1q2 - q0q3) - my)
       + _2bx * q2 * (_2bx * (q1q3 + q0q2) - mz);
    s3 =  2.0f * q1 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
       + 2.0f * q2 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
       + 2.0f * q3 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - az)
       + _2bz * q1 * (_2bx * (0.5f - q2q2 - q3q3) - mx)
       + (_2bx * q0 + _2bz * q2) * (_2bx * (q1q2 - q0q3) - my)
       + _2bx * q3 * (_2bx * (q1q3 + q0q2) - mz);

    norm = inv_sqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
    s0 *= norm; s1 *= norm; s2 *= norm; s3 *= norm;

    /* apply feedback step */
    qDot1 -= BETA * s0;
    qDot2 -= BETA * s1;
    qDot3 -= BETA * s2;
    qDot4 -= BETA * s3;

    /* integrate rate of change of quaternion to yield quaternion */
    q0 += qDot1 * dt;
    q1 += qDot2 * dt;
    q2 += qDot3 * dt;
    q3 += qDot4 * dt;

    /* normalise quaternion */
    norm = inv_sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    q0 *= norm; q1 *= norm; q2 *= norm; q3 *= norm;
}

float madgwick_yaw_deg(void)
{
    /* yaw: rotation about world Z, from magnetic north */
    float yaw = atan2f(2.0f * (q0 * q3 + q1 * q2),
                       1.0f - 2.0f * (q2 * q2 + q3 * q3));
    yaw *= 57.29578f;
    if (yaw < 0.0f)
        yaw += 360.0f;
    return yaw;
}

float madgwick_roll_deg(void)
{
    float roll = atan2f(2.0f * (q0 * q1 + q2 * q3),
                        1.0f - 2.0f * (q1 * q1 + q2 * q2));
    return roll * 57.29578f;
}

float madgwick_pitch_deg(void)
{
    float pitch = asinf(2.0f * (q0 * q2 - q3 * q1));
    return pitch * 57.29578f;
}
