#include "bw_macos_input_bridge.h"
#include "keyboard.h"
#include "keyboard_sdl.h"

#include <SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE 0x200000u
#define TYPE_AT 0x16CEB8u
#define STACK_AT 0x1F0000u
#define OUTPUT_AT 0x1E0001u

static uint8_t s_memory[MEM_SIZE];
ptrdiff_t g_xbox_mem_offset;
_Thread_local uint32_t g_eax, g_esp;
static unsigned s_original_calls[8];
static int s_failed;

extern void xbox_InputSetMainPump(int on);

void *xbox_GetMemoryBase(void) { return s_memory; }
size_t xbox_GetMappedSize(void) { return sizeof s_memory; }

#define ORIGINAL(address, index, count) \
    void sub_##address##_gen(void) \
    { s_original_calls[index]++; g_eax = 0xABC00000u + index; g_esp += 4u * (count + 1u); } \
    void sub_##address(void)
ORIGINAL(0016F121, 0, 2);
ORIGINAL(0016F126, 1, 1);
ORIGINAL(0016F148, 2, 3);
ORIGINAL(0016DC16, 3, 4);
ORIGINAL(0016DC8B, 4, 1);
ORIGINAL(0016DC97, 5, 2);
ORIGINAL(0016DE89, 6, 2);
ORIGINAL(0016DEFA, 7, 2);
#undef ORIGINAL

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        s_failed = 1;
    }
}

static uint32_t read32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, s_memory + address, sizeof value);
    return value;
}

static void write32(uint32_t address, uint32_t value)
{
    memcpy(s_memory + address, &value, sizeof value);
}

static uint32_t call(void (*function)(void), unsigned count, const uint32_t *args)
{
    g_esp = STACK_AT;
    write32(STACK_AT, 0x12345678u);
    for (unsigned i = 0; i < count; i++)
        write32(STACK_AT + 4u + 4u * i, args[i]);
    g_eax = 0xBAD00000u;
    function();
    check(g_esp == STACK_AT + 4u * (count + 1u), "stdcall stack cleanup");
    return g_eax;
}

typedef struct {
    uint32_t handle;
    unsigned index;
    atomic_int *failures;
} PollThread;

static void *poll_thread(void *opaque)
{
    PollThread *thread = opaque;
    uint32_t stack = STACK_AT - 128u * (thread->index + 1u);
    uint32_t output = OUTPUT_AT - 64u * (thread->index + 1u);
    for (unsigned i = 0; i < 4000; i++) {
        XBOX_GAMEPAD pad;
        g_esp = stack;
        write32(stack, 0x12345678u);
        write32(stack + 4, thread->handle);
        write32(stack + 8, output);
        sub_0016DE89();
        memcpy(&pad, s_memory + output + 4, sizeof pad);
        if (g_eax != 0 || g_esp != stack + 12 ||
            (pad.wButtons & ~XBOX_GAMEPAD_START) != 0)
            atomic_fetch_add(thread->failures, 1);
    }
    return NULL;
}

static void *key_thread(void *opaque)
{
    (void)opaque;
    for (unsigned i = 0; i < 4000; i++)
        xbox_InputKeySet(XBOX_VK_RETURN, i & 1);
    return NULL;
}

static void concurrent_test(uint32_t handle)
{
    pthread_t threads[4];
    PollThread polls[3];
    atomic_int failures = 0;
    unsigned started = 0;
    xbox_InputKeysClear();
    for (unsigned i = 0; i < 3; i++) {
        polls[i] = (PollThread){handle, i, &failures};
        if (pthread_create(&threads[started], NULL, poll_thread, &polls[i]) != 0) {
            check(0, "create polling thread");
            break;
        }
        started++;
    }
    if (pthread_create(&threads[started], NULL, key_thread, NULL) == 0)
        started++;
    else
        check(0, "create keyboard writer thread");
    for (unsigned i = 0; i < started; i++)
        check(pthread_join(threads[i], NULL) == 0, "join input thread");
    check(atomic_load(&failures) == 0, "concurrent polling and keyboard updates");
    xbox_InputKeysClear();
}

static int push_event(Uint32 type, SDL_Keycode key, Uint8 window_event)
{
    SDL_Event event = {0};
    event.type = type;
    if (type == SDL_WINDOWEVENT)
        event.window.event = window_event;
    else
        event.key.keysym.sym = key;
    if (SDL_PushEvent(&event) != 1) {
        fprintf(stderr, "FAIL: enqueue SDL input event (%s)\n", SDL_GetError());
        s_failed = 1;
        return 0;
    }
    return 1;
}

static void pump_queued_events(unsigned expected)
{
    SDL_Event event;
    unsigned seen = 0;
    while (SDL_PollEvent(&event)) {
        xbox_InputSdlEvent(&event);
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
            event.type == SDL_WINDOWEVENT)
            seen++;
    }
    check(seen == expected, "SDL input events reach the presenter event handler");
}

static void queued_keyboard_test(uint32_t handle)
{
    uint32_t args[2] = {handle, OUTPUT_AT};
    XBOX_GAMEPAD pad;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "FAIL: initialize SDL input test (%s)\n", SDL_GetError());
        s_failed = 1;
        return;
    }
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    xbox_InputKeysClear();
    if (!push_event(SDL_KEYDOWN, SDLK_RETURN, 0) ||
        !push_event(SDL_KEYDOWN, SDLK_DOWN, 0) ||
        !push_event(SDL_KEYDOWN, SDLK_z, 0))
        goto cleanup;
    pump_queued_events(3);
    check(call(sub_0016DE89, 2, args) == 0, "poll queued SDL key presses");
    memcpy(&pad, s_memory + OUTPUT_AT + 4, sizeof pad);
    check((pad.wButtons & (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN)) ==
          (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN) &&
          pad.bAnalogButtons[XBOX_BUTTON_A] == 255,
          "queued SDL key presses reach the guest input hook");
    uint32_t packet = read32(OUTPUT_AT);

    if (!push_event(SDL_KEYUP, SDLK_RETURN, 0) ||
        !push_event(SDL_KEYUP, SDLK_DOWN, 0) ||
        !push_event(SDL_KEYUP, SDLK_z, 0))
        goto cleanup;
    pump_queued_events(3);
    check(call(sub_0016DE89, 2, args) == 0, "poll queued SDL key releases");
    memcpy(&pad, s_memory + OUTPUT_AT + 4, sizeof pad);
    check(pad.wButtons == 0 && pad.bAnalogButtons[XBOX_BUTTON_A] == 0 &&
          read32(OUTPUT_AT) != packet, "queued key releases reach the guest input hook");

    if (!push_event(SDL_KEYDOWN, SDLK_RETURN, 0) ||
        !push_event(SDL_WINDOWEVENT, 0, SDL_WINDOWEVENT_FOCUS_LOST))
        goto cleanup;
    pump_queued_events(2);
    check(call(sub_0016DE89, 2, args) == 0, "poll after queued focus loss");
    memcpy(&pad, s_memory + OUTPUT_AT + 4, sizeof pad);
    check((pad.wButtons & XBOX_GAMEPAD_START) == 0,
          "queued focus loss releases the guest input state");

cleanup:
    xbox_InputKeysClear();
    xbox_InputSetMainPump(0);
    SDL_Quit();
}

static void disabled_test(void)
{
    void (*functions[8])(void) = {
        sub_0016F121, sub_0016F126, sub_0016F148, sub_0016DC16,
        sub_0016DC8B, sub_0016DC97, sub_0016DE89, sub_0016DEFA,
    };
    const unsigned counts[8] = {2, 1, 3, 4, 1, 2, 2, 2};
    uint32_t args[4] = {0, 0, 0, 0};
    bw_macos_input_bridge_configure(0);
    for (unsigned i = 0; i < 8; i++) {
        check(call(functions[i], counts[i], args) == 0xABC00000u + i,
              "disabled hook preserves original result");
        check(s_original_calls[i] == 1, "disabled hook calls original body");
    }
}

static void enabled_test(void)
{
    uint32_t args[4] = {0};
    uint32_t handle, next;
    XBOX_GAMEPAD pad;
    bw_macos_input_bridge_configure(1);
    check(call(sub_0016F121, 2, args) == 0, "initialize");
    args[0] = TYPE_AT;
    write32(TYPE_AT + 4, 0xFFFFFFFFu);
    write32(TYPE_AT + 8, 0xFFFFFFFFu);
    check(call(sub_0016F126, 1, args) == 1, "enumerate keyboard on port zero");
    check(read32(TYPE_AT + 4) == 0 && read32(TYPE_AT + 8) == 1,
          "enumeration updates device-type bookkeeping");
    args[1] = OUTPUT_AT;
    args[2] = OUTPUT_AT + 4;
    write32(args[1], 0xFFFFFFFFu);
    write32(args[2], 0xFFFFFFFFu);
    check(call(sub_0016F148, 3, args) == 0, "no device changes");
    check(read32(args[1]) == 0 && read32(args[2]) == 0, "change masks are zero");

    args[1] = args[2] = args[3] = 0;
    handle = call(sub_0016DC16, 4, args);
    check(handle != 0, "open port zero");
    check(call(sub_0016DC16, 4, args) == 0, "duplicate open rejected");

    args[0] = handle;
    args[1] = OUTPUT_AT;
    memset(s_memory + OUTPUT_AT - 1, 0xA5, 27);
    check(call(sub_0016DC97, 2, args) == 0, "query capabilities");
    check(s_memory[OUTPUT_AT - 1] == 0xA5 && s_memory[OUTPUT_AT + 25] == 0xA5 &&
          s_memory[OUTPUT_AT] == 2, "capability layout and exact write span");
    static const uint8_t expected_caps[25] = {
        2, 0, 0, 255, 0, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0
    };
    check(memcmp(s_memory + OUTPUT_AT, expected_caps, sizeof expected_caps) == 0,
          "capability bytes advertise keyboard inputs without unsupported feedback");

    xbox_InputKeysClear();
    xbox_InputKeySet(XBOX_VK_RETURN, 1);
    xbox_InputKeySet(XBOX_VK_DOWN, 1);
    xbox_InputKeySet('Z', 1);
    memset(s_memory + OUTPUT_AT - 1, 0xA5, 24);
    check(call(sub_0016DE89, 2, args) == 0, "poll keyboard state");
    check(s_memory[OUTPUT_AT - 1] == 0xA5 && s_memory[OUTPUT_AT + 22] == 0xA5,
          "state writes exactly 22 bytes");
    memcpy(&pad, s_memory + OUTPUT_AT + 4, sizeof pad);
    check((pad.wButtons & (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN)) ==
          (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN), "digital keys in guest state");
    check(pad.bAnalogButtons[XBOX_BUTTON_A] == 255, "analog A key in guest state");
    uint32_t packet = read32(OUTPUT_AT);
    xbox_InputKeysClear();
    check(call(sub_0016DE89, 2, args) == 0, "poll released keys");
    memcpy(&pad, s_memory + OUTPUT_AT + 4, sizeof pad);
    check(pad.wButtons == 0 && pad.bAnalogButtons[XBOX_BUTTON_A] == 0 &&
          read32(OUTPUT_AT) != packet, "release and packet update");

#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    bw_macos_input_bridge_trace_synthetic(1);
#endif
    queued_keyboard_test(handle);
    concurrent_test(handle);
    args[1] = MEM_SIZE - 4u;
    check(call(sub_0016DE89, 2, args) == 87, "invalid state span rejected");
    args[1] = OUTPUT_AT;
    check(call(sub_0016DEFA, 2, args) == 50, "feedback reports unsupported");
    args[0] = handle;
    call(sub_0016DC8B, 1, args);
    check(call(sub_0016DE89, 2, args) == 1167, "closed handle rejected");
    args[0] = TYPE_AT;
    args[1] = args[2] = args[3] = 0;
    next = call(sub_0016DC16, 4, args);
    check(next && next != handle, "reopen produces a new handle generation");
    args[0] = handle;
    args[1] = OUTPUT_AT;
    check(call(sub_0016DE89, 2, args) == 1167, "stale handle rejected after reopen");
    args[0] = next;
    call(sub_0016DC8B, 1, args);

    args[0] = TYPE_AT + 0x40u;
    write32(args[0] + 8, 0xFFFFFFFFu);
    check(call(sub_0016F126, 1, args) == 0 && read32(args[0] + 8) == 0,
          "other device types do not appear connected");
    for (unsigned i = 0; i < 8; i++)
        check(s_original_calls[i] == 1, "enabled hook does not invoke USB implementation");
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)s_memory;
    disabled_test();
    enabled_test();
    return s_failed ? 1 : 0;
}
