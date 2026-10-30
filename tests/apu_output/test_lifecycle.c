#include "apu.h"
#include "apu_state.h"
#include "apu_xaudio2.h"
#include "dsp_dma_regs.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_ADDRESS_SPACE (UINT64_C(1) << 32)
#define GUEST_RAM_SIZE (64u * 1024u * 1024u)
#define GP_WINDOW 0xFE830000u
#define EP_WINDOW 0xFE850000u
#define DSP_WINDOWS_SIZE 0x50000u
#define HALT_FRAME_INSTRUCTION 0x0a8420u
#define CLEAR_DMA_NEXT_EOL 0x0a940eu
#define START_DMA_INSTRUCTION 0x0a9620u
#define WINDOW_WRITES 2000000u

void test_set_guest_base(uint8_t *base);
void bw_xdsp_init(MCPXAPUState *d);
void bw_xdsp_shutdown(void);
int bw_xdsp_enabled(void);
int bw_xdsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);

static int check(int condition, const char *description)
{
    if (!condition)
        fprintf(stderr, "FAIL: %s\n", description);
    return condition;
}

static uint8_t *map_guest(void)
{
    uint8_t *base = mmap(NULL, (size_t)GUEST_ADDRESS_SPACE, PROT_NONE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
    if (mprotect(base, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE) ||
        mprotect(base + 0x80000000u, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE) ||
        mprotect(base + GP_WINDOW, DSP_WINDOWS_SIZE, PROT_READ | PROT_WRITE)) {
        munmap(base, (size_t)GUEST_ADDRESS_SPACE);
        return NULL;
    }
    test_set_guest_base(base);
    return base;
}

static int run_pipeline(uint8_t *base, int threaded, int partial, int use_ep)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    uint32_t *program = (uint32_t *)(base + GP_WINDOW + NV_PAPU_GPPMEM);
    int ok = 1;
    if (!d)
        return check(0, "allocate synthetic APU state");

    memset(base + GP_WINDOW, 0, DSP_WINDOWS_SIZE);
    for (int i = 0; i < 0x1000; i++)
        program[i] = HALT_FRAME_INSTRUCTION;
    *(uint32_t *)(base + GP_WINDOW + NV_PAPU_GPRST) =
        NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    if (use_ep) {
        uint32_t *ep_program = (uint32_t *)(base + EP_WINDOW + NV_PAPU_EPPMEM);
        uint32_t *ep_x = (uint32_t *)(base + EP_WINDOW + NV_PAPU_EPXMEM);
        for (int i = 0; i < 0x1000; i += 3) {
            ep_program[i] = CLEAR_DMA_NEXT_EOL;
            if (i + 1 < 0x1000) ep_program[i + 1] = START_DMA_INSTRUCTION;
            if (i + 2 < 0x1000) ep_program[i + 2] = HALT_FRAME_INSTRUCTION;
        }
        ep_x[0] = NODE_POINTER_EOL;
        ep_x[1] = NODE_CONTROL_DIRECTION | (1u << 10); /* FIFO 0, 16-bit */
        ep_x[2] = 512;
        ep_x[3] = 0x100;
        for (int i = 0; i < 512; i++)
            ep_x[0x100 + i] = (i & 1) ? 0xc00000u : 0x240000u;
        *(uint32_t *)(base + EP_WINDOW + NV_PAPU_EPRST) =
            NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    }
    setenv("BW_APU_DSP", "xemu", 1);
    setenv("BW_APU_DSP_THREAD", threaded ? "1" : "0", 1);
    d->ram_ptr = base;
    g_apu_ram_ptr = base;
    bw_xdsp_init(d);
    ok &= check(bw_xdsp_enabled(), "synthetic DSP starts");

    for (int frame = 0; frame < (partial ? 3 : 128) && ok; frame++) {
        d->ep_frame_div = frame;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            bins[0][i] = (frame & 1) ? 0.125f : 0.25f;
            bins[1][i] = -0.5f;
        }
        ok &= check(bw_xdsp_frame(d, bins), "DSP handles a frame");
        if (frame % 8 == 7) {
            for (int f = 0; f < 8; f++) {
                int16_t left = use_ep ? 9216 : ((f & 1) ? 4096 : 8192);
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                    if (d->monitor.frame_buf[f * NUM_SAMPLES_PER_FRAME + i][0] != left ||
                        d->monitor.frame_buf[f * NUM_SAMPLES_PER_FRAME + i][1] != -16384) {
                        ok &= check(0, "ordered non-silent DSP output");
                        break;
                    }
                }
            }
        }
    }
    /* A partial period leaves asynchronous work pending at shutdown. */
    bw_xdsp_shutdown();
    bw_xdsp_shutdown();
    g_apu_ram_ptr = NULL;
    free(d);
    return ok;
}

static int run_no_device(uint8_t *base)
{
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        MCPXAPUState *d = mcpx_apu_init_standalone(base);
        ok &= check(d == NULL, "device failure rejects APU initialization");
        ok &= check(!xa2_is_active(), "failed output is not marked active");
        if (d)
            mcpx_apu_shutdown(d);
    }
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    MCPXAPUState *d = mcpx_apu_init_standalone(base);
    ok &= check(d != NULL, "APU can recover after a device initialization failure");
    if (d)
        mcpx_apu_shutdown(d);
    return ok;
}

static void *write_window(void *opaque)
{
    uint32_t *word = opaque;
    for (unsigned i = 0; i < WINDOW_WRITES; i++)
        __atomic_fetch_add(word, 1, __ATOMIC_RELAXED);
    return NULL;
}

static int run_window_concurrency(uint8_t *base)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    uint32_t *program = (uint32_t *)(base + GP_WINDOW + NV_PAPU_GPPMEM);
    uint32_t *word = (uint32_t *)(base + GP_WINDOW + NV_PAPU_GPXMEM) + 0x800;
    pthread_t writer;
    int ok = check(d != NULL, "allocate APU for DSP-window concurrency");
    if (!d)
        return 0;
    memset(base + GP_WINDOW, 0, DSP_WINDOWS_SIZE);
    for (int i = 0; i < 0x1000; i++)
        program[i] = HALT_FRAME_INSTRUCTION;
    *(uint32_t *)(base + GP_WINDOW + NV_PAPU_GPRST) =
        NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    *word = 1;
    d->ram_ptr = base;
    g_apu_ram_ptr = base;
    setenv("BW_APU_DSP_THREAD", "1", 1);
    bw_xdsp_init(d);
    for (int frame = 0; frame < 8; frame++) {
        d->ep_frame_div = frame;
        ok &= check(bw_xdsp_frame(d, bins), "prime DSP window");
    }
    if (pthread_create(&writer, NULL, write_window, word) != 0) {
        ok &= check(0, "start concurrent guest-window writer");
    } else {
        for (int frame = 8; frame < 136; frame++) {
            d->ep_frame_div = frame;
            ok &= check(bw_xdsp_frame(d, bins), "run DSP while guest writes its window");
        }
        pthread_join(writer, NULL);
        for (int frame = 136; frame < 144; frame++) {
            d->ep_frame_div = frame;
            ok &= check(bw_xdsp_frame(d, bins), "complete concurrent window sync");
        }
        ok &= check(__atomic_load_n(word, __ATOMIC_RELAXED) == WINDOW_WRITES + 1,
                    "DSP sync preserves every concurrent guest-window write");
    }
    bw_xdsp_shutdown();
    g_apu_ram_ptr = NULL;
    free(d);
    return ok;
}

static int run_mmio_wakeup(uint8_t *base)
{
    static const struct { uint32_t first; unsigned count; } fifo_groups[] = {
        { NV_PAPU_GPOFBASE0, GP_OUTPUT_FIFO_COUNT },
        { NV_PAPU_GPIFBASE0, GP_INPUT_FIFO_COUNT },
        { NV_PAPU_EPOFBASE0, EP_OUTPUT_FIFO_COUNT },
        { NV_PAPU_EPIFBASE0, EP_INPUT_FIFO_COUNT },
    };
    MCPXAPUState *d = mcpx_apu_init_standalone(base);
    int ok = check(d != NULL, "APU starts for MMIO wakeup");
    if (!d)
        return 0;
    for (unsigned group = 0; group < sizeof(fifo_groups) / sizeof(fifo_groups[0]); group++) {
        for (unsigned index = 0; index < fifo_groups[group].count; index++) {
            for (unsigned offset = 0; offset <= 8; offset += 4) {
                uint32_t addr = fifo_groups[group].first + 16 * index + offset;
                uint32_t value = 0x1000 + addr;
                mcpx_apu_mmio_write(d, addr, value, 4);
                ok &= check(mcpx_apu_mmio_read(d, addr, 4) == value, "FIFO MMIO round trip");
                mcpx_apu_mmio_write(d, addr, 0, 4);
            }
        }
    }
    mcpx_apu_mmio_write(d, NV_PAPU_TVL2D, 0xffff, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_TVL3D, 0xffff, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_TVLMP, 0xffff, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_FECTL, NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL,
                        NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF, 4);
    for (int wait = 0; wait < 2000 && (wait < 100 || xa2_samples_played() == 0); wait++) {
        mcpx_apu_mmio_write(d, NV_PAPU_FECTL, NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);
        mcpx_apu_mmio_write(d, NV_PAPU_ISTS, UINT32_MAX, 4);
        mcpx_apu_mmio_write(d, NV_PAPU_GPSMAXSGE, wait & 1, 4);
        (void)mcpx_apu_mmio_read(d, NV_PAPU_GPSMAXSGE, 4);
        mcpx_apu_mmio_write(d, NV_PAPU_EPOFBASE0, 0, 4);
        mcpx_apu_mmio_write(d, NV_PAPU_EPOFEND0, 0, 4);
        mcpx_apu_mmio_write(d, NV_PAPU_EPOFCUR0, wait, 4);
        (void)mcpx_apu_mmio_read(d, NV_PAPU_EPOFCUR0, 4);
        mcpx_apu_mmio_write(d, NV_PAPU_XGSCNT, 0, 4);
        (void)mcpx_apu_mmio_read(d, NV_PAPU_XGSCNT, 4);
        usleep(1000);
    }
    ok &= check(xa2_samples_played() > 0, "MMIO start wakes the real frame worker");
    mcpx_apu_shutdown(d);
    return ok;
}

int main(int argc, char **argv)
{
    uint8_t *base = map_guest();
    int ok = check(base != NULL, "map synthetic guest RAM and DSP windows");
    if (!ok)
        return 1;
    if (argc > 1 && !strcmp(argv[1], "--no-device")) {
        ok &= run_no_device(base);
    } else {
        ok &= check(mcpx_apu_init_standalone(NULL) == NULL, "missing RAM is rejected");
        for (int threaded = 0; threaded <= 1; threaded++) {
            for (int use_ep = 0; use_ep <= 1; use_ep++) {
                ok &= run_pipeline(base, threaded, 0, use_ep);
                for (int i = 0; i < 4; i++)
                    ok &= run_pipeline(base, threaded, 1, use_ep);
            }
        }
        ok &= run_window_concurrency(base);
        setenv("BW_APU_DSP_THREAD", "1", 1);
        ok &= run_mmio_wakeup(base);
        for (int i = 0; i < 3; i++) {
            MCPXAPUState *d = mcpx_apu_init_standalone(base);
            ok &= check(d != NULL, "full APU instance starts");
            if (!d)
                break;
            ok &= check(mcpx_apu_init_standalone(base) == NULL, "second instance is rejected");
            mcpx_apu_play_test_tone(d);
            for (int wait = 0; wait < 2000 && xa2_samples_played() == 0; wait++)
                usleep(1000);
            ok &= check(xa2_samples_played() > 0, "frame worker feeds the SDL device");
            mcpx_apu_shutdown(d);
            ok &= check(!xa2_is_active(), "output stops after active-tone shutdown");
        }
    }
    test_set_guest_base(NULL);
    munmap(base, (size_t)GUEST_ADDRESS_SPACE);
    return ok ? 0 : 1;
}
