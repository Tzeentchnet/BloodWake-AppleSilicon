/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp.h - Blood Wake's opaque wrapper around xemu's MCPX DSP (D-012).
 *
 * xemu's DSP56300 interpreter (src/xemu_dsp, from xemu v0.8.139, GPL-2.0+)
 * defines its own DSPState, which the toolkit's APU headers also name, so the
 * carried apu_dsp.c talks to it only through this header.
 */
#ifndef BW_DSP_H
#define BW_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct BwDsp BwDsp;

/* dir true: DSP -> memory (a write), false: memory -> DSP (a read). */
typedef void (*bw_dsp_scratch_rw)(void *opaque, uint8_t *ptr, uint32_t addr,
                                  size_t len, bool dir);
typedef void (*bw_dsp_fifo_rw)(void *opaque, uint8_t *ptr, unsigned int index,
                               size_t len, bool dir);

BwDsp *bw_dsp_new(void *opaque, bw_dsp_scratch_rw scratch, bw_dsp_fifo_rw fifo, bool is_gp);
void bw_dsp_free(BwDsp *dsp);
void bw_dsp_reset(BwDsp *dsp);
void bw_dsp_bootstrap(BwDsp *dsp);

/* One frame: start-of-frame interrupt, then run until the program halts or
 * max_cycles pass. Returns the cycles used. */
uint32_t bw_dsp_run_frame(BwDsp *dsp, uint32_t max_cycles);

uint32_t bw_dsp_read(BwDsp *dsp, char space, uint32_t addr);
void bw_dsp_write(BwDsp *dsp, char space, uint32_t addr, uint32_t value);

/* Direct views of the memories, for the per-frame window sync (sizes in
 * 32-bit words; each holds one 24-bit DSP word). */
uint32_t *bw_dsp_mem(BwDsp *dsp, char space, uint32_t *words);

#endif
