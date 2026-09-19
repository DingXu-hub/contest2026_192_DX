/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mic_audprc.h - capture through the AUDPRC audio processor.
 *
 * The vendor's working SF32LB52 AI-voice product uses
 *   CONFIG_BSP_ENABLE_AUD_CODEC=y + CONFIG_BSP_AUDPRC_RX0_DMA=y
 * i.e. AUDCODEC ADC -> AUDPRC -> RX_CH0 -> DMA.  This is that path.
 */

#ifndef __MIC_AUDPRC_H
#define __MIC_AUDPRC_H

#include <stdint.h>
#include <stdbool.h>

/* Start capture; src_sel selects which source feeds the AUDPRC ADC path
 * (walked by mic_prc_sweep() when the right one is unknown).  The AUDCODEC
 * itself must already be configured (mic_start()). */
bool mic_prc_start(int src_sel);
void mic_prc_stop(void);
bool mic_prc_running(void);

void mic_prc_stats(uint32_t *samples, int32_t *peak, int32_t *rms,
                   uint32_t *irqs, uint32_t *cndtr);
int  mic_prc_read(int16_t *out, int max_samples);

/* raw DMA buffer inspection, independent of the interrupt path */
void mic_prc_peek(int *nonzero, int32_t *peak, int32_t *first, int32_t *last);

/* Try every ADC source selector and report which produces data */
void mic_prc_sweep(void);

#endif /* __MIC_AUDPRC_H */
