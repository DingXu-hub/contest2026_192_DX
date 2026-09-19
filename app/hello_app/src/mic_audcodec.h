/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mic_audcodec.h - on-board MEMS microphone capture via the SoC's internal
 * AUDCODEC (LP ADC path), DMA'd into a ring buffer for the app / PC.
 *
 * The Huangshan Pi core board integrates a MEMS mic wired to the codec's
 * analogue input (MIC_BIAS + MIC_ADC_IN; the reference board for the same
 * chip marks PA09 as "share with MIC").  The vendor HAL ships the codec
 * driver but no board glue, and its Kconfig path is RT-Thread-only, so this
 * file drives the HAL directly - the same approach the LCD/GPU/IMU use.
 *
 * Every clocking parameter is runtime tunable (mic_cfg) because the codec
 * clock divider / OSR combination for 16 kHz is not documented in the
 * shipped tree; `!mic sweep` walks the plausible space and reports RMS.
 */

#ifndef __MIC_AUDCODEC_H
#define __MIC_AUDCODEC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t sample_rate;        /* target sample rate (informational)   */
    uint8_t  pll_type;           /* updata_pll_freq(): 0 = 16k/48k series */
    uint8_t  clk_src_sel;        /* AUDCODEC: 0 = xtal 48M, 1 = codec PLL */
    uint8_t  osr_sel;            /* ADC OSR: 0:200 1:300 2:400 3:600      */
    uint8_t  clk_div;            /* ADC clock divider                     */
    uint8_t  opmode;             /* ADC path operation mode               */
    uint8_t  sel_clk_adc_source; /* analogue path clock select            */
    uint8_t  sel_clk_adc;
    uint8_t  diva_clk_adc;
    uint8_t  fsp;                /* ADC1_CFG1 FSP                         */
    uint8_t  rough_vol;          /* ADC channel volume (0..0xf)           */
    uint8_t  channel;            /* digital ADC channel: 0 or 1           */
} mic_cfg_t;

/* defaults: 16 kHz, codec PLL (49.152 MHz) - see mic_audcodec.c */
void mic_cfg_default(mic_cfg_t *cfg);

/* bring up (or reconfigure) the codec + DMA; false on failure */
bool mic_start(const mic_cfg_t *cfg);
void mic_stop(void);
bool mic_running(void);

/* pull captured samples (16-bit mono); returns the number copied */
int  mic_read(int16_t *out, int max_samples);

/* statistics since the last mic_stats_reset() */
void mic_stats_reset(void);
void mic_stats(uint32_t *samples, int32_t *peak, int32_t *rms);

/* bring-up variant experiment: 1 = produced samples, 0 = no data, -1 = failed */
int  mic_try(int variant);

/* direct DMA/buffer evidence, independent of the IRQ statistics */
void mic_probe(int ms);

/* on-device brute-force sweep over clock/frame options (reports any hit) */
void mic_sweep2(void);

/* trigger the PLL VCO calibration and report whether it completes */
void mic_pll_probe(void);

/* install the keep-awake callback (called from the capture paths) */
void mic_set_activity_hook(void (*fn)(void));

/* register dump for bring-up debugging */
void mic_dump_regs(void);
void mic_poll_regs(int rounds);

/* live parameters (for the link-driven sweep) */
mic_cfg_t *mic_cfg(void);

#endif /* __MIC_AUDCODEC_H */
