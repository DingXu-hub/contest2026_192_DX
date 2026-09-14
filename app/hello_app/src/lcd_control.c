/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * lcd_control.c - AMOLED brightness & power control (see lcd_control.h).
 */

#include <nuttx/config.h>
#include <nuttx/lcd/lcd_dev.h>

#include "lcd_control.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define LCD_DEV "/dev/lcd0"

static int g_lcd_fd = -1;

int lcd_ctrl_init(void)
{
    g_lcd_fd = open(LCD_DEV, O_RDWR);
    if (g_lcd_fd < 0)
    {
        printf("[LCD] %s open failed\n", LCD_DEV);
        return -1;
    }
    printf("[LCD] %s ready\n", LCD_DEV);
    return 0;
}

int lcd_ctrl_set_brightness(int pct)
{
    if (g_lcd_fd < 0)
        return -1;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    return ioctl(g_lcd_fd, LCDDEVIO_SETCONTRAST, (unsigned long)pct);
}

int lcd_ctrl_set_power(bool on)
{
    if (g_lcd_fd < 0)
        return -1;

    return ioctl(g_lcd_fd, LCDDEVIO_SETPOWER, (unsigned long)(on ? 1 : 0));
}

void lcd_ctrl_deinit(void)
{
    if (g_lcd_fd >= 0)
    {
        close(g_lcd_fd);
        g_lcd_fd = -1;
    }
}
