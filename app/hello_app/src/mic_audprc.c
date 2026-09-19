/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mic_audprc.c - capture through AUDPRC (the audio processor), not the
 * AUDCODEC channel FIFO.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The vendor's own working AI-voice product on the same silicon configures
 * exactly this combination (OpenSiFli/xiaozhi-sf32,
 * app/boards/sf32lb52-xty-ai/hcpu/board.conf):
 *
 *     CONFIG_BSP_ENABLE_AUD_CODEC=y     (the internal AUDCODEC - same as ours)
 *     CONFIG_BSP_AUDPRC_RX0_DMA=y       (capture)
 *     CONFIG_BSP_AUDPRC_TX0_DMA=y       (playback)
 *
 * On this chip the record path is  AUDCODEC ADC -> AUDPRC ADC path -> RX_CH0
 * FIFO -> DMA(AUDPRC_RX0).  Every earlier attempt armed the AUDCODEC's *own*
 * channel FIFO (ADC_CH0_ENTRY -> DMAC1_CH4 with request 39) which is a
 * different route: the block was alive, the PLL ran, the DMA was armed, all
 * 1184+32 register combinations were correct - and no sample ever appeared,
 * because the samples do not go that way.
 *
 * The register definitions and the register-level sequence follow the vendor
 * HAL (chips/drivers/hal/bf0_hal_audprc.c, cmsis/sf32lb52x/audprc.h); the ROM
 * provides the HAL entry points on this part, so only the caller-side wiring
 * lives here.
 */

#include "mic_audprc.h"

#include "bf0_hal.h"

#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* DMA wiring for AUDPRC RX0, copied from the vendor header
 * chips/boards/include/config/sf32lb52x/dma_config.h (the app does not get that
 * include path, so the values are written out as our AUDCODEC driver does) */
#define MIC_PRC_DMA_INSTANCE   DMA1_Channel4      /* AUDPRC_RX0_DMA_INSTANCE */
#define MIC_PRC_DMA_REQUEST    DMA_REQUEST_53     /* AUDPRC_RX0_DMA_REQUEST  */
#define MIC_PRC_DMA_IRQ        DMAC1_CH4_IRQn     /* AUDPRC_RX0_DMA_IRQ      */

#define MIC_PRC_BUF_SAMPLES    1024
#define MIC_PRC_RING_SAMPLES   8192

static int16_t               s_buf[MIC_PRC_BUF_SAMPLES];
static int16_t               s_ring[MIC_PRC_RING_SAMPLES];
static volatile uint32_t     s_ring_w;
static volatile uint32_t     s_ring_r;
static volatile uint32_t     s_stats_n;
static volatile int32_t      s_stats_peak;
static volatile int64_t      s_stats_sumsq;
static volatile uint32_t     s_irqs;
static bool                   s_running;
static int                    s_src_sel;

static AUDPRC_HandleTypeDef  s_prc;
static DMA_HandleTypeDef     s_dma;

/* ---------------- sample intake (same shape as the AUDCODEC driver) ------ */

static void publish(const int16_t *s, uint32_t n)
{
    uint32_t i;
    int32_t peak = s_stats_peak;

    for (i = 0; i < n; i++)
    {
        int32_t v = s[i];
        int32_t a = v < 0 ? -v : v;

        if (a > peak)
            peak = a;
        s_stats_sumsq += (int64_t)v * (int64_t)v;
        s_ring[s_ring_w] = (int16_t)v;
        s_ring_w = (s_ring_w + 1) % MIC_PRC_RING_SAMPLES;
    }
    s_stats_n += n;
    s_stats_peak = peak;
}

void HAL_AUDPRC_RxHalfCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
    (void)haprc;
    (void)cid;
    if (s_running)
        publish(s_buf, MIC_PRC_BUF_SAMPLES / 2);
    s_irqs++;
}

void HAL_AUDPRC_RxCpltCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
    (void)haprc;
    (void)cid;
    if (s_running)
        publish(s_buf + MIC_PRC_BUF_SAMPLES / 2, MIC_PRC_BUF_SAMPLES / 2);
    s_irqs++;
}

void HAL_AUDPRC_ErrorCallback(AUDPRC_HandleTypeDef *haprc, int cid)
{
    (void)haprc;
    (void)cid;
    puts("[MicPRC] dma error");
}

static int mic_prc_dma_irq(int irq, void *context, void *arg)
{
    (void)irq;
    (void)context;
    (void)arg;
    HAL_DMA_IRQHandler(&s_dma);
    return 0;
}

/* ---------------- start / stop -------------------------------------------- */

bool mic_prc_start(int src_sel)
{
    AUDPRC_ADCCfgTypeDef cfg;

    mic_prc_stop();
    s_src_sel = src_sel;

    /* 1) un-gate and reset the audio processor (the codec itself is brought up
     *    by mic_start(), which the caller runs first) */
    HAL_RCC_EnableModule(RCC_MOD_AUDPRC);
    printf("[MicPRC] after RCC: ID=%08lx CFG=%08lx RX0_CFG=%08lx STB=%08lx",
           (unsigned long)hwp_audprc->ID, (unsigned long)hwp_audprc->CFG,
           (unsigned long)hwp_audprc->RX_CH0_CFG,
           (unsigned long)hwp_audprc->STB);
    puts("");

    HAL_RCC_ResetModule(RCC_MOD_AUDPRC);
    printf("[MicPRC] after reset: CFG=%08lx", (unsigned long)hwp_audprc->CFG);
    puts("");

    /* 2) DMA handle for the RX0 channel */
    memset(&s_dma, 0, sizeof(s_dma));
    memset(&s_prc, 0, sizeof(s_prc));
    s_dma.Instance     = MIC_PRC_DMA_INSTANCE;
    s_dma.Init.Request = MIC_PRC_DMA_REQUEST;
    s_prc.Instance     = hwp_audprc;
    s_prc.hdma[HAL_AUDPRC_RX_CH0] = &s_dma;

    if (HAL_AUDPRC_Init(&s_prc) != HAL_OK)
    {
        puts("[MicPRC] HAL_AUDPRC_Init failed");
        return false;
    }
    printf("[MicPRC] Init ok: CFG=%08lx", (unsigned long)hwp_audprc->CFG);
    puts("");

    /* 3) ADC path: route the codec ADC in, 0 dB, no loopback / swap */
    memset(&cfg, 0, sizeof(cfg));
    cfg.src_sel        = (uint8_t)src_sel;   /* 0/1: which source feeds RX   */
    cfg.src_ch_en      = 1;
    cfg.data_swap      = 0;
    cfg.rx2tx_loopback = 0;
    cfg.vol_l          = 0;
    cfg.vol_r          = 0;
    if (HAL_AUDPRC_Config_ADCPath(&s_prc, &cfg) != HAL_OK)
    {
        puts("[MicPRC] Config_ADCPath failed");
        return false;
    }
    printf("[MicPRC] ADCPath ok: ADC_PATH_CFG0=%08lx",
           (unsigned long)hwp_audprc->ADC_PATH_CFG0);
    puts("");

    /* 4) power the processor and its ADC path on */
    __HAL_AUDPRC_ENABLE(&s_prc);
    __HAL_AUDPRC_ADCPATH_ENABLE(&s_prc);
    printf("[MicPRC] enabled: CFG=%08lx", (unsigned long)hwp_audprc->CFG);
    puts("");

    /* 5) circular DMA out of the RX_CH0 FIFO */
    if (HAL_AUDPRC_Receive_DMA(&s_prc, (uint8_t *)s_buf,
                               sizeof(s_buf), HAL_AUDPRC_RX_CH0) != HAL_OK)
    {
        puts("[MicPRC] Receive_DMA failed");
        return false;
    }

    irq_attach(MIC_PRC_DMA_IRQ, mic_prc_dma_irq, NULL);
    up_enable_irq(MIC_PRC_DMA_IRQ);

    s_ring_w = s_ring_r = 0;
    s_stats_n = 0;
    s_stats_peak = 0;
    s_stats_sumsq = 0;
    s_irqs = 0;
    s_running = true;

    printf("[MicPRC] started: src_sel=%d CFG=%08lx ADC_PATH=%08lx RX0_CFG=%08lx",
           src_sel, (unsigned long)hwp_audprc->CFG,
           (unsigned long)hwp_audprc->ADC_PATH_CFG0,
           (unsigned long)hwp_audprc->RX_CH0_CFG);
    puts("");
    return true;
}

void mic_prc_stop(void)
{
    if (s_running)
    {
        HAL_AUDPRC_DMAStop(&s_prc, HAL_AUDPRC_RX_CH0);
        s_running = false;
    }
    up_disable_irq(MIC_PRC_DMA_IRQ);
}

bool mic_prc_running(void)
{
    return s_running;
}

void mic_prc_stats(uint32_t *samples, int32_t *peak, int32_t *rms,
                   uint32_t *irqs, uint32_t *cndtr)
{
    uint32_t n = s_stats_n;

    if (samples)
        *samples = n;
    if (peak)
        *peak = s_stats_peak;
    if (rms)
        *rms = n ? (int32_t)__builtin_sqrt((double)(s_stats_sumsq / (int64_t)n))
                 : 0;
    if (irqs)
        *irqs = s_irqs;
    if (cndtr)
        *cndtr = (uint32_t)s_dma.Instance->CNDTR;
}

/* Read the raw DMA buffer directly - independent of the DMA interrupt, so this
 * distinguishes "the transfer runs but our IRQ is not wired" from "no data". */
void mic_prc_peek(int *nonzero, int32_t *peak, int32_t *first, int32_t *last)
{
    int i;
    int nz = 0;
    int32_t pk = 0;

    for (i = 0; i < MIC_PRC_BUF_SAMPLES; i++)
    {
        int32_t v = s_buf[i];
        int32_t a = v < 0 ? -v : v;

        if (v != 0)
            nz++;
        if (a > pk)
            pk = a;
    }
    if (nonzero)
        *nonzero = nz;
    if (peak)
        *peak = pk;
    if (first)
        *first = s_buf[0];
    if (last)
        *last = s_buf[MIC_PRC_BUF_SAMPLES - 1];
}

int mic_prc_read(int16_t *out, int max_samples)
{
    int n = 0;

    while (n < max_samples && s_ring_r != s_ring_w)
    {
        out[n++] = s_ring[s_ring_r];
        s_ring_r = (s_ring_r + 1) % MIC_PRC_RING_SAMPLES;
    }
    return n;
}

/* Walk the ADC source selector on the hardware and report which one (if any)
 * makes the RX channel produce data - the same measure-don't-guess approach
 * that finally localised the AUDCODEC problem. */
void mic_prc_sweep(void)
{
    int s;
    int hits = 0;

    for (s = 0; s < 4; s++)
    {
        uint32_t n0 = s_stats_n;
        uint32_t i0 = s_irqs;
        uint32_t c0;
        uint32_t c1;
        int i;

        if (!mic_prc_start(s))
            continue;
        c0 = (uint32_t)s_dma.Instance->CNDTR;
        for (i = 0; i < 20; i++)
        {
            usleep(10000);
        }
        c1 = (uint32_t)s_dma.Instance->CNDTR;
        printf("[MicPRCSWEEP] src_sel=%d cndtr %lu->%lu irqs %lu->%lu samples %lu->%lu",
               s, (unsigned long)c0, (unsigned long)c1, (unsigned long)i0,
               (unsigned long)s_irqs, (unsigned long)n0,
               (unsigned long)s_stats_n);
        puts("");
        {
            int nz;
            int32_t pk;

            mic_prc_peek(&nz, &pk, NULL, NULL);
            printf("[MicPRCSWEEP]   buf nonzero=%d peak=%ld", nz, (long)pk);
            puts("");
            if (nz > 0 || pk > 0 || c1 != c0 || s_stats_n != n0 || s_irqs != i0)
                hits++;
        }
        mic_prc_stop();
    }
    printf("[MicPRCSWEEP] %d source selectors tried, %d produced data", 4, hits);
    puts("");
}
