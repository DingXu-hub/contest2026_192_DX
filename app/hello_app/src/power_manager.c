/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * power_manager.c - see power_manager.h.
 *
 * The CO5300 AMOLED brightness is applied with LCDDEVIO_SETCONTRAST on
 * /dev/lcd0 (mapped to the panel 0x51 register by the board LCD driver),
 * screen power with LCDDEVIO_SETPOWER.  The brightness ramp is smoothed
 * so ALS changes do not strobe the panel.
 */

#include <nuttx/config.h>

#include "power_manager.h"
#include "lcd_control.h"

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

int pm_init(power_manager_t *pm)
{
    memset(pm, 0, sizeof(*pm));
    pm->state = PM_STATE_ACTIVE;
    pm->backlight_pct = 50;
    pm->backlight_target = 50;
    pm->screen_on = true;
    pm->target_fps = PM_FPS_ACTIVE;
    pm->last_activity_ms = get_time_ms();
    pm->state_entry_ms = pm->last_activity_ms;
    pm->current_ma_estimate = PM_ACTIVE_CURRENT_MA_TARGET;

    lcd_ctrl_init();
    lcd_ctrl_set_power(true);
    lcd_ctrl_set_brightness(pm->backlight_pct);

    printf("[PM] init state=ACTIVE fps=%d backlight=%d%%\n",
           pm->target_fps, pm->backlight_pct);
    return 0;
}

void pm_update(power_manager_t *pm)
{
    uint32_t now = get_time_ms();
    uint32_t idle_time = now - pm->last_activity_ms;

    /* heartbeat: re-assert panel power + brightness every 5 s so a panel
     * that drifted into a low-power state (or a flaky FPC contact that
     * just needs a nudge) gets re-lit automatically */
    if (now - pm->last_lcd_kick_ms >= 5000)
    {
        pm->last_lcd_kick_ms = now;
        lcd_ctrl_set_power(true);
        lcd_ctrl_set_brightness(pm->backlight_pct);
    }

#ifdef CONFIG_ARCH_SIM
    /* on the host simulator keep the demo awake forever */
    idle_time = 0;
#endif

    switch (pm->state)
    {
    case PM_STATE_ACTIVE:
        if (idle_time > PM_INACTIVE_TIMEOUT_MS)
            pm_enter_idle(pm);
        break;
    case PM_STATE_IDLE:
        if (idle_time > PM_SLEEP_TIMEOUT_MS)
            pm_enter_sleep(pm);
        break;
    case PM_STATE_SLEEP:
        break;
    case PM_STATE_WAKE_TRANSITION:
        if (idle_time > 400)
            pm_enter_active(pm);
        break;
    default:
        break;
    }

    /* smooth brightness ramp toward the ALS target */
    if (pm->backlight_pct != pm->backlight_target)
    {
        int16_t diff = pm->backlight_target - pm->backlight_pct;
        int16_t step = (int16_t)((diff > 0) ? 3 : -3);

        if (abs(diff) <= 3)
            pm->backlight_pct = pm->backlight_target;
        else
            pm->backlight_pct += step;

        lcd_ctrl_set_brightness(pm->backlight_pct);
    }

    pm->current_ma_estimate = pm_estimate_current(pm);
}

void pm_report_activity(power_manager_t *pm)
{
    pm->last_activity_ms = get_time_ms();

    if (pm->state == PM_STATE_IDLE)
        pm_enter_active(pm);
    else if (pm->state == PM_STATE_SLEEP)
        pm_wake(pm);
}

void pm_enter_active(power_manager_t *pm)
{
    pm->state = PM_STATE_ACTIVE;
    pm->target_fps = PM_FPS_ACTIVE;
    pm->screen_on = true;
    pm->state_entry_ms = get_time_ms();

    lcd_ctrl_set_power(true);
    lcd_ctrl_set_brightness(pm->backlight_target);

    printf("[PM] -> ACTIVE (fps=%d)\n", pm->target_fps);
}

void pm_enter_idle(power_manager_t *pm)
{
    pm->state = PM_STATE_IDLE;
    pm->target_fps = PM_FPS_IDLE;
    pm->backlight_target = 15;
    pm->state_entry_ms = get_time_ms();

    printf("[PM] -> IDLE (fps=%d)\n", pm->target_fps);
}

void pm_enter_sleep(power_manager_t *pm)
{
    pm->state = PM_STATE_SLEEP;
    pm->screen_on = false;
    pm->target_fps = PM_FPS_SLEEP;
    pm->backlight_target = 0;
    pm->gpu_active = false;
    pm->bt_active = false;
    pm->state_entry_ms = get_time_ms();

    lcd_ctrl_set_brightness(0);
    lcd_ctrl_set_power(false);

    printf("[PM] -> SLEEP\n");
}

void pm_wake(power_manager_t *pm)
{
    pm->state = PM_STATE_WAKE_TRANSITION;
    pm->screen_on = true;
    pm->last_activity_ms = get_time_ms();
    pm->state_entry_ms = pm->last_activity_ms;

    lcd_ctrl_set_power(true);
    lcd_ctrl_set_brightness(pm->backlight_target);

    printf("[PM] -> WAKE_TRANSITION\n");
}

void pm_set_backlight(power_manager_t *pm, uint8_t pct)
{
    if (pct > 100) pct = 100;
    pm->backlight_target = pct;

    if (pm->state == PM_STATE_SLEEP)
        pm->backlight_target = 0;
}

uint16_t pm_estimate_current(power_manager_t *pm)
{
    uint16_t ma = 4; /* base: LDO + RTC + sensor rail */

    if (pm->screen_on)
    {
        ma += (uint16_t)(pm->backlight_pct * 18 / 100); /* AMOLED 0..18mA */
    }

    if (pm->gpu_active)
        ma += 6;

    if (pm->bt_active)
        ma += 3;

    if (pm->target_fps >= 30)
        ma += 8;  /* big core busy */
    else if (pm->target_fps > 1)
        ma += 2;

    return ma;
}

void pm_deinit(power_manager_t *pm)
{
    printf("[PM] deinit, final est %dmA\n", pm->current_ma_estimate);
    lcd_ctrl_deinit();
}
