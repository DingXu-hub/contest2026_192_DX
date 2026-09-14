/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * lcd_control.h - AMOLED brightness & power control through /dev/lcd0
 * (the NuttX lcddev upper half).  The Huangshan Pi AMOLED (CO5300) has no
 * backlight PWM; brightness is the OLED register 0x51 written through the
 * panel driver's SetBrightness op, exposed as "contrast" (0..100) by the
 * lcddev interface.
 */

#ifndef __LCD_CONTROL_H
#define __LCD_CONTROL_H

#include <stdbool.h>

int  lcd_ctrl_init(void);
int  lcd_ctrl_set_brightness(int pct);  /* 0..100 */
int  lcd_ctrl_set_power(bool on);
void lcd_ctrl_deinit(void);

#endif /* __LCD_CONTROL_H */
