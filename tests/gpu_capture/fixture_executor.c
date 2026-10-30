#define _POSIX_C_SOURCE 200809L
#include "fixture_executor.h"
#include "bw_gpu_capture_fixture.h"
#include "platform/win32_compat.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 16u
#define HEIGHT 12u
#define PITCH 80u
#define SURFACE_BYTES (PITCH * HEIGHT)
#define COLOR_AT 0x10000u
#define COLOR2_AT 0x11000u
#define POS_AT 0x20000u
#define DIFF_AT 0x21000u
#define SEM_AT 0x30000u
#define MEM_BYTES 0x40000u
#define MAX_INDICES 64u

static _Alignas(16) uint8_t s_mem[MEM_BYTES];
static unsigned s_presents;
static uint32_t s_present_address, s_present_pixel;

uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base = 0x300000u, g_xbox_stack_size = 0x200000u;

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    s_presents++;
    s_present_address = va;
    if (va <= MEM_BYTES - (3u * PITCH + 4u * 4u + 4u) && pitch == PITCH)
        memcpy(&s_present_pixel, s_mem + va + 3u * PITCH + 4u * 4u, 4);
}
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
void xbox_Nv2aFrameCounterFlip(void) {}
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }
int nv2a_pb_dma_walk(void) { return 1; }
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
ULONGLONG GetTickCount64(void) { return 1000; }
int bw_ab_off(const char *fix) { (void)fix; return 0; }
void bw_ab_frame(uint32_t frame) { (void)frame; }

extern void nv2a_pb_exec_method(uint32_t subchannel, uint32_t method, uint32_t parameter);
extern int nv2a_pb_exec_idx16(const uint32_t *p, uint32_t n);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

static void configure(int audit, const char *path, const char *suffix)
{
    char audit_path[1024];
    unsetenv("RECOMP_FB_DUMP");
    unsetenv("RECOMP_FB_DUMP_FLIPS");
    unsetenv("RECOMP_RASTER_TEST");
    unsetenv("RECOMP_PB_EXEC_VERBOSE");
    unsetenv("BW_PB_MTRACE");
    unsetenv("RECOMP_FRAME_TRACE");
    setenv("RECOMP_FLIP_PACING", "0", 1);
    setenv("BW_PB_BULK", "1", 1);
    if (audit) {
        snprintf(audit_path, sizeof audit_path, "%s.%s-audit.txt", path, suffix);
        setenv("BW_GPU_AUDIT", "1", 1);
        setenv("BW_GPU_AUDIT_EVERY", "1", 1);
        setenv("BW_GPU_AUDIT_FILE", audit_path, 1);
    } else {
        unsetenv("BW_GPU_AUDIT");
    }
}

static void method(uint32_t m, uint32_t value)
{
    nv2a_pb_exec_method(0, m, value);
}

static uint32_t pixel(uint32_t surface, uint32_t x, uint32_t y)
{
    uint32_t value;
    memcpy(&value, s_mem + surface + y * PITCH + x * 4u, sizeof value);
    return value;
}

static int upload(BwGpuCapture *capture, uint32_t address, const void *bytes, uint32_t size)
{
    if ((uint64_t)address + size > MEM_BYTES ||
        bw_gpu_capture_memory(capture, address, bytes, size) != BW_GPU_CAPTURE_OK)
        return 0;
    memcpy(s_mem + address, bytes, size);
    return 1;
}

static int checkpoint(BwGpuCapture *capture, uint32_t address, uint32_t size)
{
    return bw_gpu_capture_barrier(capture, BW_GPU_CAPTURE_BARRIER_EXECUTOR) ==
               BW_GPU_CAPTURE_OK &&
           bw_gpu_capture_expect_memory(capture, address, s_mem + address, size) ==
               BW_GPU_CAPTURE_OK;
}

int fixture_capture(const char *path, int audit)
{
    static const float pos[3][3] = { {2, 2, 0.5f}, {14, 2, 0.5f}, {2, 10, 0.5f} };
    const uint32_t red[3] = { 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u };
    const uint32_t green[3] = { 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u };
    const uint32_t sem_init[2] = { 0x12345678u, 0x87654321u };
    const uint32_t pair = 0x00010000u;
    uint8_t surface[SURFACE_BYTES];
    BwGpuCapture *capture;
    int attached = 0, ok = 0;
    configure(audit, path, "capture");
    memset(s_mem, 0xCC, sizeof s_mem);
    memset(surface, 0xA5, sizeof surface);
    capture = bw_gpu_capture_create(1024u * 1024u, 4096);
    if (!capture || !bw_gpu_capture_fixture_begin(capture))
        goto out;
    attached = 1;
    if (!upload(capture, COLOR_AT, surface, sizeof surface) ||
        !upload(capture, COLOR2_AT, surface, sizeof surface) ||
        !upload(capture, POS_AT, pos, sizeof pos) ||
        !upload(capture, DIFF_AT, red, sizeof red) ||
        !upload(capture, SEM_AT, sem_init, sizeof sem_init))
        goto out;

    method(0x0208, 0x0008);
    method(0x0200, WIDTH << 16);
    method(0x0204, HEIGHT << 16);
    method(0x020C, PITCH);
    method(0x0210, COLOR_AT);
    method(0x1720, POS_AT);
    method(0x1760, 2u | (3u << 4) | (12u << 8));
    method(0x172C, DIFF_AT);
    method(0x176C, (4u << 4) | (4u << 8));
    method(0x1D90, 0xFF102030u);
    method(0x1D94, 0xF0);
    if (pixel(COLOR_AT, 0, 0) != 0xFF102030u ||
        !checkpoint(capture, COLOR_AT, SURFACE_BYTES))
        goto out;

    method(0x17FC, 5);
    method(0x1810, 2u << 24);
    method(0x17FC, 0);
    if (pixel(COLOR_AT, 4, 3) != 0xFFFF0000u ||
        !checkpoint(capture, COLOR_AT, SURFACE_BYTES))
        goto out;

    nv2a_pb_set_semaphore_target(SEM_AT);
    method(0x1D70, 0x11223344u);
    if (!checkpoint(capture, SEM_AT, 4))
        goto out;
    if (!upload(capture, DIFF_AT, green, sizeof green))
        goto out;
    method(0x17FC, 5);
    /* A capture must force the scanner's bulk path back to per-method calls. */
    if (nv2a_pb_exec_idx16(&pair, 1))
        goto out;
    method(0x1800, pair);
    method(0x1808, 2);
    method(0x17FC, 0);
    if (pixel(COLOR_AT, 4, 3) != 0xFF00FF00u ||
        !checkpoint(capture, COLOR_AT, SURFACE_BYTES))
        goto out;

    nv2a_pb_set_semaphore_target(SEM_AT + 4u);
    method(0x1D70, 0x55667788u);
    if (!checkpoint(capture, SEM_AT + 4u, 4))
        goto out;
    nv2a_pb_set_semaphore_target(0);

    method(0x0210, COLOR2_AT);
    method(0x1D90, 0xFF405060u);
    method(0x1D94, 0xF0);
    if (!checkpoint(capture, COLOR2_AT, SURFACE_BYTES))
        goto out;
    method(0x17FC, 5);
    method(0x1810, 2u << 24);
    method(0x17FC, 0);
    if (pixel(COLOR2_AT, 4, 3) != 0xFF00FF00u ||
        !checkpoint(capture, COLOR2_AT, SURFACE_BYTES))
        goto out;
    method(0x0130, 0);
    if (s_presents != 1 || s_present_address != COLOR2_AT ||
        s_present_pixel != 0xFF00FF00u)
        goto out;
    if (bw_gpu_capture_fixture_end() != BW_GPU_CAPTURE_OK)
        goto out;
    attached = 0;
    ok = bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_OK;
out:
    if (attached)
        (void)bw_gpu_capture_fixture_end();
    bw_gpu_capture_destroy(capture);
    if (!ok)
        fprintf(stderr, "synthetic capture failed\n");
    return ok;
}

typedef struct {
    uint8_t initialized[MEM_BYTES], dirty[MEM_BYTES], shadow[MEM_BYTES];
    uint32_t surface, width, height, pitch, pos, diffuse, semaphore;
    uint32_t indices[MAX_INDICES], count, draws, flips, syncs, checks, releases;
    uint32_t methods_executed;
    unsigned format_set, clear_set, pos_format, diffuse_format;
    int in_begin, pending_sync;
} StrictReplay;

static StrictReplay s_replay;

static int span_ok(uint32_t address, uint32_t bytes)
{
    return bytes && (uint64_t)address + bytes <= MEM_BYTES;
}

static int all_marked(const uint8_t *map, uint32_t address, uint32_t bytes)
{
    uint32_t i;
    if (!span_ok(address, bytes))
        return 0;
    for (i = 0; i < bytes; i++)
        if (!map[address + i])
            return 0;
    return 1;
}

static int any_marked(const uint8_t *map, uint32_t address, uint32_t bytes)
{
    uint32_t i;
    for (i = 0; i < bytes; i++)
        if (map[address + i])
            return 1;
    return 0;
}

static int surface_ready(const StrictReplay *r)
{
    return r->format_set && r->width == WIDTH && r->height == HEIGHT &&
           r->pitch == PITCH && (r->surface == COLOR_AT || r->surface == COLOR2_AT) &&
           all_marked(r->initialized, r->surface, SURFACE_BYTES);
}

static int vertex_inputs_ready(const StrictReplay *r)
{
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    float xy[3][2];
    uint32_t i;
    if (r->pos != POS_AT || r->diffuse != DIFF_AT || !r->pos_format ||
        !r->diffuse_format || r->count != 3)
        return 0;
    for (i = 0; i < r->count; i++) {
        uint32_t index = r->indices[i];
        uint64_t pa = (uint64_t)r->pos + index * UINT64_C(12);
        uint64_t ca = (uint64_t)r->diffuse + index * UINT64_C(4);
        float p[3];
        if (index >= MAX_INDICES || pa + 12 > POS_AT + 12u * MAX_INDICES ||
            ca + 4 > DIFF_AT + 4u * MAX_INDICES ||
            !all_marked(r->initialized, (uint32_t)pa, 12) ||
            !all_marked(r->initialized, (uint32_t)ca, 4))
            return 0;
        memcpy(p, r->shadow + (uint32_t)pa, sizeof p);
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) ||
            p[0] < 0 || p[0] >= WIDTH || p[1] < 0 || p[1] >= HEIGHT)
            return 0;
        if (p[0] < minx) minx = p[0];
        if (p[0] > maxx) maxx = p[0];
        if (p[1] < miny) miny = p[1];
        if (p[1] > maxy) maxy = p[1];
        xy[i][0] = p[0];
        xy[i][1] = p[1];
    }
    return (maxx - minx >= 8.0f || maxy - miny >= 8.0f) &&
           (xy[1][0] - xy[0][0]) * (xy[2][1] - xy[0][1]) !=
           (xy[1][1] - xy[0][1]) * (xy[2][0] - xy[0][0]);
}

static BwGpuCaptureStatus preflight_method(StrictReplay *r, const BwGpuCaptureEvent *event)
{
    uint32_t m = event->data.method.method, p = event->data.method.parameter, i;
    if (event->data.method.subchannel != 0)
        return BW_GPU_CAPTURE_UNSUPPORTED;
    if (r->in_begin && m != 0x17FC && m != 0x1810 && m != 0x1800 && m != 0x1808)
        return BW_GPU_CAPTURE_UNSUPPORTED;
    switch (m) {
    case 0x0208:
        if (p != 0x0008) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->format_set = 1;
        break;
    case 0x0200:
        if (p != WIDTH << 16) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->width = p >> 16;
        break;
    case 0x0204:
        if (p != HEIGHT << 16) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->height = p >> 16;
        break;
    case 0x020C:
        if (p != PITCH) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->pitch = p;
        break;
    case 0x0210:
        if (p != COLOR_AT && p != COLOR2_AT) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->surface = p;
        break;
    case 0x1720:
        if (p != POS_AT) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->pos = p;
        break;
    case 0x1760:
        if (p != (2u | (3u << 4) | (12u << 8))) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->pos_format = 1;
        break;
    case 0x172C:
        if (p != DIFF_AT) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->diffuse = p;
        break;
    case 0x176C:
        if (p != ((4u << 4) | (4u << 8))) return BW_GPU_CAPTURE_UNSUPPORTED;
        r->diffuse_format = 1;
        break;
    case 0x1D90:
        r->clear_set = 1;
        break;
    case 0x1D94:
        if (p != 0xF0 || !r->clear_set || !surface_ready(r))
            return BW_GPU_CAPTURE_UNSUPPORTED;
        memset(r->dirty + r->surface, 1, SURFACE_BYTES);
        r->pending_sync = 1;
        break;
    case 0x17FC:
        if (p == 5 && !r->in_begin) {
            r->in_begin = 1;
            r->count = 0;
        } else if (p == 0 && r->in_begin && surface_ready(r) && vertex_inputs_ready(r)) {
            r->in_begin = 0;
            r->draws++;
            memset(r->dirty + r->surface, 1, SURFACE_BYTES);
            r->pending_sync = 1;
        } else
            return BW_GPU_CAPTURE_UNSUPPORTED;
        break;
    case 0x1810: {
        uint32_t start = p & 0xFFFFFFu, count = (p >> 24) + 1u;
        if (!r->in_begin || count > MAX_INDICES - r->count ||
            (uint64_t)start + count > MAX_INDICES)
            return BW_GPU_CAPTURE_UNSUPPORTED;
        for (i = 0; i < count; i++)
            r->indices[r->count++] = start + i;
        break;
    }
    case 0x1800:
        if (!r->in_begin || r->count > MAX_INDICES - 2)
            return BW_GPU_CAPTURE_UNSUPPORTED;
        r->indices[r->count++] = p & 0xFFFFu;
        r->indices[r->count++] = p >> 16;
        break;
    case 0x1808:
        if (!r->in_begin || r->count >= MAX_INDICES)
            return BW_GPU_CAPTURE_UNSUPPORTED;
        r->indices[r->count++] = p;
        break;
    case 0x1D70:
        if (!r->semaphore || !all_marked(r->initialized, r->semaphore, 4))
            return BW_GPU_CAPTURE_UNSUPPORTED;
        memset(r->dirty + r->semaphore, 1, 4);
        r->pending_sync = 1;
        r->releases++;
        break;
    case 0x0130:
        if (p || !surface_ready(r) || !r->draws)
            return BW_GPU_CAPTURE_UNSUPPORTED;
        r->flips++;
        break;
    default:
        return BW_GPU_CAPTURE_UNSUPPORTED;
    }
    return BW_GPU_CAPTURE_OK;
}

static BwGpuCaptureStatus preflight(void *context, const BwGpuCaptureEvent *event)
{
    StrictReplay *r = (StrictReplay *)context;
    uint32_t a, size;
    switch (event->type) {
    case BW_GPU_CAPTURE_MEMORY:
        a = event->data.memory.address;
        size = event->data.memory.size;
        if (!span_ok(a, size) || any_marked(r->dirty, a, size))
            return BW_GPU_CAPTURE_UNSUPPORTED;
        memcpy(r->shadow + a, event->data.memory.bytes, size);
        memset(r->initialized + a, 1, size);
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_EXPECT_MEMORY:
        a = event->data.memory.address;
        size = event->data.memory.size;
        if (!all_marked(r->initialized, a, size) || r->pending_sync)
            return BW_GPU_CAPTURE_UNSUPPORTED;
        memset(r->dirty + a, 0, size);
        r->checks++;
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_SEMAPHORE_TARGET:
        a = event->data.semaphore.address;
        if (a && ((a != SEM_AT && a != SEM_AT + 4u) ||
                  !all_marked(r->initialized, a, 4)))
            return BW_GPU_CAPTURE_UNSUPPORTED;
        r->semaphore = a;
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_BARRIER:
        if (event->data.barrier.kind != BW_GPU_CAPTURE_BARRIER_EXECUTOR)
            return BW_GPU_CAPTURE_UNSUPPORTED;
        r->pending_sync = 0;
        r->syncs++;
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_METHOD:
        return preflight_method(r, event);
    default:
        return BW_GPU_CAPTURE_UNSUPPORTED;
    }
}

static BwGpuCaptureStatus preflight_end(void *context)
{
    StrictReplay *r = (StrictReplay *)context;
    if (r->in_begin || r->pending_sync || !r->draws || !r->flips ||
        !r->syncs || !r->checks || !r->releases || any_marked(r->dirty, 0, MEM_BYTES))
        return BW_GPU_CAPTURE_UNSUPPORTED;
    return BW_GPU_CAPTURE_OK;
}

static BwGpuCaptureStatus execute(void *context, const BwGpuCaptureEvent *event)
{
    StrictReplay *r = (StrictReplay *)context;
    switch (event->type) {
    case BW_GPU_CAPTURE_MEMORY:
        memcpy(s_mem + event->data.memory.address, event->data.memory.bytes,
               event->data.memory.size);
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_EXPECT_MEMORY:
        return memcmp(s_mem + event->data.memory.address, event->data.memory.bytes,
                      event->data.memory.size) ? BW_GPU_CAPTURE_MISMATCH : BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_SEMAPHORE_TARGET:
        nv2a_pb_set_semaphore_target(event->data.semaphore.address);
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_BARRIER:
        /* This profile runs the CPU executor synchronously on this thread. */
        return event->data.barrier.kind == BW_GPU_CAPTURE_BARRIER_EXECUTOR
             ? BW_GPU_CAPTURE_OK : BW_GPU_CAPTURE_UNSUPPORTED;
    case BW_GPU_CAPTURE_METHOD:
        r->methods_executed++;
        nv2a_pb_exec_method(event->data.method.subchannel, event->data.method.method,
                            event->data.method.parameter);
        return BW_GPU_CAPTURE_OK;
    default:
        return BW_GPU_CAPTURE_UNSUPPORTED;
    }
}

static const BwGpuCaptureReplayOps s_ops = { preflight, preflight_end, execute };

int fixture_replay(const char *path, int audit, int expect_mismatch)
{
    BwGpuCaptureStatus status;
    uint32_t failure = UINT32_MAX;
    configure(audit, path, "replay");
    memset(s_mem, 0xCC, sizeof s_mem);
    memset(&s_replay, 0, sizeof s_replay);
    status = bw_gpu_capture_replay_file(path, &s_ops, &s_replay, &failure);
    if (expect_mismatch)
        return status == BW_GPU_CAPTURE_MISMATCH && failure != UINT32_MAX &&
               s_replay.methods_executed != 0;
    if (status != BW_GPU_CAPTURE_OK || s_presents != 1 ||
        s_present_address != COLOR2_AT || s_present_pixel != 0xFF00FF00u ||
        pixel(COLOR_AT, 4, 3) != 0xFF00FF00u ||
        pixel(COLOR2_AT, 0, 0) != 0xFF405060u) {
        fprintf(stderr, "synthetic replay failed: status %d, event %u\n", status, failure);
        return 0;
    }
    return 1;
}

int fixture_replay_rejection(const char *path, BwGpuCaptureStatus expected)
{
    BwGpuCaptureStatus status;
    configure(0, path, "reject");
    memset(s_mem, 0xCC, sizeof s_mem);
    memset(&s_replay, 0, sizeof s_replay);
    status = bw_gpu_capture_replay_file(path, &s_ops, &s_replay, NULL);
    return status == expected && !s_replay.methods_executed && s_mem[COLOR_AT] == 0xCC;
}

static atomic_int s_wrong_thread_started, s_wrong_end_status;

static void *wrong_thread(void *unused)
{
    (void)unused;
    atomic_store(&s_wrong_thread_started, 1);
    atomic_store(&s_wrong_end_status, bw_gpu_capture_fixture_end());
    for (unsigned i = 0; i < 1000; i++)
        nv2a_pb_set_semaphore_target((i & 1u) ? SEM_AT : 0);
    return NULL;
}

int fixture_thread_violation(void)
{
    BwGpuCapture *capture = bw_gpu_capture_create(1024u * 1024u, 4096);
    pthread_t thread;
    uint32_t data = 1;
    int ok = 0;
    configure(0, "unused", "unused");
    if (!capture || !bw_gpu_capture_fixture_begin(capture))
        goto out;
    if (pthread_create(&thread, NULL, wrong_thread, NULL) != 0) {
        (void)bw_gpu_capture_fixture_end();
        goto out;
    }
    while (!atomic_load(&s_wrong_thread_started))
        ;
    for (unsigned i = 0; i < 1000; i++) {
        (void)bw_gpu_capture_memory(capture, 0x1000, &data, sizeof data);
        method(0x0208, 8);
    }
    pthread_join(thread, NULL);
    ok = atomic_load(&s_wrong_end_status) == BW_GPU_CAPTURE_THREAD &&
         bw_gpu_capture_fixture_end() == BW_GPU_CAPTURE_THREAD &&
         bw_gpu_capture_status(capture) == BW_GPU_CAPTURE_THREAD;
out:
    bw_gpu_capture_destroy(capture);
    return ok;
}

int fixture_late_attach(void)
{
    BwGpuCapture *capture = bw_gpu_capture_create(1024, 16);
    int ok;
    configure(0, "unused", "unused");
    if (!capture)
        return 0;
    nv2a_pb_set_semaphore_target(SEM_AT);
    ok = !bw_gpu_capture_fixture_begin(capture);
    nv2a_pb_set_semaphore_target(0);
    method(0x0208, 8);
    ok = ok && !bw_gpu_capture_fixture_begin(capture);
    bw_gpu_capture_destroy(capture);
    return ok;
}

int fixture_bulk_disabled(void)
{
    const uint32_t pair = 0x00010000u;
    configure(0, "unused", "unused");
    method(0x0208, 8);
    return nv2a_pb_exec_idx16(&pair, 1) == 1;
}
