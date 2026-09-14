/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * gpu.h - ePicasso (EPIC) GPU abstraction for SF32LB52
 *
 * The SF32LB52 integrates SiFli's in-house "ePicasso" 2.5D graphics engine
 * (HAL_EPIC_*).  It provides hardware rectangle fill, image copy, rotation
 * and scaling (bilinear) engines that offload the big core.
 *
 * Two backends are provided:
 *   - CONFIG_EXAMPLES_HUANGSHAN_RUNNING_GPU_ACCEL=y : real EPIC hardware
 *   - otherwise                                   : software fallback
 * Runtime probing: gpu_init() returns 0 on success, the renderer then uses
 * the accelerated path; a negative return means the software fallback is
 * used (rendering still works).
 */

#ifndef __HUANGSHAN_GPU_H
#define __HUANGSHAN_GPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the ePicasso GPU.  Returns 0 on success, negative errno on
 * failure (caller falls back to the software renderer). */
int gpu_init(void);

/* De-initialize (power down) the GPU. */
void gpu_deinit(void);

/* Hardware fill of a rectangle inside dst with an RGB565 color.
 * Coordinates are clipped to [0, dst_w) x [0, dst_h). */
void gpu_fill_rect(uint16_t *dst, int dst_w, int dst_h,
                   int x0, int y0, int x1, int y1, uint16_t color);

/* Rotate+scale the src tile (RGB565) around its center and composite it on
 * top of dst (opaque), writing the result back to dst.
 *
 *   src      - RGB565 tile (tile_w x tile_h, stride tile_w pixels)
 *   dst      - RGB565 target buffer (dst_w x dst_h, stride dst_w pixels)
 *   center_x/center_y - position (in dst coordinates) of the src center
 *   angle_deg - counter-clockwise rotation in degrees
 *   scale    - zoom factor (1.0 = no scaling); 2.0 scales up 2x
 *
 * Everything outside the rotated tile keeps its previous dst content, so
 * callers fill dst with the background color first. */
void gpu_rotate_scale(uint16_t *src, int tile_w, int tile_h,
                      uint16_t *dst, int dst_w, int dst_h,
                      int center_x, int center_y,
                      float angle_deg, float scale);

/* Copy a rectangular region from src to dst (1:1 blit).  NOTE: the EPIC
 * HAL only offers an interrupt-driven copy (HAL_EPIC_Copy_IT) which needs
 * the EPIC IRQ handler attached; the renderer uses the polling rotate
 * path instead, so this entry point is currently unused. */
void gpu_blit(const uint16_t *src, int src_w, int src_h, int src_stride,
              uint16_t *dst, int dst_w, int dst_h, int dst_stride,
              int dst_x, int dst_y);

#ifdef __cplusplus
}
#endif

#endif /* __HUANGSHAN_GPU_H */
