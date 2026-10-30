#include "bw_dsp.h"
#include "dsp.h"
#include "dsp_dma_regs.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 8
#define HALT_FRAME_INSTRUCTION 0x0a8420u /* bset #0, x:$ffffc4 */

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int ready, go;

typedef struct {
    uint8_t bytes[64];
    size_t len;
    unsigned index;
    int calls;
} Capture;

static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    (void)opaque;
    (void)addr;
    if (!dir)
        memset(ptr, 0, len);
}

static void fifo_rw(void *opaque, uint8_t *ptr, unsigned index, size_t len, bool dir)
{
    Capture *c = opaque;
    if (dir && len <= sizeof(c->bytes)) {
        memcpy(c->bytes, ptr, len);
        c->len = len;
        c->index = index;
        c->calls++;
    }
}

static void *run_interpreter(void *opaque)
{
    int *failed = opaque;
    Capture c = {0};
    BwDsp *d;
    uint32_t words;

    pthread_mutex_lock(&gate_lock);
    ready++;
    pthread_cond_broadcast(&gate_cond);
    while (!go)
        pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    d = bw_dsp_new(&c, scratch_rw, fifo_rw, true);
    if (!d) {
        *failed = 1;
        return NULL;
    }
    if (!bw_dsp_mem(d, 'P', &words) || words < 1 ||
        bw_dsp_mem(d, '?', &words) != NULL || words != 0)
        *failed = 1;
    bw_dsp_write(d, 'P', 0, HALT_FRAME_INSTRUCTION);
    bw_dsp_write(d, 'X', 42, 0x123456);
    if (bw_dsp_read(d, 'X', 42) != 0x123456)
        *failed = 1;
    for (int i = 0; i < 100; i++) {
        bw_dsp_reset(d);
        if (bw_dsp_run_frame(d, 1000) != 4)
            *failed = 1;
    }
    bw_dsp_free(d);
    return NULL;
}

static int run_dma(DSPState *d, uint32_t seed)
{
    dsp_write_memory(d, 'X', 0, NODE_POINTER_EOL);
    dsp_write_memory(d, 'X', 1, NODE_CONTROL_DIRECTION | (2u << 10));
    dsp_write_memory(d, 'X', 2, 4);
    dsp_write_memory(d, 'X', 3, 0x100);
    dsp_write_memory(d, 'X', 4, 0);
    dsp_write_memory(d, 'X', 5, 0);
    dsp_write_memory(d, 'X', 6, 0);
    for (uint32_t i = 0; i < 4; i++)
        dsp_write_memory(d, 'X', 0x100 + i, seed + i);
    dsp_dma_write(&d->dma, DMA_NEXT_BLOCK, 0);
    dsp_dma_write(&d->dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    return !d->dma.error;
}

int main(void)
{
    pthread_t threads[THREADS];
    int failed[THREADS] = {0}, ok = 1;
    Capture a = {0}, b = {0};
    DSPState *da, *db;

    for (int i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, run_interpreter, &failed[i]) != 0) {
            fprintf(stderr, "FAIL: cannot create DSP test thread\n");
            return 1;
        }
    }
    pthread_mutex_lock(&gate_lock);
    while (ready != THREADS)
        pthread_cond_wait(&gate_cond, &gate_lock);
    go = 1;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        if (failed[i]) {
            fprintf(stderr, "FAIL: concurrent DSP initialization or execution\n");
            ok = 0;
        }
    }

    da = dsp_init(&a, scratch_rw, fifo_rw, true);
    db = dsp_init(&b, scratch_rw, fifo_rw, false);
    if (!da || !db) {
        fprintf(stderr, "FAIL: allocate DSP DMA instances\n");
        if (da) dsp_destroy(da);
        if (db) dsp_destroy(db);
        return 1;
    }
    if (!run_dma(da, 0x123400) || !run_dma(db, 0x654300) ||
        a.calls != 1 || b.calls != 1 || a.len != 16 || b.len != 16 ||
        a.index != 0 || b.index != 0 ||
        da->dma.scratch_buf == db->dma.scratch_buf) {
        fprintf(stderr, "FAIL: DSP DMA buffers or callbacks\n");
        ok = 0;
    } else {
        uint32_t first_a, first_b;
        memcpy(&first_a, a.bytes, sizeof(first_a));
        memcpy(&first_b, b.bytes, sizeof(first_b));
        if (first_a != 0x123400 || first_b != 0x654300) {
            fprintf(stderr, "FAIL: DSP DMA delivered wrong samples\n");
            ok = 0;
        }
    }
    dsp_destroy(da);
    dsp_destroy(db);
    return ok ? 0 : 1;
}
