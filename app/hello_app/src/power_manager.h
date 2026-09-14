/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * power_manager.h - power state machine (active -> idle -> sleep) with
 * ALS-driven AMOLED brightness, GPS-less <30 mA active budget model.
 *
 * Targets (Huangshan Pi, 450 mAh cell):
 *   active: ~30 mA   (map preview, 60 fps)
 *   idle:   ~8 mA    (dimmed, 15 fps, sensors low rate)
 *   sleep:  <0.5 mA  (screen off, LSM6DSL wake-on-motion via small core)
 */

#ifndef __POWER_MANAGER_H
#define __POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#define PM_ACTIVE_CURRENT_MA_TARGET  30
#define PM_IDLE_CURRENT_MA_TARGET    8
#define PM_SLEEP_CURRENT_UA_TARGET   500
/* screen stays on during development/demo: auto-sleep disabled */
#define PM_INACTIVE_TIMEOUT_MS       0x7FFFFFFF
#define PM_SLEEP_TIMEOUT_MS          0x7FFFFFFF
#define PM_FPS_ACTIVE                60
#define PM_FPS_IDLE                  15
#define PM_FPS_SLEEP                 4

typedef enum {
    PM_STATE_ACTIVE = 0,
    PM_STATE_IDLE,
    PM_STATE_SLEEP,
    PM_STATE_WAKE_TRANSITION,
} pm_state_t;

typedef struct {
    pm_state_t state;
    uint8_t    backlight_pct;      /* 0..100 (CO5300 brightness) */
    uint8_t    backlight_target;
    uint8_t    target_fps;
    uint32_t   last_activity_ms;
    uint32_t   state_entry_ms;
    uint32_t last_lcd_kick_ms;
    uint16_t   current_ma_estimate;
    bool       screen_on;
    bool       gpu_active;
    bool       bt_active;
} power_manager_t;

int  pm_init(power_manager_t *pm);
void pm_update(power_manager_t *pm);
void pm_report_activity(power_manager_t *pm);
void pm_enter_active(power_manager_t *pm);
void pm_enter_idle(power_manager_t *pm);
void pm_enter_sleep(power_manager_t *pm);
void pm_wake(power_manager_t *pm);
void pm_set_backlight(power_manager_t *pm, uint8_t pct);
uint16_t pm_estimate_current(power_manager_t *pm);
void pm_deinit(power_manager_t *pm);

#endif /* __POWER_MANAGER_H */
