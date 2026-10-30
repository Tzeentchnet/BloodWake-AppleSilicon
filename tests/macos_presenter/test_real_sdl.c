#include <SDL.h>

#include "bw_gamma.h"
#include "xinput_xbox.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 640u
#define HEIGHT 480u
#define PITCH (WIDTH * 4u)
#define FRAME_AT 0x1000u

int xbox_HostWindowMain(int (*guest_main)(void));
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch);

static _Alignas(16) uint8_t s_memory[2u * 1024u * 1024u];
static atomic_int s_guest_calls;
static int s_headless;

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)(uintptr_t)s_memory; }
void *xbox_GetMemoryBase(void) { return s_memory; }
size_t xbox_GetMappedSize(void) { return sizeof s_memory; }
int bw_xbox_contiguous_memory_region(void **host, uint32_t *va, size_t *bytes)
{
    (void)host; (void)va; (void)bytes;
    return 0;
}
int bw_audit_on(void) { return 0; }
void bw_audit_gamma(const uint8_t ramp[768]) { (void)ramp; }

static void wait_ms(unsigned ms)
{
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

static int wait_start(int pressed)
{
    for (unsigned i = 0; i < 200; i++) {
        XBOX_INPUT_STATE state;
        if (xbox_InputGetState(0, &state) == 0 &&
            !!(state.Gamepad.wButtons & XBOX_GAMEPAD_START) == pressed)
            return 1;
        wait_ms(5);
    }
    return 0;
}

static int guest_main(void)
{
    SDL_Event event;
    uint32_t pixel = 0xFF112233u, after;
    atomic_fetch_add(&s_guest_calls, 1);
    if (s_headless)
        return pthread_main_np() ? 37 : 1;
    if (pthread_main_np() ||
        (SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) &
         (SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) !=
         (SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER))
        return 2;
    memcpy(s_memory + FRAME_AT, &pixel, sizeof pixel);
    xbox_FramebufferWindowPresent(FRAME_AT, PITCH);
    memset(&event, 0, sizeof event);
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_RETURN;
    if (SDL_PushEvent(&event) != 1 || !wait_start(1))
        return 3;
    memset(&event, 0, sizeof event);
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    if (SDL_PushEvent(&event) != 1 || !wait_start(0))
        return 4;
    memcpy(&after, s_memory + FRAME_AT, sizeof after);
    if (after != pixel)
        return 5;
    wait_ms(50);
    return 37;
}

int main(int argc, char **argv)
{
    uint8_t ramp[768];
    int rc, ok;
    if (argc != 2)
        return 2;
    setenv("RECOMP_FB_WINDOW", "1", 1);
    setenv("RECOMP_KEYBOARD", "1", 1);
    setenv("BW_GPU_GAMMA", "1", 1);
    unsetenv("BW_GPU_AB");
    s_headless = !strcmp(argv[1], "headless");
    if (s_headless) {
        setenv("RECOMP_HEADLESS", "1", 1);
    } else {
        unsetenv("RECOMP_HEADLESS");
        if (!strcmp(argv[1], "dummy"))
            setenv("SDL_VIDEODRIVER", "dummy", 1);
        else
            unsetenv("SDL_VIDEODRIVER");
    }
    for (unsigned i = 0; i < 256; i++) {
        ramp[i] = (uint8_t)(255u - i);
        ramp[256 + i] = (uint8_t)i;
        ramp[512 + i] = (uint8_t)i;
    }
    bw_gamma_from_title(ramp);
    rc = xbox_HostWindowMain(guest_main);
    ok = !strcmp(argv[1], "dummy")
       ? rc != 0 && !atomic_load(&s_guest_calls)
       : rc == 37 && atomic_load(&s_guest_calls) == 1;
    if (!ok)
        fprintf(stderr, "real SDL %s failed: rc=%d guest_calls=%d (%s)\n",
                argv[1], rc, atomic_load(&s_guest_calls), SDL_GetError());
    fflush(stderr);
    _exit(ok ? 0 : 1);
}
