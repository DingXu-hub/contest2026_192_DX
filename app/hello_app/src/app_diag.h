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

/* --- demo run simulation (default off) -------------------------------- *
 * Turns the running engine into a deterministic simulator: the step
 * detector is replaced by a steady cadence and the bearing is turned at a
 * constant rate, so the dead-reckoned trail draws a CIRCLE on the run page.
 * Use it for screenshots/video when a real outdoor run is not possible; the
 * trail is then SIMULATED and must be labelled as such in the video.
 * ---------------------------------------------------------------------- */
#define APP_DEMO_RUN 1
#define APP_DEMO_RUN_SPM 170        /* synthesised cadence, steps/minute */
#define APP_DEMO_RUN_YAW_DPS 9.0f   /* turn rate -> one lap about every 40 s */

#endif /* __APP_DIAG_H */
