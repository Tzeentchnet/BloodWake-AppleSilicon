/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/*
 * bw_dsp.c - Blood Wake's wrapper around xemu's MCPX DSP (D-012). See bw_dsp.h.
 *
 * Only the C interpreter is built (xemu's JIT is not vendored); dsp.c's JIT
 * entry points are satisfied below and never selected.
 */
#include "qemu/osdep.h"
#include "dsp.h"
#include "dsp_internal.h"
#include "interp/dsp_cpu.h"
#include "ui/xemu-settings.h"
#include "bw_dsp.h"

struct bw_xemu_config g_config = { { false } };

/* dsp.c names the JIT engine; it is never chosen (use_dsp_jit is false). */
const DSPOps jit_dsp_ops;
void dsp_jit_init(DSPState *dsp)
{
    dsp_c_init(dsp);
}

struct BwDsp {
    DSPState *dsp;
};

BwDsp *bw_dsp_new(void *opaque, bw_dsp_scratch_rw scratch, bw_dsp_fifo_rw fifo, bool is_gp)
{
    BwDsp *b = (BwDsp *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->dsp = dsp_init(opaque, (dsp_scratch_rw_func)scratch, (dsp_fifo_rw_func)fifo, is_gp);
    dsp_set_halt_requested(b->dsp, false);
    dsp_set_cycle_count(b->dsp, 0);
    return b;
}

void bw_dsp_free(BwDsp *b)
{
    if (!b)
        return;
    dsp_destroy(b->dsp);
    free(b);
}

void bw_dsp_reset(BwDsp *b)
{
    dsp_reset(b->dsp);
}

void bw_dsp_bootstrap(BwDsp *b)
{
    dsp_bootstrap(b->dsp);
}

uint32_t bw_dsp_run_frame(BwDsp *b, uint32_t max_cycles)
{
    uint32_t used;
    dsp_start_frame(b->dsp);
    dsp_set_halt_requested(b->dsp, false);
    dsp_set_cycle_count(b->dsp, 0);
    do {
        dsp_run(b->dsp, 1000);
        used = dsp_get_cycle_count(b->dsp);
    } while (!dsp_get_halt_requested(b->dsp) && used < max_cycles);
    return used;
}

uint32_t bw_dsp_read(BwDsp *b, char space, uint32_t addr)
{
    return dsp_read_memory(b->dsp, space, addr);
}

void bw_dsp_write(BwDsp *b, char space, uint32_t addr, uint32_t value)
{
    dsp_write_memory(b->dsp, space, addr, value);
}

uint32_t *bw_dsp_mem(BwDsp *b, char space, uint32_t *words)
{
    dsp_core_t *core = (dsp_core_t *)b->dsp->backend;
    switch (space) {
    case 'X': *words = DSP_XRAM_SIZE; return core->xram;
    case 'Y': *words = DSP_YRAM_SIZE; return core->yram;
    case 'P': *words = DSP_PRAM_SIZE; return core->pram;
    case 'M': *words = DSP_MIXBUFFER_SIZE; return core->mixbuffer;
    default: *words = 0; return NULL;
    }
}
