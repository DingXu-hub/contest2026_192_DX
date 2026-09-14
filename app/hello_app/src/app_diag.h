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

#endif /* __APP_DIAG_H */
