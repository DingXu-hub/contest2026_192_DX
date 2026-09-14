/*
 * devshot.h - development-only full-frame PPM snapshot over the UART.
 *
 * Set HUANGSHAN_DEV_SHOT to 1 to build a debug firmware that cycles the
 * pages and dumps one PPM frame every 5 s (pair it with
 * tools/listen_ui.py).  Release builds keep it at 0: all the code below
 * is compiled out.
 */
#ifndef __HUANGSHAN_DEVSHOT_H
#define __HUANGSHAN_DEVSHOT_H

#define HUANGSHAN_DEV_SHOT 0   /* dev: capture */

#if HUANGSHAN_DEV_SHOT
extern bool g_print_silent;
#endif

#endif /* __HUANGSHAN_DEVSHOT_H */
