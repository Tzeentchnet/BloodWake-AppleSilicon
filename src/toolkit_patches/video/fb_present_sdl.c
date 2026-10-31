/**
 * Show the guest framebuffer in a window: the POSIX half of fb_present.c.
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest would otherwise own
 * that thread for the whole run. So the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, the presents
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * A title opts in by handing its boot sequence to xbox_HostWindowMain()
 * from main(). With RECOMP_FB_WINDOW set (the same switch as the Win32
 * window) it opens the window, starts the guest on a pthread with at least
 * the main thread's stack and, on macOS, its QoS, and pumps events and
 * presents until the guest returns or the window is closed. Without it, or
 * with RECOMP_HEADLESS=1, the guest runs on the calling thread. An explicitly
 * requested window must open successfully before the guest starts.
 *
 * Frames come from the executor's flip (xbox_FramebufferWindowPresent, on
 * the executor's thread). The flip copies the finished surface into a free
 * slot of three and publishes it with an index swap under a mutex held for a
 * few instructions, so the executor never waits for the window and the
 * window never reads guest memory the rasteriser may be writing. Legacy
 * surfaces use the GDI window's 640x480 convention (pitch / 640 is 2 for
 * R5G6B5, 4 for X8R8G8B8). Metal snapshots instead carry frozen format,
 * geometry and supersampling metadata, and are resolved before upload. The
 * window is black until the first completed flip.
 *
 * Closing the window ends the process without running teardown handlers:
 * lifted code and its detached workers cannot be stopped mid-flight.
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_WINDOW_SCALE=<n> sizes the window (default 2).
 */
#if !defined(_WIN32)

#include <SDL.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "video_player.h"
#include "keyboard_sdl.h"     /* src/input: keys into the pad stand-in */
#include "../bw_runtime_lifecycle.h"
#include "../../gpu/bw_gamma.h"
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "bw_framebuffer_metal.h"
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_GetMemoryBase(void);
extern size_t xbox_GetMappedSize(void);
extern void xbox_InputSetMainPump(int on);   /* src/input/xinput_device.c */

#define FB_W 640u
#define FB_H 480u

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t bpp, width, height;
};

/* Released on startup failure, before the guest or its workers can use SDL. */
#define FBWIN_SDL_SUBSYSTEMS \
    (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int     s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop */
static _Atomic int     s_wake_pending;
static _Atomic int     s_in_present;         /* executor inside Present */
static _Atomic int     s_present_error;

enum {
    PRESENT_ERROR_FORMAT = 1,
    PRESENT_ERROR_DESCRIPTOR,
    PRESENT_ERROR_MEMORY,
    PRESENT_ERROR_ALLOC
};

/* Counted at the flip, read once a second for the title bar. */
static uint64_t s_flips;
static uint32_t s_frame_draws;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    (void)fb_va;
    (void)pitch;
}

/* The legacy path retains its old behaviour. Metal uses its frozen two-axis
 * sampling descriptor rather than this vertical-only hint. */
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

/* The window is opened by xbox_HostWindowMain, on the main thread. */
void xbox_FramebufferWindowStart(void) {}

static void wake_main(void)
{
    /* Wake the main loop, once per batch of frames it has not looked at. */
    if (!atomic_exchange_explicit(&s_wake_pending, 1, memory_order_acq_rel)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        /* Not queued (full, or filtered): clear the latch, or no frame
         * would wake the loop again before its timeout. */
        if (SDL_PushEvent(&ev) <= 0)
            atomic_store_explicit(&s_wake_pending, 0, memory_order_release);
    }
}

/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t bpp, uint32_t width, uint32_t height)
{
    s_slot[s_back].bpp = bpp;          /* only this thread touches s_back */
    s_slot[s_back].width = width;
    s_slot[s_back].height = height;
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
    }
    pthread_mutex_unlock(&s_lock);
    wake_main();
}

static void present_fail(int error)
{
    int expected = 0;
    atomic_compare_exchange_strong_explicit(&s_present_error, &expected, error,
                                            memory_order_release,
                                            memory_order_relaxed);
    wake_main();
}

static const uint8_t *guest_frame(uint32_t fb_va, uint32_t pitch,
                                  uint32_t row_bytes, uint32_t height)
{
    const uint64_t limit = UINT64_C(1) << 32;
    uint64_t rows, end;
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
    void *contig = NULL;
    uint32_t contig_va = 0;
    size_t contig_size = 0;
    int valid;

    if (!height || !row_bytes || pitch < row_bytes)
        return NULL;
    rows = (uint64_t)(height - 1u) * pitch;
    if (rows > limit - fb_va || row_bytes > limit - fb_va - rows)
        return NULL;
    end = (uint64_t)fb_va + rows + row_bytes;
    if (!base || base != (uintptr_t)xbox_GetMemoryBase() ||
        end - 1u > UINTPTR_MAX - base)
        return NULL;
    valid = end <= xbox_GetMappedSize();
    if (!valid &&
        bw_xbox_contiguous_memory_region(&contig, &contig_va, &contig_size) &&
        base <= UINTPTR_MAX - contig_va &&
        (uintptr_t)contig == base + contig_va &&
        contig_size <= limit - contig_va &&
        fb_va >= contig_va && end <= (uint64_t)contig_va + contig_size)
        valid = 1;
    return valid ? (const uint8_t *)(base + fb_va) : NULL;
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    struct fb_slot *s = &s_slot[s_back];
    const uint8_t *src;
    uint32_t bpp = pitch / FB_W, y;
    size_t need;

    if (bpp != 2 && bpp != 4) {
        present_fail(PRESENT_ERROR_FORMAT);
        return;
    }
    src = guest_frame(fb_va, pitch, FB_W * bpp, FB_H);
    if (!src) {
        present_fail(PRESENT_ERROR_MEMORY);
        return;
    }
    need = (size_t)FB_W * FB_H * bpp;
    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n) {
            present_fail(PRESENT_ERROR_ALLOC);
            return;
        }
        s->px = n;
        s->cap = need;
    }
    for (y = 0; y < FB_H; y++)
        memcpy(s->px + (size_t)y * FB_W * bpp, src + (size_t)y * pitch,
               (size_t)FB_W * bpp);
    if (bpp == 4)
        bw_gamma_apply_xrgb((uint32_t *)s->px, (size_t)FB_W * FB_H);
    publish(bpp, FB_W, FB_H);
}

#if defined(__APPLE__)
static _Atomic int s_metal_expected;
static _Thread_local BwMetalFramebuffer s_metal_frame;
static _Thread_local int s_metal_pending;
static uint8_t *s_metal_samples;
static size_t s_metal_samples_cap;

void bw_sdl_framebuffer_expect_metal(void)
{
    atomic_store_explicit(&s_metal_expected, 1, memory_order_release);
}

static int metal_descriptor_valid(const BwMetalFramebuffer *frame)
{
    uint64_t row_bytes, out_width, out_height;

    if (!frame || !frame->guest_va || !frame->width || !frame->height ||
        (frame->pixel_format != BW_METAL_PIXEL_BGRA8 &&
         frame->pixel_format != BW_METAL_PIXEL_BGRX8_ZERO) ||
        (frame->aa_sx != 1 && frame->aa_sx != 2) ||
        (frame->aa_sy != 1 && frame->aa_sy != 2) ||
        frame->width % frame->aa_sx || frame->height % frame->aa_sy)
        return 0;
    row_bytes = (uint64_t)frame->width * 4u;
    out_width = frame->width / frame->aa_sx;
    out_height = frame->height / frame->aa_sy;
    return row_bytes <= frame->pitch && out_width <= INT_MAX / 4u &&
           out_height <= INT_MAX && out_width * out_height <= SIZE_MAX / 4u;
}

int bw_sdl_framebuffer_prepare_metal(const BwMetalFramebuffer *frame)
{
    int ok = 1;

    /* A sync may be followed by no window present. The next sync replaces
     * that unconsumed description rather than reusing it for another flip. */
    s_metal_pending = 0;
    if (!atomic_load_explicit(&s_running, memory_order_seq_cst))
        return 1;
    atomic_fetch_add_explicit(&s_in_present, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&s_running, memory_order_seq_cst)) {
        if (!metal_descriptor_valid(frame)) {
            present_fail(PRESENT_ERROR_DESCRIPTOR);
            ok = 0;
        } else {
            s_metal_frame = *frame;
            s_metal_pending = 1;
        }
    }
    atomic_fetch_sub_explicit(&s_in_present, 1, memory_order_seq_cst);
    return ok;
}

static int read_mapped_bytes(void *dst, const void *src, size_t bytes)
{
    mach_vm_size_t copied = 0;
    return bytes &&
           mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)src,
                                  (mach_vm_size_t)bytes,
                                  (mach_vm_address_t)(uintptr_t)dst,
                                  &copied) == KERN_SUCCESS && copied == bytes;
}

/* Numeric mapping bounds do not guarantee that the current pages are
 * readable. Mach copies fail cleanly if guest protection changes underneath
 * a snapshot. Padding is not read, even when it contains inaccessible pages. */
static int read_mapped_rows(uint8_t *dst, const uint8_t *src, size_t row_bytes,
                            uint32_t pitch, uint32_t rows)
{
    uint32_t y;
    if (!rows || rows > SIZE_MAX / row_bytes)
        return 0;
    if (pitch == row_bytes)
        return read_mapped_bytes(dst, src, row_bytes * rows);
    for (y = 0; y < rows; y++) {
        if (!read_mapped_bytes(dst + (size_t)y * row_bytes,
                               (const void *)((uintptr_t)src + (uint64_t)y * pitch),
                               row_bytes))
            return 0;
    }
    return 1;
}

static void present_copy_metal(const BwMetalFramebuffer *frame)
{
    struct fb_slot *slot = &s_slot[s_back];
    uint32_t width = frame->width / frame->aa_sx;
    uint32_t height = frame->height / frame->aa_sy;
    uint32_t row_bytes = frame->width * 4u;
    size_t need = (size_t)width * height * 4u;
    const uint8_t *src;

    src = guest_frame(frame->guest_va, frame->pitch, row_bytes, frame->height);
    if (!src) {
        present_fail(PRESENT_ERROR_MEMORY);
        return;
    }
    if (slot->cap < need) {
        uint8_t *next = (uint8_t *)realloc(slot->px, need);
        if (!next) {
            present_fail(PRESENT_ERROR_ALLOC);
            return;
        }
        slot->px = next;
        slot->cap = need;
    }
    if (frame->aa_sx == 1 && frame->aa_sy == 1) {
        if (!read_mapped_rows(slot->px, src, row_bytes, frame->pitch, frame->height)) {
            present_fail(PRESENT_ERROR_MEMORY);
            return;
        }
    } else {
        const uint32_t batch_height = 32;
        uint32_t sample_rows = height < batch_height ? height : batch_height;
        size_t sample_need;
        uint32_t y;

        sample_rows *= frame->aa_sy;
        if (sample_rows > SIZE_MAX / row_bytes) {
            present_fail(PRESENT_ERROR_ALLOC);
            return;
        }
        sample_need = (size_t)sample_rows * row_bytes;
        if (s_metal_samples_cap < sample_need) {
            uint8_t *next = (uint8_t *)realloc(s_metal_samples, sample_need);
            if (!next) {
                present_fail(PRESENT_ERROR_ALLOC);
                return;
            }
            s_metal_samples = next;
            s_metal_samples_cap = sample_need;
        }
        for (y = 0; y < height;) {
            uint32_t batch = height - y < batch_height ? height - y : batch_height;
            const uint8_t *batch_src =
                (const uint8_t *)((uintptr_t)src + (uint64_t)y * frame->aa_sy * frame->pitch);
            uint32_t by, x;
            if (!read_mapped_rows(s_metal_samples, batch_src, row_bytes, frame->pitch,
                                  batch * frame->aa_sy)) {
                present_fail(PRESENT_ERROR_MEMORY);
                return;
            }
            /* The basic fallback is a box average in RGB before the DAC
             * gamma ramp. It is not the title-selected NV2A reconstruction
             * filter, which has not yet been traced. */
            for (by = 0; by < batch; by++) {
                for (x = 0; x < width; x++) {
                    unsigned sum[3] = {0, 0, 0};
                    unsigned sy, sx, c;
                    unsigned count = frame->aa_sx * frame->aa_sy;
                    uint8_t *out = slot->px + ((size_t)(y + by) * width + x) * 4u;
                    for (sy = 0; sy < frame->aa_sy; sy++) {
                        const uint8_t *row = s_metal_samples +
                            (size_t)(by * frame->aa_sy + sy) * row_bytes;
                        for (sx = 0; sx < frame->aa_sx; sx++) {
                            const uint8_t *sample = row + ((size_t)x * frame->aa_sx + sx) * 4u;
                            for (c = 0; c < 3; c++)
                                sum[c] += sample[c];
                        }
                    }
                    for (c = 0; c < 3; c++)
                        out[c] = (uint8_t)((sum[c] + count / 2u) / count);
                    out[3] = 0xff;
                }
            }
            y += batch;
        }
    }
    bw_gamma_apply_xrgb((uint32_t *)slot->px, (size_t)width * height);
    publish(4, width, height);
}
#endif

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's, all seq_cst, so either the window sees
     * this present in flight or this present sees the window closing. */
    if (!atomic_load_explicit(&s_running, memory_order_seq_cst))
        return;
#if defined(__APPLE__)
    if ((!fb_va || !pitch) &&
        !atomic_load_explicit(&s_metal_expected, memory_order_acquire))
        return;
#else
    if (!fb_va || !pitch)
        return;
#endif
    atomic_fetch_add_explicit(&s_in_present, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&s_running, memory_order_seq_cst)) {
#if defined(__APPLE__)
        if (atomic_load_explicit(&s_metal_expected, memory_order_acquire)) {
            int pending = s_metal_pending;
            BwMetalFramebuffer frame = s_metal_frame;
            s_metal_pending = 0;
            if (!pending || frame.guest_va != fb_va || frame.pitch != pitch)
                present_fail(PRESENT_ERROR_DESCRIPTOR);
            else
                present_copy_metal(&frame);
        } else
#endif
            present_copy(fb_va, pitch);
    }
    atomic_fetch_sub_explicit(&s_in_present, 1, memory_order_seq_cst);
}

/* Title bar: "<XBE title> | FPS: n | draws: n", as the GDI window shows it. */
static char s_title[128] = "Xbox Recomp";

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    /* UTF-16 (the certificate's title) to UTF-8 for SDL. BMP only: a lone
     * surrogate becomes '?'. */
    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        pthread_mutex_lock(&s_lock);
        memcpy(s_title, buf, o + 1);
        pthread_mutex_unlock(&s_lock);
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
}

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static int s_guest_rc;
static _Atomic int s_guest_done;
static Uint32 s_guest_done_event;
static pthread_mutex_t s_guest_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_guest_gate = PTHREAD_COND_INITIALIZER;
static int s_guest_ready;

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    pthread_mutex_lock(&s_guest_gate_lock);
    while (!s_guest_ready)
        pthread_cond_wait(&s_guest_gate, &s_guest_gate_lock);
    pthread_mutex_unlock(&s_guest_gate_lock);
    s_guest_rc = s_guest_main();
    atomic_store_explicit(&s_guest_done, 1, memory_order_release);
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

static int start_guest(pthread_t *thread)
{
    pthread_attr_t attr;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    rc = pthread_attr_init(&attr);
    if (rc != 0)
        return rc;
    rc = pthread_attr_setstacksize(&attr, stack);
    if (rc != 0) {
        pthread_attr_destroy(&attr);
        return rc;
    }
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(thread, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    return rc;
}

/* ── The window, on the process main thread ───────────────────────────── */

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

static int s_window_started;

static void runtime_failure(const char *what)
{
    fprintf(stderr, "  [FBWIN] %s: %s\n", what, SDL_GetError());
    fflush(stderr);
    _exit(1);
}

static void check_present_error(void)
{
    int error = atomic_load_explicit(&s_present_error, memory_order_acquire);
    if (error) {
        const char *why = error == PRESENT_ERROR_FORMAT
                        ? "unsupported framebuffer pitch"
                        : error == PRESENT_ERROR_DESCRIPTOR
                        ? "invalid or unmatched Metal framebuffer descriptor"
                        : error == PRESENT_ERROR_MEMORY
                        ? "framebuffer is inaccessible or outside mapped guest memory"
                        : "cannot allocate a framebuffer snapshot";
        fprintf(stderr, "  [FBWIN] %s\n", why);
        fflush(stderr);
        _exit(1);
    }
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    pthread_t guest;
    uint32_t tex_bpp = 0, tex_width = 0, tex_height = 0;
    uint64_t shown_seq = 0, t_title = 0, flips_title = 0;
    const char *e, *driver;
    int vsync, scale, dirty = 1, sdl_started = 0, rc;

#if defined(__APPLE__)
    if (!pthread_main_np()) {
        fprintf(stderr, "  [FBWIN] the host window must run on the process main thread\n");
        return 1;
    }
#endif
    if (!guest_main || s_window_started) {
        fprintf(stderr, "  [FBWIN] invalid or repeated host window invocation\n");
        return 1;
    }
    /* Ctrl-C and SIGTERM keep their default meaning; SDL would turn them
     * into an SDL_QUIT that a headless run never reads. Set before the
     * headless return: audio and input start SDL subsystems there too. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!env_on("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS"))
        return guest_main();

    e = getenv("RECOMP_PRESENT_VSYNC");
    vsync = !e || env_on("RECOMP_PRESENT_VSYNC");
    e = getenv("RECOMP_WINDOW_SCALE");
    scale = e ? atoi(e) : 2;
    if (scale < 1 || scale > 8)
        scale = 2;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    if (SDL_Init(FBWIN_SDL_SUBSYSTEMS) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video unavailable: %s\n", SDL_GetError());
        return 1;
    }
    sdl_started = 1;
    driver = SDL_GetCurrentVideoDriver();
    if (!driver || !strcmp(driver, "dummy") || !strcmp(driver, "offscreen")) {
        fprintf(stderr, "  [FBWIN] a visible window is required (SDL video driver: %s)\n",
                driver ? driver : "unavailable");
        goto startup_failed;
    }
    win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           (int)FB_W * scale, (int)FB_H * scale,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "  [FBWIN] cannot create a visible SDL window: %s\n", SDL_GetError());
        goto startup_failed;
    }
    {
        Uint32 flags = SDL_GetWindowFlags(win);
        if (!(flags & SDL_WINDOW_SHOWN) || (flags & SDL_WINDOW_HIDDEN)) {
            fprintf(stderr, "  [FBWIN] SDL created a window that is not visible\n");
            goto startup_failed;
        }
    }
    ren = SDL_CreateRenderer(win, -1, vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    if (!ren || SDL_RenderSetLogicalSize(ren, (int)FB_W, (int)FB_H) != 0) {
        fprintf(stderr, "  [FBWIN] cannot create the SDL renderer: %s\n", SDL_GetError());
        goto startup_failed;
    }
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s renderer,"
                    " vsync %s); guest runs on a pthread\n",
                    FB_W, FB_H, ri.name, vsync ? "on" : "off");
    }

    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [FBWIN] cannot register SDL events: %s\n", SDL_GetError());
        goto startup_failed;
    }
    s_frame_event = s_guest_done_event + 1;
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    if (env_on("BW_SYNTHETIC_INPUT_DIAGNOSTIC")) {
        const SDL_Keycode keys[] = {SDLK_RETURN, SDLK_DOWN, SDLK_z};
        SDL_Event event;
        xbox_InputSetMainPump(1);
        xbox_InputSdlInit();
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            memset(&event, 0, sizeof event);
            event.type = SDL_KEYDOWN;
            event.key.keysym.sym = keys[i];
            if (SDL_PushEvent(&event) != 1) {
                fprintf(stderr, "  [FBWIN] cannot enqueue synthetic input: %s\n", SDL_GetError());
                goto startup_failed;
            }
        }
        int count = 0, got = 0;
        unsigned seen = 0;
        while (count < 256 &&
               (got = SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_KEYDOWN, SDL_KEYDOWN)) == 1) {
            xbox_InputSdlEvent(&event);
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
                if (event.key.keysym.sym == keys[i] && !event.key.repeat &&
                    !(event.key.keysym.mod & KMOD_GUI))
                    seen |= 1u << i;
            count++;
        }
        if (got < 0 || seen != 7u || count == 256) {
            fprintf(stderr, "  [FBWIN] cannot process synthetic input events: %s\n", SDL_GetError());
            goto startup_failed;
        }
        fprintf(stderr, "  [FBWIN] synthetic SDL key presses processed\n");
    }
#endif
    s_guest_main = guest_main;
    rc = start_guest(&guest);
    if (rc != 0) {
        fprintf(stderr, "  [FBWIN] cannot start the guest thread: %s\n", strerror(rc));
        goto startup_failed;
    }
    s_window_started = 1;
    /* This thread pumps SDL events from here on, which also updates the
     * game controllers; the guest waits until input initialization completes. */
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    atomic_store_explicit(&s_running, 1, memory_order_seq_cst);
    pthread_mutex_lock(&s_guest_gate_lock);
    s_guest_ready = 1;
    pthread_cond_signal(&s_guest_gate);
    pthread_mutex_unlock(&s_guest_gate_lock);

    for (;;) {
        SDL_Event ev;
        uint64_t seq, now;
        const uint8_t *px = NULL;
        uint32_t bpp = 0, pitch = 0, width = 0, height = 0;

        /* Timeouts also observe a guest return or failure if SDL drops an
         * event because its queue is full. */
        if (SDL_WaitEventTimeout(&ev, shown_seq ? 100 : 16)) {
            do {
                /* Keys for the pad stand-in, and the window events that
                 * release them; it only reads the event. */
                xbox_InputSdlEvent(&ev);
                if (ev.type == SDL_QUIT) {
                    fprintf(stderr, "  [FBWIN] window closed\n");
                    fflush(stderr);
                    _exit(0);
                } else if (ev.type == s_frame_event)
                    atomic_store_explicit(&s_wake_pending, 0, memory_order_release);
                else if (ev.type == SDL_WINDOWEVENT
                         && (ev.window.event == SDL_WINDOWEVENT_EXPOSED
                             || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
                    dirty = 1;
            } while (SDL_PollEvent(&ev));
        }

        check_present_error();
        pthread_mutex_lock(&s_lock);
        seq = s_ready_seq;
        if (seq != shown_seq) {
            int t = s_front;
            s_front = s_ready;
            s_ready = t;
        }
        pthread_mutex_unlock(&s_lock);

        if (seq != shown_seq) {
            shown_seq = seq;
            px = s_slot[s_front].px;
            bpp = s_slot[s_front].bpp;
            width = s_slot[s_front].width;
            height = s_slot[s_front].height;
            pitch = width * bpp;
        }
        if (px) {
            if (!tex || bpp != tex_bpp || width != tex_width || height != tex_height) {
                SDL_Texture *next = SDL_CreateTexture(ren, bpp == 4 ? SDL_PIXELFORMAT_RGB888
                                                                     : SDL_PIXELFORMAT_RGB565,
                                                       SDL_TEXTUREACCESS_STREAMING,
                                                       (int)width, (int)height);
                if (!next)
                    runtime_failure("cannot create the framebuffer texture");
                if (tex)
                    SDL_DestroyTexture(tex);
                tex = next;
                tex_bpp = bpp;
                tex_width = width;
                tex_height = height;
            }
            if (SDL_UpdateTexture(tex, NULL, px, (int)pitch) != 0)
                runtime_failure("cannot update the framebuffer texture");
            dirty = 1;
        }
        if (dirty) {
            dirty = 0;
            if (SDL_SetRenderDrawColor(ren, 0, 0, 0, 255) != 0 ||
                SDL_RenderClear(ren) != 0 ||
                (tex && SDL_RenderCopy(ren, tex, NULL, NULL) != 0))
                runtime_failure("cannot render the framebuffer");
            SDL_RenderPresent(ren);
        }

        now = now_ms();
        if (now - t_title >= 1000) {
            uint64_t f = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
            char title[sizeof s_title];
            char tb[192];
            pthread_mutex_lock(&s_lock);
            memcpy(title, s_title, sizeof title);
            pthread_mutex_unlock(&s_lock);
            snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %u", title,
                     t_title ? (double)(f - flips_title) * 1000.0 / (double)(now - t_title)
                             : 0.0,
                     (unsigned)__atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED));
            SDL_SetWindowTitle(win, tb);
            t_title = now;
            flips_title = f;
        }
        if (atomic_load_explicit(&s_guest_done, memory_order_acquire))
            break;
    }

    /* No frame copy may still touch guest memory when the host resumes. */
    atomic_store_explicit(&s_running, 0, memory_order_seq_cst);
    while (atomic_load_explicit(&s_in_present, memory_order_seq_cst))
        sched_yield();
    rc = pthread_join(guest, NULL);
    if (rc != 0) {
        fprintf(stderr, "  [FBWIN] cannot join guest thread: %s\n", strerror(rc));
        fflush(stderr);
        _exit(1);
    }
    check_present_error();
    fprintf(stderr, "  [FBWIN] guest returned\n");
    fflush(stderr);

    /* Detached guest/USB workers may still use SDL and guest memory. The host
     * stops those it can before unmapping, or exits the process without
     * teardown. Leave the main-pump flag and all SDL resources intact. */
    return s_guest_rc;

startup_failed:
    if (ren)
        SDL_DestroyRenderer(ren);
    if (win)
        SDL_DestroyWindow(win);
    if (sdl_started)
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
    return 1;
}

#endif /* !_WIN32 */
