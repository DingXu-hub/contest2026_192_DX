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
static uint32_t               s_poll_word;   /* words consumed by the poller */
static bool                   s_poll_init;

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

    /* HAL_AUDPRC_Init() writes Init.clk_div into CFG.AUDCLK_DIV and
     * Init.adc_div/dac_div into STB.  A memset handle leaves all three at
     * zero - i.e. an ADC clock divider of zero - which is the prime suspect
     * for the "one short burst of zeros then nothing" we measured.
     * 48 MHz / 16 kHz = 3000 as a first rational guess. */
    s_prc.Init.clk_div = 1;
    s_prc.Init.adc_div = 3000;
    s_prc.Init.dac_div = 3000;
    s_prc.Init.clk_sel = 0;   /* 0: xtal 48M, 1: PLL 44.1M */

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

    /* HAL_DMA_Start_IT() did not leave the completion interrupts enabled in the
     * measured CCR, so set them explicitly: without TCIE/HTIE the transfer runs
     * but no callback ever fires (which is exactly what irqs=0 showed). */
    s_dma.Instance->CCR |= (1u << 1) | (1u << 2);
    printf("[MicPRC] dma CCR=%08lx CNDTR=%lu\n",
           (unsigned long)s_dma.Instance->CCR,
           (unsigned long)s_dma.Instance->CNDTR);
    usleep(20000);

    s_ring_w = s_ring_r = 0;
    s_stats_n = 0;
    s_stats_peak = 0;
    s_stats_sumsq = 0;
    s_irqs = 0;
    s_poll_word = 0;
    s_poll_init = false;
    s_running = true;

    printf("[MicPRC] started: src_sel=%d CFG=%08lx ADC_PATH=%08lx RX0_CFG=%08lx",
           src_sel, (unsigned long)hwp_audprc->CFG,
           (unsigned long)hwp_audprc->ADC_PATH_CFG0,
           (unsigned long)hwp_audprc->RX_CH0_CFG);
    puts("");
    return true;
}

/* Poll-based intake - deliberately independent of the DMA interrupt.
 *
 * The DMA keeps writing the circular buffer; the poller watches CNDTR (which
 * counts WORD transfers, two int16 samples each) to see how much is new and
 * copies it into the ring.  This is what lets capture work at all while the
 * interrupt wiring is unsolved: HAL_NVIC_EnableIRQ() is a raw CMSIS call, so
 * NuttX reports the channel's IRQ 66 as unexpected and no callback runs.  At
 * 16 kHz a 1024-sample buffer lasts 64 ms, so polling every 20 ms is safe. */
void mic_prc_poll(void)
{
    uint32_t words;
    uint32_t done;

    if (!s_running)
        return;

    /* words still to transfer; the buffer holds MIC_PRC_BUF_SAMPLES/2 words */
    done = (MIC_PRC_BUF_SAMPLES / 2) - s_dma.Instance->CNDTR;

    if (!s_poll_init)
    {
        s_poll_word = done;
        s_poll_init = true;
        return;
    }

    words = done - s_poll_word;          /* new words since the last poll */
    words %= (MIC_PRC_BUF_SAMPLES / 2);

    while (words--)
    {
        publish(&s_buf[2 * s_poll_word], 2);
        s_poll_word = (s_poll_word + 1) % (MIC_PRC_BUF_SAMPLES / 2);
    }
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
    int src, fmt, mode;
    int tried = 0;
    int hits = 0;

    for (src = 0; src < 2; src++)
    for (fmt = 0; fmt < 2; fmt++)
    for (mode = 0; mode < 2; mode++)
    {
        uint32_t cfg0;
        int nz = 0;
        int32_t pk = 0;
        int i;

        if (!mic_prc_start(src))
            continue;

        /* poke the RX format/mode bits the HAL never touches */
        cfg0 = hwp_audprc->RX_CH0_CFG;
        cfg0 &= ~(AUDPRC_RX_CH0_CFG_FORMAT_Msk | AUDPRC_RX_CH0_CFG_MODE_Msk);
        cfg0 |= ((uint32_t)fmt << AUDPRC_RX_CH0_CFG_FORMAT_Pos) |
                ((uint32_t)mode << AUDPRC_RX_CH0_CFG_MODE_Pos);
        hwp_audprc->RX_CH0_CFG = cfg0;

        for (i = 0; i < 12; i++)
            usleep(10000);

        mic_prc_peek(&nz, &pk, NULL, NULL);
        printf("[MicPRCSWEEP] src=%d fmt=%d mode=%d RX_CFG=%08lx CCR=%08lx "
               "CNDTR=%lu nz=%d peak=%ld irqs=%lu\n",
               src, fmt, mode, (unsigned long)hwp_audprc->RX_CH0_CFG,
               (unsigned long)s_dma.Instance->CCR,
               (unsigned long)s_dma.Instance->CNDTR, nz, (long)pk,
               (unsigned long)s_irqs);
        usleep(40000);
        if (nz > 0 || pk > 0)
            hits++;
        mic_prc_stop();
        tried++;
    }
    printf("[MicPRCSWEEP] %d combinations, %d produced non-zero data", tried, hits);
    puts("");
}

/* Microphone-bias sweep, driven by the module datasheet: MIC_BIAS is a
 * programmable LDO (1.4-2.8 V) whose level lives in BG_CFG0.MIC_VREF_SEL, with
 * ADC_ANA_CFG.CAPCODE choosing the bias decoupling capacitor and
 * BG_CFG0.EN_AMP the microphone amplifier.  Our firmware only ever enabled
 * those blocks and never picked a level, so walk the levels while the AUDPRC
 * capture path runs and judge by the raw DMA buffer (no interrupts involved). */
void mic_prc_sweep_bias(void)
{
    static const uint8_t vrefs[] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const uint8_t caps[]  = {3, 8, 16, 31};
    static const uint8_t amps[]  = {0, 1};
    unsigned v, c, a;
    int hits = 0;
    int tried = 0;

    for (v = 0; v < sizeof(vrefs); v++)
    for (c = 0; c < sizeof(caps); c++)
    for (a = 0; a < sizeof(amps); a++)
    {
        int nz = 0;
        int32_t pk = 0;
        int i;

        hwp_audcodec->BG_CFG0 = (hwp_audcodec->BG_CFG0 &
                                 ~AUDCODEC_BG_CFG0_MIC_VREF_SEL_Msk) |
                                ((uint32_t)vrefs[v]
                                 << AUDCODEC_BG_CFG0_MIC_VREF_SEL_Pos);
        if (amps[a])
            hwp_audcodec->BG_CFG0 |= AUDCODEC_BG_CFG0_EN_AMP;
        else
            hwp_audcodec->BG_CFG0 &= ~AUDCODEC_BG_CFG0_EN_AMP;
        hwp_audcodec->ADC_ANA_CFG = (hwp_audcodec->ADC_ANA_CFG &
                                     ~AUDCODEC_ADC_ANA_CFG_CAPCODE_Msk) |
                                    ((uint32_t)caps[c]
                                     << AUDCODEC_ADC_ANA_CFG_CAPCODE_Pos) |
                                    AUDCODEC_ADC_ANA_CFG_MICBIAS_EN;

        if (!mic_prc_start(0))
        {
            printf("[MicBIAS] vref=%u cap=%u amp=%u START FAILED\n",
                   vrefs[v], caps[c], amps[a]);
            usleep(30000);
            continue;
        }
        for (i = 0; i < 8; i++)
            usleep(10000);
        mic_prc_peek(&nz, &pk, NULL, NULL);
        printf("[MicBIAS] vref=%u cap=%u amp=%u BG0=%08lx ANA=%08lx nz=%d peak=%ld\n",
               vrefs[v], caps[c], amps[a],
               (unsigned long)hwp_audcodec->BG_CFG0,
               (unsigned long)hwp_audcodec->ADC_ANA_CFG, nz, (long)pk);
        usleep(30000);
        if (nz > 0 || pk > 0)
            hits++;
        mic_prc_stop();
        tried++;
    }
    printf("[MicBIAS] %d combinations, %d produced non-zero data\n", tried, hits);
    usleep(30000);
}
