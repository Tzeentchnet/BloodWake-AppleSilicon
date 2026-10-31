#include "bw_hook_platform.h"
#include "bw_macos_input_bridge.h"
#include "keyboard.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern BW_HOOK_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern void *xbox_GetMemoryBase(void);
extern size_t xbox_GetMappedSize(void);

extern void sub_0016DC16_gen(void);
extern void sub_0016DC8B_gen(void);
extern void sub_0016DC97_gen(void);
extern void sub_0016DE89_gen(void);
extern void sub_0016DEFA_gen(void);
extern void sub_0016F121_gen(void);
extern void sub_0016F126_gen(void);
extern void sub_0016F148_gen(void);

static const uint32_t BW_GAMEPAD_TYPE = 0x0016CEB8u;
static const uint32_t BW_HANDLE_PREFIX = 0xE1000000u;
static const uint32_t BW_HANDLE_LIMIT = 0x00FFFFFFu;
static const uint32_t BW_ERROR_INVALID_PARAMETER = 87u;
static const uint32_t BW_ERROR_NOT_SUPPORTED = 50u;
static const uint32_t BW_ERROR_DEVICE_NOT_CONNECTED = 1167u;

static atomic_int s_enabled;
static atomic_uint s_observed;
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
static atomic_int s_trace_synthetic;

void bw_macos_input_bridge_trace_synthetic(int enabled)
{
    atomic_store_explicit(&s_trace_synthetic, enabled != 0, memory_order_release);
}
#endif
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_generation, s_handle;
static int s_initialized;

void bw_macos_input_bridge_configure(int enabled)
{
    atomic_store_explicit(&s_enabled, enabled != 0, memory_order_release);
}

static int enabled(void)
{
    return atomic_load_explicit(&s_enabled, memory_order_acquire);
}

static void observed(unsigned bit, const char *operation)
{
    unsigned mask = 1u << bit;
    if (!(atomic_fetch_or_explicit(&s_observed, mask, memory_order_relaxed) & mask))
        fprintf(stderr, "[BW-INPUT] experimental hook observed %s\n", operation);
}

static void *guest_span(uint32_t address, size_t length)
{
    size_t mapped = xbox_GetMappedSize();
    if (!address || !length || !xbox_GetMemoryBase() ||
        (uint64_t)address + length > mapped ||
        (uintptr_t)xbox_GetMemoryBase() != (uintptr_t)g_xbox_mem_offset)
        return NULL;
    return (void *)((uintptr_t)g_xbox_mem_offset + address);
}

static void arguments(uint32_t *args, unsigned count)
{
    size_t bytes = ((size_t)count + 1u) * sizeof(uint32_t);
    uint8_t *stack = guest_span(g_esp, bytes);
    if (!stack) {
        fprintf(stderr, "[BW-INPUT] invalid guest stack for experimental input hook\n");
        _Exit(1);
    }
    memcpy(args, stack + sizeof(uint32_t), count * sizeof(uint32_t));
}

static void finish(unsigned count, uint32_t result)
{
    g_eax = result;
    g_esp += (count + 1u) * sizeof(uint32_t);
}

static void invalid(const char *operation)
{
    fprintf(stderr, "[BW-INPUT] invalid or unsupported %s in experimental input hook\n",
            operation);
}

void sub_0016F121(void)
{
    uint32_t args[2];
    if (!enabled()) {
        sub_0016F121_gen();
        return;
    }
    observed(0, "device initialization");
    arguments(args, 2);
    if (args[0] || args[1]) {
        invalid("device initialization parameters");
        _Exit(1);
    }
    pthread_mutex_lock(&s_lock);
    s_initialized = 1;
    s_handle = 0;
    pthread_mutex_unlock(&s_lock);
    finish(2, 0);
}

void sub_0016F126(void)
{
    uint32_t args[1], mask = 0;
    void *type;
    if (!enabled()) {
        sub_0016F126_gen();
        return;
    }
    observed(1, "device enumeration");
    arguments(args, 1);
    type = guest_span(args[0], 3u * sizeof(uint32_t));
    if (!type) {
        invalid("device type");
    } else {
        pthread_mutex_lock(&s_lock);
        mask = s_initialized && args[0] == BW_GAMEPAD_TYPE ? 1u : 0u;
        pthread_mutex_unlock(&s_lock);
        memcpy((uint8_t *)type + 4, &(uint32_t){0}, sizeof(uint32_t));
        memcpy((uint8_t *)type + 8, &mask, sizeof mask);
    }
    finish(1, mask);
}

void sub_0016F148(void)
{
    uint32_t args[3], zero = 0;
    void *insertions, *removals;
    if (!enabled()) {
        sub_0016F148_gen();
        return;
    }
    observed(2, "device change query");
    arguments(args, 3);
    insertions = guest_span(args[1], sizeof zero);
    removals = guest_span(args[2], sizeof zero);
    if (!guest_span(args[0], 3u * sizeof(uint32_t)) || !insertions || !removals) {
        invalid("device change query");
    } else {
        memcpy(insertions, &zero, sizeof zero);
        memcpy(removals, &zero, sizeof zero);
    }
    finish(3, 0);
}

void sub_0016DC16(void)
{
    uint32_t args[4], handle = 0;
    if (!enabled()) {
        sub_0016DC16_gen();
        return;
    }
    observed(3, "controller open");
    arguments(args, 4);
    pthread_mutex_lock(&s_lock);
    if (s_initialized && args[0] == BW_GAMEPAD_TYPE && args[1] == 0 &&
        args[2] == 0 && args[3] == 0 && !s_handle && s_generation < BW_HANDLE_LIMIT)
        handle = s_handle = BW_HANDLE_PREFIX | ++s_generation;
    pthread_mutex_unlock(&s_lock);
    if (!handle)
        invalid("controller open");
    finish(4, handle);
}

void sub_0016DC8B(void)
{
    uint32_t args[1];
    int valid;
    if (!enabled()) {
        sub_0016DC8B_gen();
        return;
    }
    observed(4, "controller close");
    arguments(args, 1);
    pthread_mutex_lock(&s_lock);
    valid = s_handle && args[0] == s_handle;
    if (valid)
        s_handle = 0;
    pthread_mutex_unlock(&s_lock);
    if (!valid)
        invalid("controller close");
    finish(1, 0);
}

void sub_0016DC97(void)
{
    uint32_t args[2], result = BW_ERROR_DEVICE_NOT_CONNECTED;
    uint8_t caps[25] = {0};
    void *destination;
    if (!enabled()) {
        sub_0016DC97_gen();
        return;
    }
    observed(5, "capability query");
    arguments(args, 2);
    destination = guest_span(args[1], sizeof caps);
    if (!destination) {
        invalid("capabilities destination");
        finish(2, BW_ERROR_INVALID_PARAMETER);
        return;
    }
    pthread_mutex_lock(&s_lock);
    if (s_handle && args[0] == s_handle) {
        caps[0] = 2;
        /* Leave the output-motor capability bytes clear. */
        memset(caps + 3, 0xFF, 18);
        caps[4] = 0;
        memcpy(destination, caps, sizeof caps);
        result = 0;
    }
    pthread_mutex_unlock(&s_lock);
    if (result)
        invalid("capabilities handle");
    finish(2, result);
}

void sub_0016DE89(void)
{
    uint32_t args[2], result = BW_ERROR_DEVICE_NOT_CONNECTED;
    void *destination;
    XBOX_INPUT_STATE state;
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    int synthetic_seen = 0;
#endif
    _Static_assert(sizeof(XBOX_GAMEPAD) == 18, "unexpected Xbox gamepad layout");
    if (!enabled()) {
        sub_0016DE89_gen();
        return;
    }
    observed(6, "controller state query");
    arguments(args, 2);
    destination = guest_span(args[1], sizeof state.dwPacketNumber + sizeof state.Gamepad);
    if (!destination) {
        invalid("controller state destination");
        finish(2, BW_ERROR_INVALID_PARAMETER);
        return;
    }
    pthread_mutex_lock(&s_lock);
    if (s_handle && args[0] == s_handle) {
        xbox_InputKeyboardState(&state);
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
        synthetic_seen = atomic_load_explicit(&s_trace_synthetic, memory_order_acquire) &&
            (state.Gamepad.wButtons & (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN)) ==
                (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN) &&
            state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255;
#endif
        memcpy(destination, &state.dwPacketNumber, sizeof state.dwPacketNumber);
        memcpy((uint8_t *)destination + sizeof state.dwPacketNumber,
               &state.Gamepad, sizeof state.Gamepad);
        result = 0;
    }
    pthread_mutex_unlock(&s_lock);
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    if (synthetic_seen)
        observed(8, "synthetic Start/Down/A state");
#endif
    if (result)
        invalid("controller state handle");
    finish(2, result);
}

void sub_0016DEFA(void)
{
    uint32_t args[2], result = BW_ERROR_DEVICE_NOT_CONNECTED;
    if (!enabled()) {
        sub_0016DEFA_gen();
        return;
    }
    observed(7, "controller feedback");
    arguments(args, 2);
    pthread_mutex_lock(&s_lock);
    if (s_handle && args[0] == s_handle)
        result = BW_ERROR_NOT_SUPPORTED;
    pthread_mutex_unlock(&s_lock);
    invalid(result == BW_ERROR_NOT_SUPPORTED ? "controller feedback" : "feedback handle");
    finish(2, result);
}
