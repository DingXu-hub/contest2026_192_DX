/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * gpu_epic.c - ePicasso (EPIC) GPU backend for the Huangshan Running renderer
 *
 * Uses the SiFli HAL_EPIC_* API (bf0_hal_epic.h) that ships in
 * vendor_sifli/chips/drivers/{Include,hal} and is compiled into the
 * architecture library whenever the board build globs the HAL sources.
 *
 * The EPIC engine can fill, copy, rotate and scale RGB565 buffers in
 * PSRAM/SRAM; the framebuffer of this board lives in the 8 MB OPI-PSRAM,
 * so both src tiles and the dst framebuffer are GPU-accessible.
 *
 * Cache notes: the CPU writes the src tile, so we clean the D-cache before
 * GPU reads; after GPU writes the framebuffer we invalidate the D-cache
 * before the CPU (or LCDC DMA) reads it.
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <nuttx/cache.h>

#include "gpu.h"

#ifdef CONFIG_EXAMPLES_HUANGSHAN_RUNNING_GPU_ACCEL
#include "bf0_hal.h"

static EPIC_HandleTypeDef s_epic;
static EZIP_HandleTypeDef  s_ezip;
static bool s_gpu_ready = false;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int gpu_init(void)
{
    if (s_gpu_ready)
        return 0;

    /* The EPIC engine pairs with the EZIP (image codec) engine; HAL_EPIC_Init
     * dereferences epic->hezip, so initialize EZIP first. */
    memset(&s_ezip, 0, sizeof(s_ezip));
    s_ezip.Instance = EZIP;
    if (HAL_EZIP_Init(&s_ezip) != HAL_OK)
    {
        printf("[GPU] HAL_EZIP_Init failed\n");
        return -1;
    }

    memset(&s_epic, 0, sizeof(s_epic));
    s_epic.Instance = EPIC;
    s_epic.hezip    = &s_ezip;

    if (HAL_EPIC_Init(&s_epic) != HAL_OK)
    {
        printf("[GPU] HAL_EPIC_Init failed\n");
        return -1;
    }

    s_gpu_ready = true;
    printf("[GPU] ePicasso ready (EPIC @ 0x%08lx)\n",
           (unsigned long)EPIC_BASE);
    return 0;
}

void gpu_deinit(void)
{
    if (!s_gpu_ready)
        return;

    /* Keep the clock running in active mode; no-op deinit for now. */
    s_gpu_ready = false;
}

void gpu_fill_rect(uint16_t *dst, int dst_w, int dst_h,
                   int x0, int y0, int x1, int y1, uint16_t color)
{
    EPIC_FillingCfgTypeDef fill;

    if (!s_gpu_ready || !dst)
        return;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= dst_w) x1 = dst_w - 1;
    if (y1 >= dst_h) y1 = dst_h - 1;
    if (x1 < x0 || y1 < y0)
        return;

    HAL_EPIC_FillDataInit(&fill);
    fill.start      = (uint8_t *)dst + (y0 * dst_w + x0) * sizeof(uint16_t);
    fill.color_mode = EPIC_COLOR_RGB565;
    fill.width      = (uint16_t)(x1 - x0 + 1);
    fill.height     = (uint16_t)(y1 - y0 + 1);
    fill.total_width = (uint16_t)dst_w;
    fill.color_r    = (uint8_t)(((color >> 11) & 0x1F) * 255 / 31);
    fill.color_g    = (uint8_t)(((color >> 5)  & 0x3F) * 255 / 63);
    fill.color_b    = (uint8_t)(((color)       & 0x1F) * 255 / 31);
    fill.alpha      = 0xFF;

    if (HAL_EPIC_FillStart(&s_epic, &fill) != HAL_OK)
    {
        printf("[GPU] fill failed (%d,%d)-(%d,%d)\n", x0, y0, x1, y1);
    }
}

void gpu_rotate_scale(uint16_t *src, int tile_w, int tile_h,
                      uint16_t *dst, int dst_w, int dst_h,
                      int center_x, int center_y,
                      float angle_deg, float scale)
{
    EPIC_BlendingDataType    fg, bg, out;
    EPIC_TransformCfgTypeDef rot;

    if (!s_gpu_ready || !src || !dst)
        return;

    /* Clamp center so the tile always intersects the framebuffer. */
    if (center_x < -tile_w) center_x = -tile_w;
    if (center_x > dst_w + tile_w) center_x = dst_w + tile_w;
    if (center_y < -tile_h) center_y = -tile_h;
    if (center_y > dst_h + tile_h) center_y = dst_h + tile_h;

    /* Foreground: the tile, positioned in dst coordinates. */
    HAL_EPIC_BlendDataInit(&fg);
    fg.data        = (uint8_t *)src;
    fg.color_mode  = EPIC_COLOR_RGB565;
    fg.width       = (uint16_t)tile_w;
    fg.height      = (uint16_t)tile_h;
    fg.total_width = (uint16_t)tile_w;
    fg.x_offset    = (int16_t)(center_x - tile_w / 2);
    fg.y_offset    = (int16_t)(center_y - tile_h / 2);

    /* Background = the existing framebuffer content. */
    HAL_EPIC_BlendDataInit(&bg);
    bg.data        = (uint8_t *)dst;
    bg.color_mode  = EPIC_COLOR_RGB565;
    bg.width       = (uint16_t)dst_w;
    bg.height      = (uint16_t)dst_h;
    bg.total_width = (uint16_t)dst_w;

    /* Output layer (same buffer as bg). */
    HAL_EPIC_BlendDataInit(&out);
    out.data        = (uint8_t *)dst;
    out.color_mode  = EPIC_COLOR_RGB565;
    out.width       = (uint16_t)dst_w;
    out.height      = (uint16_t)dst_h;
    out.total_width = (uint16_t)dst_w;

    /* Rotate around the tile center; CCW on screen = -angle in the
     * Y-down framebuffer space.  scale_x==scale_y<EPIC_INPUT_SCALE_NONE
     * means "scale up" (see bf0_hal_epic.h). */
    HAL_EPIC_RotDataInit(&rot);
    rot.angle    = (int16_t)(-angle_deg * 10.0f);
    rot.pivot_x  = (int16_t)(tile_w / 2);
    rot.pivot_y  = (int16_t)(tile_h / 2);

    if (scale > 0.01f)
    {
        float s = EPIC_INPUT_SCALE_NONE / scale;
        if (s < 1.0f) s = 1.0f;
        if (s > EPIC_VL_SCALE_RATIO_XPITCH_MAX)
        {
            s = EPIC_VL_SCALE_RATIO_XPITCH_MAX;
            printf("[GPU] scale clamp to %d\n", (int)s);
        }
        rot.scale_x = (uint32_t)s;
        rot.scale_y = (uint32_t)s;
    }
    else
    {
        rot.scale_x = EPIC_INPUT_SCALE_NONE;
        rot.scale_y = EPIC_INPUT_SCALE_NONE;
    }

    /* The caller (render_tile) has already cleaned the D-cache for the
     * source tile.  Run the hardware rotate+scale (polling mode). */
    if (HAL_EPIC_Rotate(&s_epic, &rot, &fg, &bg, &out, 0xFF) != HAL_OK)
    {
        printf("[GPU] rotate failed\n");
        return;
    }

    /* The destination framebuffer was written by the GPU, so invalidate
     * the stale lines before the CPU (or LCDC DMA) reads them again. */
    up_invalidate_dcache((uintptr_t)dst,
                         (uintptr_t)dst + (size_t)dst_w * dst_h * 2);
}

void gpu_blit(const uint16_t *src, int src_w, int src_h, int src_stride,
              uint16_t *dst, int dst_w, int dst_h, int dst_stride,
              int dst_x, int dst_y)
{
    EPIC_BlendingDataType s, d;

    if (!s_gpu_ready || !src || !dst)
        return;

    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    if (dst_x >= dst_w || dst_y >= dst_h)
        return;

    int w = src_w;
    int h = src_h;
    if (dst_x + w > dst_w) w = dst_w - dst_x;
    if (dst_y + h > dst_h) h = dst_h - dst_y;
    if (w <= 0 || h <= 0)
        return;

    HAL_EPIC_BlendDataInit(&s);
    s.data        = (uint8_t *)src;
    s.color_mode  = EPIC_COLOR_RGB565;
    s.width       = (uint16_t)w;
    s.height      = (uint16_t)h;
    s.total_width = (uint16_t)src_stride;

    HAL_EPIC_BlendDataInit(&d);
    d.data        = (uint8_t *)dst;
    d.color_mode  = EPIC_COLOR_RGB565;
    d.width       = (uint16_t)w;
    d.height      = (uint16_t)h;
    d.total_width = (uint16_t)dst_stride;
    d.x_offset    = (int16_t)dst_x;
    d.y_offset    = (int16_t)dst_y;

    up_clean_dcache((uintptr_t)src,
                    (uintptr_t)src + (size_t)src_stride * h * 2);

    if (HAL_EPIC_Copy_IT(&s_epic, &s, &d) != HAL_OK)
    {
        printf("[GPU] blit failed\n");
        return;
    }

    /* HAL_EPIC_Copy_IT is interrupt-driven and the app does not attach
     * the EPIC IRQ handler, so this entry point is not used by the
     * renderer; keep a bounded wait to avoid a hang if it is ever used. */
    for (uint32_t spin = 0; spin < 1000000 && s_epic.State == HAL_EPIC_STATE_BUSY; spin++)
    {
    }

    up_invalidate_dcache((uintptr_t)dst,
                         (uintptr_t)dst + (size_t)dst_stride * dst_h * 2);
}

#else /* !CONFIG_EXAMPLES_HUANGSHAN_RUNNING_GPU_ACCEL */

/* ------------------------------------------------------------------ *
 * Software fallback (no EPIC): the same API, implemented with memset /
 * nearest-neighbour transforms.  Used when the GPU is disabled or when
 * the HAL is not available in a given build.
 * ------------------------------------------------------------------ */

static void sw_fill(uint16_t *dst, int dst_w, int dst_h,
                    int x0, int y0, int x1, int y1, uint16_t color)
{
    int x, y;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= dst_w) x1 = dst_w - 1;
    if (y1 >= dst_h) y1 = dst_h - 1;
    if (x1 < x0 || y1 < y0)
        return;

    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++)
        {
            dst[y * dst_w + x] = color;
        }
    }
}

int gpu_init(void)
{
    printf("[GPU] Software renderer (no ePicasso)\n");
    return -1; /* caller keeps using the software path */
}

void gpu_deinit(void)
{
}

void gpu_fill_rect(uint16_t *dst, int dst_w, int dst_h,
                   int x0, int y0, int x1, int y1, uint16_t color)
{
    sw_fill(dst, dst_w, dst_h, x0, y0, x1, y1, color);
}

void gpu_rotate_scale(uint16_t *src, int tile_w, int tile_h,
                      uint16_t *dst, int dst_w, int dst_h,
                      int center_x, int center_y,
                      float angle_deg, float scale)
{
    float rad = angle_deg * 0.017453292519943295f;
    float c = cosf(rad);
    float s = sinf(rad);
    int sx, sy;
    int tx, ty;

    (void)center_x;
    (void)center_y;

    /* Inverse-map every destination pixel into the (rotated+scaled) tile. */
    for (ty = 0; ty < dst_h; ty++)
    {
        for (tx = 0; tx < dst_w; tx++)
        {
            float dx = (float)(tx - center_x) / scale;
            float dy = (float)(ty - center_y) / scale;
            float rx =  c * dx + s * dy;
            float ry = -s * dx + c * dy;
            sx = (int)(rx + tile_w / 2.0f);
            sy = (int)(ry + tile_h / 2.0f);

            if (sx >= 0 && sx < tile_w && sy >= 0 && sy < tile_h)
            {
                dst[ty * dst_w + tx] = src[sy * tile_w + sx];
            }
        }
    }
}

void gpu_blit(const uint16_t *src, int src_w, int src_h, int src_stride,
              uint16_t *dst, int dst_w, int dst_h, int dst_stride,
              int dst_x, int dst_y)
{
    int x, y;
    int w = src_w;
    int h = src_h;

    if (dst_x < 0) { w += dst_x; dst_x = 0; }
    if (dst_y < 0) { h += dst_y; dst_y = 0; }
    if (dst_x + w > dst_w) w = dst_w - dst_x;
    if (dst_y + h > dst_h) h = dst_h - dst_y;
    if (w <= 0 || h <= 0)
        return;

    for (y = 0; y < h; y++)
    {
        const uint16_t *sp = src + (size_t)y * src_stride;
        uint16_t *dp = dst + (size_t)(dst_y + y) * dst_stride + dst_x;
        memcpy(dp, sp, (size_t)w * sizeof(uint16_t));
    }
}

#endif /* CONFIG_EXAMPLES_HUANGSHAN_RUNNING_GPU_ACCEL */
