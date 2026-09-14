/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_diag.h - runtime diagnostic verbosity.
 *
 * Set APP_DIAG_VERBOSE to 1 for development builds: periodic fusion
 * diagnostics (2 Hz [Fuse]), the heartbeat [Alive], the render FPS line
 * and touch debugging are printed.  Release/demo builds keep it 0 so the
 * console stays quiet (less UART traffic, less CPU).
 */
#ifndef __APP_DIAG_H
#define __APP_DIAG_H

#define APP_DIAG_VERBOSE 0

/* 1 = one-shot demo seed: writes a handful of run history entries into the
 * KV store on the first boot after flashing (the stats page then has data
 * for screenshots/video).  The entries persist in /dev/config0, so this can
 * be flashed once and turned off again. */
#define APP_DEMO_SEED 0

#endif /* __APP_DIAG_H */
