/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mic_audcodec.c - MEMS microphone capture through the internal AUDCODEC.
 *
 * Flow (mirrors the vendor SDK's documented RX sample):
 *   updata_pll_freq(0) -> HAL_TURN_ON_PLL()      codec PLL (49.152 MHz)
 *   HAL_AUDCODEC_Init()                          + DMA handle for ADC0
 *   HAL_AUDCODEC_Config_RChanel(0, &adc_cfg)     16-bit, HPF on
 *   HAL_AUDCODEC_Receive_DMA(buf, size, ADC_CH0) circular DMA, half/full IRQ
 *   HAL_AUCODEC_Refgen_Init(), Config_Analog_ADCPath(), LP_ENABLE()
 *
 * The half/full callbacks copy the finished half-buffer into a software
 * ring buffer and update the RMS/peak statistics; the app (or the PC over
 * the framed link) drains it with mic_read().
 */

#include <nuttx/config.h>

#include "mic_audcodec.h"

#include "bf0_hal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

/* present in the HAL object but not declared in bf0_hal_audcodec.h */
extern HAL_StatusTypeDef HAL_AUDCODEC_Config_ADCPath_Volume(
    AUDCODEC_HandleTypeDef *hacodec, int channel, int volume);
extern int bf0_enable_pll(uint32_t freq, uint8_t type);

/* ------------------------------------------------------------------ *
 * bring-up experiment switches
 *
 * The official HAL leaves several steps to the caller (MspInit is empty and
 * the ADC-path configuration is commented out of HAL_AUDCODEC_Init), so the
 * only way to find which combination this board needs is to try them on the
 * hardware.  `!mic try [n]` walks these variants and reports which one makes
 * the ADC produce samples.  See docs/麦克风硬件与实现方案.md.
 * ------------------------------------------------------------------ */
static struct {
    bool en_dly3;         /* CFG |= 3 << ADC_EN_DLY_SEL (vendor sets it for DAC)  */
    bool pll_full;        /* bf0_enable_pll() instead of updata_pll_freq()+HAL_TURN_ON_PLL() */
    bool analog_late;     /* analogue ADC path after the digital enable           */
    bool vendor_adc_cfg;  /* also call the vendor Config_ADCPath()                */
} s_try;

/* DMA wiring for AUDCODEC ADC0, copied from the vendor header
 * vendor/sifli/chips/boards/include/config/sf32lb52x/dma_config.h (the app
 * does not get that include path). */
#define MIC_DMA_INSTANCE   DMA1_Channel4
#define MIC_DMA_REQUEST    DMA_REQUEST_39
#define MIC_DMA_IRQ        DMAC1_CH4_IRQn
#define MIC_DMA_IRQ_NAME   DMAC1_CH4_IRQHandler

#define MIC_DMA_BYTES      4096          /* 2 x 1024 samples of 16 bit     */
#define MIC_RING_SAMPLES   4096          /* software ring (8 KB of samples) */

static AUDCODEC_HandleTypeDef  s_codec;
static DMA_HandleTypeDef       s_dma;
static AUDCODE_ADC_CLK_CONFIG_TYPE s_adc_clk;
static mic_cfg_t               s_cfg;
static volatile bool           s_running;
static volatile uint32_t       s_half_irqs;
static void (*s_activity)(void);   /* keep-awake hook (power manager) */

static uint8_t                 s_dma_buf[MIC_DMA_BYTES] __attribute__((aligned(4)));
static volatile int16_t        s_ring[MIC_RING_SAMPLES];
static volatile uint32_t       s_ring_w;      /* write index (samples) */
static uint32_t                s_ring_r;      /* read index (samples)  */

/* statistics */
static volatile uint32_t       s_stats_n;
static volatile int32_t        s_stats_peak;
static volatile int64_t        s_stats_sumsq;

void mic_cfg_default(mic_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate        = 16000;
    cfg->pll_type           = 0;    /* 16k/48k series -> 49.152 MHz PLL */
    cfg->clk_src_sel        = 1;    /* use the codec PLL                */
    cfg->osr_sel            = 1;    /* 300 (per HAL comment)            */
    cfg->clk_div            = 10;   /* 48M / (300 * 10) = 16 kHz        */
    cfg->opmode             = 0;
    cfg->sel_clk_adc_source = 0;
    cfg->sel_clk_adc        = 1;
    cfg->diva_clk_adc       = 5;
    cfg->fsp                = 0;
    cfg->rough_vol          = 0xa;
    cfg->channel            = 0;
}

mic_cfg_t *mic_cfg(void)
{
    return &s_cfg;
}

/* the app installs a keep-awake callback (the power manager gates the
 * codec clock in IDLE/SLEEP, so every capture path reports activity) */
void mic_set_activity_hook(void (*fn)(void))
{
    s_activity = fn;
}

static inline void report_activity(void)
{
    if (s_activity)
        s_activity();
}

bool mic_running(void)
{
    return s_running;
}

/* ---------------- sample intake ---------------- */

static void publish(const uint8_t *base, uint32_t bytes)
{
    const int16_t *s = (const int16_t *)base;
    uint32_t n = bytes / 2;
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
        s_ring_w = (s_ring_w + 1) % MIC_RING_SAMPLES;
    }
    s_stats_n += n;
    s_stats_peak = peak;
}

/* the HAL calls these from its DMA ISR context */
void HAL_AUDCODEC_RxHalfCpltCallback(AUDCODEC_HandleTypeDef *hacodec, int cid)
{
    (void)hacodec;
    (void)cid;
    if (s_running)
        publish(s_dma_buf, MIC_DMA_BYTES / 2);
    s_half_irqs++;
    report_activity();
}

void HAL_AUDCODEC_RxCpltCallback(AUDCODEC_HandleTypeDef *hacodec, int cid)
{
    (void)hacodec;
    (void)cid;
    if (s_running)
        publish(s_dma_buf + MIC_DMA_BYTES / 2, MIC_DMA_BYTES / 2);
    s_half_irqs++;
    report_activity();
}

/* the DMA channel IRQ (DMAC1_CH4) */
int MIC_DMA_IRQ_NAME(int irq, void *context, void *arg);

int MIC_DMA_IRQ_NAME(int irq, void *context, void *arg)
{
    (void)irq;
    (void)context;
    (void)arg;
    HAL_DMA_IRQHandler(&s_dma);
    return 0;
}

/* ---------------- start / stop ---------------- */

bool mic_start(const mic_cfg_t *cfg)
{
    if (cfg)
        s_cfg = *cfg;
    else if (s_cfg.sample_rate == 0)
        mic_cfg_default(&s_cfg);      /* first call: sensible defaults */

    mic_stop();

    /* the HAL's MspInit is empty, so the caller must un-gate the blocks:
     * without these the codec registers accept writes but nothing runs
     * (measured: DMA IRQs stayed 0 for every clock configuration) */
    /* NOTE: RCC_MOD_AUDPRC (bit 20, the audio data-path gate) was tried here
     * as well - it does not make the ADC convert, and with it enabled the
     * whole codec register block started reading back the bus magic pattern
     * 0xc0dec000 when the system idles, so it is left alone. */
#if defined(SF32LB52X)
    HAL_RCC_EnableModule(RCC_MOD_AUDCODEC);
#else
    HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_HP);
    HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_LP);
#endif
    HAL_RCC_EnableModule(RCC_MOD_DMAC1);

    /* the mic input is the codec's analogue pin: park the digital driver
     * (the reference board does the same: "PAD_PA09 ... share with MIC") */
    HAL_PIN_Set(PAD_PA09, GPIO_A9, PIN_NOPULL, 1);

    /* codec PLL: 49.152 MHz for the 16 kHz (48 k family) sample rates */
    if (s_try.pll_full)
    {
        /* the vendor's full sequence: HXT audio buffer, bandgap, PLL config,
         * VCO calibration loop and the lock check */
        int r = bf0_enable_pll(49152000u, (uint8_t)s_cfg.pll_type);
        printf("[Mic] bf0_enable_pll -> %d\n", r);
    }
    else
    {
        if (updata_pll_freq(s_cfg.pll_type) != 0)
            printf("[Mic] pll lock failed for type %u\n", s_cfg.pll_type);
        HAL_TURN_ON_PLL();
    }

    memset(&s_adc_clk, 0, sizeof(s_adc_clk));
    s_adc_clk.samplerate         = s_cfg.sample_rate;
    s_adc_clk.clk_src_sel        = s_cfg.clk_src_sel;
    s_adc_clk.clk_div            = s_cfg.clk_div;
    s_adc_clk.osr_sel            = s_cfg.osr_sel;
    s_adc_clk.sel_clk_adc_source = s_cfg.sel_clk_adc_source;
    s_adc_clk.sel_clk_adc        = s_cfg.sel_clk_adc;
    s_adc_clk.diva_clk_adc       = s_cfg.diva_clk_adc;
    s_adc_clk.fsp                = s_cfg.fsp;

    memset(&s_codec, 0, sizeof(s_codec));
    memset(&s_dma, 0, sizeof(s_dma));

    s_dma.Instance       = MIC_DMA_INSTANCE;
    /* ADC1 shares the codec DMA controller but uses request 40; the digital
     * channel decides which of the two analogue front-ends is read */
    s_dma.Init.Request   = (s_cfg.channel == 1) ? DMA_REQUEST_40
                                                : MIC_DMA_REQUEST;
    /* SF32LB52X has a single AUDCODEC register block (the _HP/_LP pair only
     * exists on 56x/58x), so only Instance is needed */
    s_codec.Instance     = hwp_audcodec;
    s_codec.hdma[(s_cfg.channel == 1) ? HAL_AUDCODEC_ADC_CH1
                                      : HAL_AUDCODEC_ADC_CH0] = &s_dma;

    s_codec.Init.samplerate_index = 0;
    s_codec.Init.adc_cfg.opmode   = s_cfg.opmode;
    s_codec.Init.adc_cfg.adc_clk  = &s_adc_clk;

    if (HAL_AUDCODEC_Init(&s_codec) != HAL_OK)
    {
        printf("[Mic] HAL_AUDCODEC_Init failed\n");
        return false;
    }

    s_ring_w = s_ring_r = 0;
    mic_stats_reset();
    s_half_irqs = 0;

    /* reset the ADC path first (the vendor sequence does this before
     * configuring a channel) */
    HAL_AUDCODEC_Clear_All_Channel(&s_codec, 0x2);

    if (HAL_AUDCODEC_Config_RChanel(&s_codec, s_cfg.channel,
                                    &s_codec.Init.adc_cfg) != HAL_OK)
    {
        printf("[Mic] Config_RChanel failed\n");
        return false;
    }

    if (s_try.vendor_adc_cfg)
    {
        /* The vendor's HAL_AUDCODEC_Config_ADCPath() is not linked for this
         * chip (it lives in the excluded bf0_hal_audcodec.c), so replicate its
         * register write verbatim - it writes the ADC clock fields into CFG,
         * not into ADC_CFG.  The experiment is to see whether that is what the
         * hardware actually wants. */
        uint32_t value =
            MAKE_REG_VAL(s_cfg.clk_div, AUDCODEC_ADC_CFG_CLK_DIV_Msk,
                         AUDCODEC_ADC_CFG_CLK_DIV_Pos)
            | MAKE_REG_VAL(s_cfg.clk_src_sel, AUDCODEC_ADC_CFG_CLK_SRC_SEL_Msk,
                           AUDCODEC_ADC_CFG_CLK_SRC_SEL_Pos)
            | MAKE_REG_VAL(s_cfg.opmode, AUDCODEC_ADC_CFG_OP_MODE_Msk,
                           AUDCODEC_ADC_CFG_OP_MODE_Pos)
            | MAKE_REG_VAL(s_cfg.osr_sel, AUDCODEC_ADC_CFG_OSR_SEL_Msk,
                          AUDCODEC_ADC_CFG_OSR_SEL_Pos);

        hwp_audcodec->CFG = value;
        printf("[Mic] vendor-style CFG write = %08lx\n", (unsigned long)value);
    }

    if (HAL_AUDCODEC_Receive_DMA(&s_codec, s_dma_buf, sizeof(s_dma_buf),
                                 (s_cfg.channel == 1) ? HAL_AUDCODEC_ADC_CH1
                                                      : HAL_AUDCODEC_ADC_CH0) != HAL_OK)
    {
        printf("[Mic] Receive_DMA failed\n");
        return false;
    }

    /* 0 dB ADC path (the HAL's default leaves the volume from the last use) */
    HAL_AUDCODEC_Config_ADCPath_Volume(&s_codec, s_cfg.channel, 0);

    /* The HAL starts the DMA inside Receive_DMA but throws the status away,
     * and the channel was measured with CCR.EN = 0 afterwards (so nothing ever
     * moved).  Kick it here as well and report what the registers say - if the
     * start refuses, this is our bug, not the codec's. */
    {
        HAL_StatusTypeDef st = HAL_DMA_Start_IT(
            &s_dma,
            (uint32_t)&hwp_audcodec->ADC_CH0_ENTRY,
            (uint32_t)s_dma_buf, MIC_DMA_BYTES / 4);

        printf("[MicDMA] Start_IT -> %d ccr=%08lx cndtr=%lu\n", (int)st,
               (unsigned long)s_dma.Instance->CCR,
               (unsigned long)s_dma.Instance->CNDTR);

        /* Watch the transfer from inside the start path: if CNDTR decrements
         * the codec IS converting, and the earlier "no data" readings were an
         * artefact of reading the registers after the PM gated the clocks. */
        {
            uint32_t c0 = s_dma.Instance->CNDTR;
            uint32_t e0 = hwp_audcodec->ADC_CH0_ENTRY;
            uint32_t n0 = s_stats_n;

            usleep(150000);
            printf("[MicLIVE] +150 ms: cndtr %lu->%lu entry %08lx->%08lx "
                   "samples %lu->%lu irq=%lu\n",
                   (unsigned long)c0, (unsigned long)s_dma.Instance->CNDTR,
                   (unsigned long)e0,
                   (unsigned long)hwp_audcodec->ADC_CH0_ENTRY,
                   (unsigned long)n0, (unsigned long)s_stats_n,
                   (unsigned long)s_half_irqs);
        }
    }

    HAL_NVIC_SetPriority(MIC_DMA_IRQ, 1, 0);
    HAL_NVIC_EnableIRQ(MIC_DMA_IRQ);

    HAL_AUCODEC_Refgen_Init();
    if (!s_try.analog_late)
        HAL_AUDCODEC_Config_Analog_ADCPath(&s_adc_clk);

    /* 52x uses the single block: enable the ADC path (the 56x/58x "LP"
     * variants of these macros do not exist here) */
    __HAL_AUDCODEC_ADC_ENABLE(&s_codec);

    if (s_try.analog_late)
        HAL_AUDCODEC_Config_Analog_ADCPath(&s_adc_clk);
    if (s_try.en_dly3)
        hwp_audcodec->CFG |= (3 << AUDCODEC_CFG_ADC_EN_DLY_SEL_Pos);

    /* Dump the analogue side right here: the codec register block reads back
     * the bus magic pattern once the PM gates its clock, so an outside
     * `!mic regs` a second later shows nothing useful. */
    printf("[MicCFG] CFG=%08lx ADC_CFG=%08lx CH0=%08lx ANA=%08lx REFGEN=%08lx\n",
           (unsigned long)hwp_audcodec->CFG,
           (unsigned long)hwp_audcodec->ADC_CFG,
           (unsigned long)hwp_audcodec->ADC_CH0_CFG,
           (unsigned long)hwp_audcodec->ADC_ANA_CFG,
           (unsigned long)hwp_audcodec->REFGEN_CFG);
    printf("[MicCFG] ADC1_1=%08lx ADC1_2=%08lx ADC2_1=%08lx ADC2_2=%08lx "
           "PLL5=%08lx PLL6=%08lx PLL0=%08lx STAT=%08lx BG0=%08lx\n",
           (unsigned long)hwp_audcodec->ADC1_CFG1,
           (unsigned long)hwp_audcodec->ADC1_CFG2,
           (unsigned long)hwp_audcodec->ADC2_CFG1,
           (unsigned long)hwp_audcodec->ADC2_CFG2,
           (unsigned long)hwp_audcodec->PLL_CFG5,
           (unsigned long)hwp_audcodec->PLL_CFG6,
           (unsigned long)hwp_audcodec->PLL_CFG0,
           (unsigned long)hwp_audcodec->PLL_STAT,
           (unsigned long)hwp_audcodec->BG_CFG0);

    s_running = true;
    report_activity();
    printf("[Mic] started: rate=%lu src=%u osr=%u div=%u op=%u vol=%u\n",
           (unsigned long)s_cfg.sample_rate, s_cfg.clk_src_sel, s_cfg.osr_sel,
           s_cfg.clk_div, s_cfg.opmode, s_cfg.rough_vol);
    return true;
}

void mic_stop(void)
{
    if (!s_running)
        return;

    __HAL_AUDCODEC_ADC_DISABLE(&s_codec);
    HAL_AUDCODEC_DMAStop(&s_codec, (s_cfg.channel == 1) ? HAL_AUDCODEC_ADC_CH1
                                                       : HAL_AUDCODEC_ADC_CH0);
    HAL_NVIC_DisableIRQ(MIC_DMA_IRQ);
    HAL_AUDCODEC_Close_Analog_ADCPath();
    HAL_TURN_OFF_PLL();
    s_running = false;
    printf("[Mic] stopped (hw irqs=%lu)\n", (unsigned long)s_half_irqs);
}

int mic_read(int16_t *out, int max_samples)
{
    int n = 0;

    while (n < max_samples && s_ring_r != s_ring_w)
    {
        out[n++] = s_ring[s_ring_r];
        s_ring_r = (s_ring_r + 1) % MIC_RING_SAMPLES;
    }
    return n;
}

void mic_stats_reset(void)
{
    s_stats_n = 0;
    s_stats_peak = 0;
    s_stats_sumsq = 0;
}

/* Run one bring-up variant end-to-end and report whether it produced data.
 * Returns 1 = samples arrived, 0 = no data, -1 = start refused. */
int mic_try(int variant)
{
    mic_cfg_t cfg;
    uint32_t n = 0;
    int32_t peak = 0, rms = 0;
    uint32_t entry0, entry1;
    struct timeval tv0, tv1;

    memset(&s_try, 0, sizeof(s_try));
    switch (variant)
    {
    case 1: s_try.en_dly3 = true; break;
    case 2: s_try.pll_full = true; break;
    case 3: s_try.analog_late = true; break;
    case 4: s_try.vendor_adc_cfg = true; break;
    case 5: s_try.en_dly3 = s_try.pll_full = s_try.analog_late = true; break;
    case 6: s_try.en_dly3 = s_try.pll_full = s_try.analog_late =
            s_try.vendor_adc_cfg = true; break;
    default: break;                       /* 0 = current baseline */
    }

    mic_cfg_default(&cfg);
    entry0 = (uint32_t)hwp_audcodec->ADC_CH0_ENTRY;
    if (!mic_start(&cfg))
    {
        printf("[MicTRY] v%d start failed\n", variant);
        return -1;
    }

    gettimeofday(&tv0, NULL);
    do
    {
        usleep(50000);
        mic_stats(&n, &peak, &rms);
        gettimeofday(&tv1, NULL);
    }
    while (n == 0 && (tv1.tv_sec - tv0.tv_sec) * 1000 +
                     (tv1.tv_usec - tv0.tv_usec) / 1000 < 500);

    entry1 = (uint32_t)hwp_audcodec->ADC_CH0_ENTRY;
    printf("[MicTRY] v%d en_dly=%d pll_full=%d analog_late=%d vendor_cfg=%d "
           "-> samples=%lu peak=%ld rms=%ld entry %08lx->%08lx CFG=%08lx "
           "RCC_ENR2=%08lx RSTR2=%08lx\n",
           variant, (int)s_try.en_dly3, (int)s_try.pll_full,
           (int)s_try.analog_late, (int)s_try.vendor_adc_cfg,
           (unsigned long)n, (long)peak, (long)rms,
           (unsigned long)entry0, (unsigned long)entry1,
           (unsigned long)hwp_audcodec->CFG,
           (unsigned long)hwp_hpsys_rcc->ENR2,
           (unsigned long)hwp_hpsys_rcc->RSTR2);
    return n > 0 ? 1 : 0;
}

void mic_stats(uint32_t *samples, int32_t *peak, int32_t *rms)
{
    uint32_t n = s_stats_n;

    if (samples)
        *samples = n;
    if (peak)
        *peak = s_stats_peak;
    if (rms)
        *rms = n ? (int32_t)__builtin_sqrt((double)(s_stats_sumsq / (int64_t)n))
                 : 0;
}

/* register dump for bring-up debugging: shows which clock/path bits are
 * actually set after mic_start() */
void mic_dump_regs(void)
{
    printf("[MicREG] CFG=%08lx ADC_CFG=%08lx CH0_CFG=%08lx ENTRY=%08lx\n",
           (unsigned long)hwp_audcodec->CFG,
           (unsigned long)hwp_audcodec->ADC_CFG,
           (unsigned long)hwp_audcodec->ADC_CH0_CFG,
           (unsigned long)hwp_audcodec->ADC_CH0_ENTRY);
    printf("[MicREG] CH1_CFG=%08lx CH1_ENTRY=%08lx\n",
           (unsigned long)hwp_audcodec->ADC_CH1_CFG,
           (unsigned long)hwp_audcodec->ADC_CH1_ENTRY);
    printf("[MicREG] ANA=%08lx REFGEN=%08lx ADC1_1=%08lx ADC1_2=%08lx ADC2_1=%08lx ADC2_2=%08lx\n",
           (unsigned long)hwp_audcodec->ADC_ANA_CFG,
           (unsigned long)hwp_audcodec->REFGEN_CFG,
           (unsigned long)hwp_audcodec->ADC1_CFG1,
           (unsigned long)hwp_audcodec->ADC1_CFG2,
           (unsigned long)hwp_audcodec->ADC2_CFG1,
           (unsigned long)hwp_audcodec->ADC2_CFG2);
    printf("[MicREG] PLL5=%08lx PLL6=%08lx ENTRY0=%08lx ENTRY1=%08lx\n",
           (unsigned long)hwp_audcodec->PLL_CFG5,
           (unsigned long)hwp_audcodec->PLL_CFG6,
           (unsigned long)hwp_audcodec->ADC_CH0_ENTRY,
           (unsigned long)hwp_audcodec->ADC_CH1_ENTRY);
    printf("[MicREG] PLL2=%08lx PLL3=%08lx STAT=%08lx BG0=%08lx BG1=%08lx BG2=%08lx\n",
           (unsigned long)hwp_audcodec->PLL_CFG2,
           (unsigned long)hwp_audcodec->PLL_CFG3,
           (unsigned long)hwp_audcodec->PLL_STAT,
           (unsigned long)hwp_audcodec->BG_CFG0,
           (unsigned long)hwp_audcodec->BG_CFG1,
           (unsigned long)hwp_audcodec->BG_CFG2);
    printf("[MicREG] PLL6=%08lx ADC1_1=%08lx ADC1_2=%08lx ADC2_2=%08lx\n",
           (unsigned long)hwp_audcodec->PLL_CFG6,
           (unsigned long)hwp_audcodec->ADC1_CFG1,
           (unsigned long)hwp_audcodec->ADC1_CFG2,
           (unsigned long)hwp_audcodec->ADC2_CFG2);
    printf("[MicREG] dma inst=%p ccr=%08lx cndtr=%lu running=%d\n",
           (void *)s_dma.Instance,
           (unsigned long)s_dma.Instance->CCR,
           (unsigned long)s_dma.Instance->CNDTR,
           (int)s_running);
}

/* Direct, IRQ-independent evidence.  Our own stats only move when the HAL's
 * DMA callbacks run, so "samples=0" cannot distinguish
 *   (a) the codec never converts            from
 *   (b) the codec converts and the DMA moves but our IRQ wiring is wrong.
 * This probe watches the DMA counter and scans the buffer itself. */
void mic_probe(int ms)
{
    uint32_t cndtr0 = s_dma.Instance->CNDTR;
    uint32_t ccr0 = s_dma.Instance->CCR;
    uint32_t n0 = s_stats_n;
    uint32_t irq0 = s_half_irqs;
    uint32_t moved = 0;
    int16_t maxabs0 = 0;
    int16_t maxabs1 = 0;
    uint32_t i;
    int step;

    for (i = 0; i < MIC_DMA_BYTES / 2; i++)
    {
        int16_t v = ((const int16_t *)s_dma_buf)[i];
        int16_t a = v < 0 ? (int16_t)-v : v;

        if (a > maxabs0)
            maxabs0 = a;
    }

    for (step = 0; step < ms / 10; step++)
    {
        usleep(10000);
        report_activity();
        if (s_dma.Instance->CNDTR != cndtr0)
            moved++;
    }

    for (i = 0; i < MIC_DMA_BYTES / 2; i++)
    {
        int16_t v = ((const int16_t *)s_dma_buf)[i];
        int16_t a = v < 0 ? (int16_t)-v : v;

        if (a > maxabs1)
            maxabs1 = a;
    }

    printf("[MicPROBE] %d ms: cndtr %lu->%lu changed=%lu/%d ccr=%08lx->%08lx "
           "stats %lu->%lu irq %lu->%lu bufsamples=%d bufmax %d->%d\n",
           ms, (unsigned long)cndtr0,
           (unsigned long)s_dma.Instance->CNDTR, (unsigned long)moved,
           ms / 10, (unsigned long)ccr0,
           (unsigned long)s_dma.Instance->CCR, (unsigned long)n0,
           (unsigned long)s_stats_n, (unsigned long)irq0,
           (unsigned long)s_half_irqs, MIC_DMA_BYTES / 2, (int)maxabs0,
           (int)maxabs1);
}

/* bring-up probe: sample the codec data register directly (bypassing
 * DMA) - if the value moves the codec is running and the problem is the
 * DMA request path, if it is frozen the codec is not sampling at all */
void mic_poll_regs(int rounds)
{
    uint32_t first = hwp_audcodec->ADC_CH0_ENTRY;
    uint32_t last = first;
    uint32_t changes = 0;
    int i;

    for (i = 0; i < rounds; i++)
    {
        uint32_t v = hwp_audcodec->ADC_CH0_ENTRY;
        if ((i % 100) == 0)
            report_activity();
        if (v != last)
            changes++;
        last = v;
        usleep(200);
    }
    printf("[MicPOLL] rounds=%d changes=%lu first=%08lx last=%08lx\n",
           rounds, (unsigned long)changes, (unsigned long)first,
           (unsigned long)last);
}
