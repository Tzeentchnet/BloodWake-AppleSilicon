#include "bw_hook_platform.h"
#include "bw_macos_input_bridge.h"
#include "bw_macos_mouse.h"
#include "keyboard.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
enum { BW_MAX_GAMEPADS = 4 };

static atomic_int s_enabled;
static atomic_uint s_observed;
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
static atomic_int s_trace_synthetic;

typedef enum {
    BW_SCRIPT_START,
    BW_SCRIPT_DPAD_DOWN,
    BW_SCRIPT_A
} BwScriptButton;

typedef struct {
    uint32_t wait_epochs;
    uint32_t press_epochs;
    BwScriptButton button;
    uint8_t pad_mask;
} BwScriptStep;

enum {
    BW_SCRIPT_WAIT,
    BW_SCRIPT_PRESS,
    BW_SCRIPT_RELEASE
};

enum {
    BW_SCRIPT_PAD_ONE = 1u << 0,
    BW_SCRIPT_PAD_TWO = 1u << 1,
    BW_SCRIPT_PAD_BOTH = BW_SCRIPT_PAD_ONE | BW_SCRIPT_PAD_TWO
};

static const char s_script_name[] = "remastered-boot-menu-to-gameplay-v1";
static const char s_split_script_name[] = "remastered-boot-menu-to-gameplay-2p-v1";
static const BwScriptStep s_gameplay_script_steps[] = {
    {300, 4, BW_SCRIPT_START, BW_SCRIPT_PAD_ONE},
    {120, 3, BW_SCRIPT_DPAD_DOWN, BW_SCRIPT_PAD_ONE},
    { 12, 3, BW_SCRIPT_DPAD_DOWN, BW_SCRIPT_PAD_ONE},
    { 12, 3, BW_SCRIPT_DPAD_DOWN, BW_SCRIPT_PAD_ONE},
    { 12, 3, BW_SCRIPT_DPAD_DOWN, BW_SCRIPT_PAD_ONE},
    { 60, 4, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {120, 4, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    { 60, 3, BW_SCRIPT_DPAD_DOWN, BW_SCRIPT_PAD_ONE},
    { 30, 4, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {180, 4, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {180, 4, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {240, 4, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
};
static const BwScriptStep s_split_script_steps[] = {
    {600, 30, BW_SCRIPT_START, BW_SCRIPT_PAD_ONE},
    {180, 12, BW_SCRIPT_DPAD_DOWN, BW_SCRIPT_PAD_ONE},
    { 90, 12, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {180, 12, BW_SCRIPT_DPAD_DOWN, BW_SCRIPT_PAD_ONE},
    { 60, 12, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {180, 12, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {180, 12, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {240, 12, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {180, 12, BW_SCRIPT_A, BW_SCRIPT_PAD_ONE},
    {240, 30, BW_SCRIPT_A, BW_SCRIPT_PAD_TWO},
    {180, 30, BW_SCRIPT_A, BW_SCRIPT_PAD_TWO},
};
static struct {
    const BwScriptStep *steps;
    size_t count;
    size_t step;
    uint32_t remaining;
    int phase;
    int active;
} s_script;

typedef struct {
    uint16_t digital;
    uint8_t analog_mask;
    uint64_t until_ms;
} BwGuidedPadState;

static struct {
    char path[1024];
    long offset;
    uint64_t started_ms;
    uint64_t last_poll_ms;
    BwGuidedPadState pad[BW_MAX_GAMEPADS];
    uint16_t recorded_buttons;
    uint8_t recorded_a;
    uint8_t recorded_b;
    int recorded;
} s_guided;

void bw_macos_input_bridge_trace_synthetic(int enabled)
{
    atomic_store_explicit(&s_trace_synthetic, enabled != 0, memory_order_release);
}
#endif
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_generation, s_handles[BW_MAX_GAMEPADS];
static uint32_t s_packets[BW_MAX_GAMEPADS];
static unsigned s_pad_count = 1;
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
static unsigned s_requested_pad_count = 1;
#endif
static int s_initialized;

#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
static const char *script_button_name(BwScriptButton button)
{
    switch (button) {
    case BW_SCRIPT_START: return "Start";
    case BW_SCRIPT_DPAD_DOWN: return "D-pad Down";
    case BW_SCRIPT_A: return "A";
    }
    return "?";
}

static void cancel_script_locked(void)
{
    memset(&s_script, 0, sizeof s_script);
}

static uint64_t monotonic_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
        return 0;
    return (uint64_t)time.tv_sec * 1000u + (uint64_t)time.tv_nsec / 1000000u;
}

static void reset_guided_locked(void)
{
    memset(&s_guided, 0, sizeof s_guided);
}

static void configure_guided_locked(void)
{
    const char *path = getenv("BW_MACOS_GUIDED_INPUT");
    FILE *file;

    reset_guided_locked();
    if (!path || !*path)
        return;
    if (!getenv("BW_NATIVE_DIAGNOSTIC") || path[0] != '/' ||
        strlen(path) >= sizeof s_guided.path) {
        fprintf(stderr, "[BW-INPUT] invalid guided input configuration\n");
        _Exit(1);
    }
    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[BW-INPUT] cannot open guided input file %s\n", path);
        _Exit(1);
    }
    fclose(file);
    memcpy(s_guided.path, path, strlen(path) + 1);
    s_guided.started_ms = monotonic_ms();
    s_requested_pad_count = 2;
    fprintf(stderr, "[BW-INPUT] guided two-pad input from %s\n", path);
}

static int parse_guided_button(const char *name, uint16_t *digital,
                               uint8_t *analog_mask)
{
    static const struct {
        const char *name;
        uint16_t digital;
        int analog;
    } buttons[] = {
        {"up", XBOX_GAMEPAD_DPAD_UP, -1},
        {"down", XBOX_GAMEPAD_DPAD_DOWN, -1},
        {"left", XBOX_GAMEPAD_DPAD_LEFT, -1},
        {"right", XBOX_GAMEPAD_DPAD_RIGHT, -1},
        {"start", XBOX_GAMEPAD_START, -1},
        {"back", XBOX_GAMEPAD_BACK, -1},
        {"a", 0, XBOX_BUTTON_A},
        {"b", 0, XBOX_BUTTON_B},
    };
    size_t index;

    for (index = 0; index < sizeof buttons / sizeof buttons[0]; index++) {
        if (strcmp(name, buttons[index].name))
            continue;
        *digital = buttons[index].digital;
        *analog_mask = buttons[index].analog < 0
                     ? 0u : (uint8_t)(1u << buttons[index].analog);
        return 1;
    }
    return 0;
}

static void guided_line_locked(char *line, uint64_t now)
{
    char *button = line;
    char *hold_text = strchr(line, ':');
    char *end;
    unsigned long hold_ms = 250;
    unsigned port = 0;
    uint16_t digital;
    uint8_t analog_mask;

    line[strcspn(line, "\r\n")] = 0;
    if (hold_text) {
        *hold_text++ = 0;
        hold_ms = strtoul(hold_text, &end, 10);
        if (!*hold_text || *end || hold_ms < 50 || hold_ms > 2000) {
            fprintf(stderr, "[BW-INPUT] invalid guided hold in \"%s\"\n", line);
            return;
        }
    }
    if ((button[0] == 'p' || button[0] == 'P') &&
        button[1] >= '1' && button[1] <= '4' && button[2] == '-') {
        port = (unsigned)(button[1] - '1');
        button += 3;
    }
    if (!parse_guided_button(button, &digital, &analog_mask)) {
        fprintf(stderr, "[BW-INPUT] invalid guided button \"%s\"\n", button);
        return;
    }
    s_guided.pad[port].digital = digital;
    s_guided.pad[port].analog_mask = analog_mask;
    s_guided.pad[port].until_ms = now + hold_ms;
    fprintf(stderr, "[BW-INPUT] guided p%u %s hold %lu ms at %llu ms\n",
            port + 1, button, hold_ms,
            (unsigned long long)(now - s_guided.started_ms));
}

static void poll_guided_locked(uint64_t now)
{
    FILE *file;
    char line[128];

    if (!s_guided.path[0] || now - s_guided.last_poll_ms < 50)
        return;
    s_guided.last_poll_ms = now;
    file = fopen(s_guided.path, "rb");
    if (!file) {
        fprintf(stderr, "[BW-INPUT] cannot reopen guided input file\n");
        return;
    }
    if (fseek(file, s_guided.offset, SEEK_SET) != 0) {
        fprintf(stderr, "[BW-INPUT] cannot seek guided input file\n");
        fclose(file);
        return;
    }
    while (fgets(line, sizeof line, file)) {
        long offset = ftell(file);
        if (!strchr(line, '\n') && !feof(file)) {
            fprintf(stderr, "[BW-INPUT] guided input line is too long\n");
            break;
        }
        guided_line_locked(line, now);
        if (offset >= 0)
            s_guided.offset = offset;
    }
    fclose(file);
}

static void apply_guided_locked(XBOX_INPUT_STATE *state, unsigned port,
                                uint64_t now)
{
    unsigned analog;
    BwGuidedPadState *guided;

    if (!s_guided.path[0])
        return;
    poll_guided_locked(now);
    guided = &s_guided.pad[port];
    if (now >= guided->until_ms)
        return;
    state->Gamepad.wButtons |= guided->digital;
    for (analog = 0; analog < 8; analog++)
        if (guided->analog_mask & (1u << analog))
            state->Gamepad.bAnalogButtons[analog] = 255;
}

static void record_guided_input_locked(const XBOX_INPUT_STATE *state,
                                       uint64_t now)
{
    const uint16_t mask = XBOX_GAMEPAD_DPAD_UP | XBOX_GAMEPAD_DPAD_DOWN |
                          XBOX_GAMEPAD_DPAD_LEFT | XBOX_GAMEPAD_DPAD_RIGHT |
                          XBOX_GAMEPAD_START | XBOX_GAMEPAD_BACK;
    uint16_t buttons;
    uint8_t a, b;

    if (!s_guided.path[0])
        return;
    buttons = state->Gamepad.wButtons & mask;
    a = state->Gamepad.bAnalogButtons[XBOX_BUTTON_A];
    b = state->Gamepad.bAnalogButtons[XBOX_BUTTON_B];
    if (s_guided.recorded && buttons == s_guided.recorded_buttons &&
        a == s_guided.recorded_a && b == s_guided.recorded_b)
        return;
    s_guided.recorded = 1;
    s_guided.recorded_buttons = buttons;
    s_guided.recorded_a = a;
    s_guided.recorded_b = b;
    fprintf(stderr,
            "[BW-INPUT] guided physical at %llu ms buttons=0x%04X a=%u b=%u\n",
            (unsigned long long)(now - s_guided.started_ms),
            buttons, a, b);
}

int bw_macos_input_bridge_select_synthetic_script(const char *name)
{
    int single = name && !strcmp(name, s_script_name);
    int split = name && !strcmp(name, s_split_script_name);
    int selected = single || split;
    pthread_mutex_lock(&s_lock);
    cancel_script_locked();
    s_requested_pad_count = split ? 2u : 1u;
    if (selected) {
        s_script.steps = split ? s_split_script_steps : s_gameplay_script_steps;
        s_script.count = split
                       ? sizeof s_split_script_steps / sizeof s_split_script_steps[0]
                       : sizeof s_gameplay_script_steps / sizeof s_gameplay_script_steps[0];
        s_script.remaining = s_script.steps[0].wait_epochs;
        s_script.phase = BW_SCRIPT_WAIT;
        s_script.active = 1;
    }
    pthread_mutex_unlock(&s_lock);
    return selected;
}

void bw_macos_input_bridge_cancel_synthetic_script(void)
{
    pthread_mutex_lock(&s_lock);
    cancel_script_locked();
    pthread_mutex_unlock(&s_lock);
}

static void apply_script_locked(XBOX_INPUT_STATE *state, unsigned port)
{
    const BwScriptStep *step;
    if (!s_script.active)
        return;
    step = &s_script.steps[s_script.step];
    if (port && s_script.phase != BW_SCRIPT_PRESS)
        return;
    if (!port && s_script.phase == BW_SCRIPT_WAIT) {
        if (s_script.remaining) {
            s_script.remaining--;
            return;
        }
        s_script.phase = BW_SCRIPT_PRESS;
        s_script.remaining = step->press_epochs;
        fprintf(stderr, "[BW-INPUT] synthetic script step %zu %s press\n",
                s_script.step + 1, script_button_name(step->button));
    } else if (!port && s_script.phase == BW_SCRIPT_RELEASE) {
        fprintf(stderr, "[BW-INPUT] synthetic script step %zu %s release\n",
                s_script.step + 1, script_button_name(step->button));
        s_script.step++;
        if (s_script.step == s_script.count) {
            cancel_script_locked();
            fprintf(stderr, "[BW-INPUT] synthetic script completed\n");
            return;
        }
        s_script.phase = BW_SCRIPT_WAIT;
        s_script.remaining = s_script.steps[s_script.step].wait_epochs;
        return;
    }

    if (step->pad_mask & (1u << port)) {
        switch (step->button) {
        case BW_SCRIPT_START:
            state->Gamepad.wButtons |= XBOX_GAMEPAD_START;
            break;
        case BW_SCRIPT_DPAD_DOWN:
            state->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
            break;
        case BW_SCRIPT_A:
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            break;
        }
    }
    if (!port && !--s_script.remaining)
        s_script.phase = BW_SCRIPT_RELEASE;
}
#endif

void bw_macos_input_bridge_configure(int enabled)
{
    atomic_store_explicit(&s_enabled, enabled != 0, memory_order_release);
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    pthread_mutex_lock(&s_lock);
    if (enabled) {
        configure_guided_locked();
    } else {
        cancel_script_locked();
        reset_guided_locked();
        s_requested_pad_count = 1;
    }
    pthread_mutex_unlock(&s_lock);
#endif
}

static int enabled(void)
{
    return atomic_load_explicit(&s_enabled, memory_order_acquire);
}

static int handle_port_locked(uint32_t handle)
{
    unsigned port;

    for (port = 0; port < s_pad_count; port++)
        if (s_handles[port] && s_handles[port] == handle)
            return (int)port;
    return -1;
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
    memset(s_handles, 0, sizeof s_handles);
    memset(s_packets, 0, sizeof s_packets);
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    s_pad_count = s_requested_pad_count;
#else
    s_pad_count = 1;
#endif
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
        mask = s_initialized && args[0] == BW_GAMEPAD_TYPE
             ? (1u << s_pad_count) - 1u : 0u;
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
    unsigned port;
    if (!enabled()) {
        sub_0016DC16_gen();
        return;
    }
    observed(3, "controller open");
    arguments(args, 4);
    port = args[1];
    pthread_mutex_lock(&s_lock);
    if (s_initialized && args[0] == BW_GAMEPAD_TYPE && port < s_pad_count &&
        args[2] == 0 && args[3] == 0 && !s_handles[port] &&
        s_generation < BW_HANDLE_LIMIT)
        handle = s_handles[port] = BW_HANDLE_PREFIX | ++s_generation;
    pthread_mutex_unlock(&s_lock);
    if (!handle)
        invalid("controller open");
    finish(4, handle);
}

void sub_0016DC8B(void)
{
    uint32_t args[1];
    int port;
    if (!enabled()) {
        sub_0016DC8B_gen();
        return;
    }
    observed(4, "controller close");
    arguments(args, 1);
    pthread_mutex_lock(&s_lock);
    port = handle_port_locked(args[0]);
    if (port >= 0) {
        s_handles[port] = 0;
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
        if (!port)
            cancel_script_locked();
#endif
    }
    pthread_mutex_unlock(&s_lock);
    if (port < 0)
        invalid("controller close");
    finish(1, 0);
}

void sub_0016DC97(void)
{
    uint32_t args[2], result = BW_ERROR_DEVICE_NOT_CONNECTED;
    int port;
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
    port = handle_port_locked(args[0]);
    if (port >= 0) {
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
    int port;
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
    memset(&state, 0, sizeof state);
    pthread_mutex_lock(&s_lock);
    port = handle_port_locked(args[0]);
    if (port >= 0) {
        if (!port) {
            xbox_InputKeyboardState(&state);
            bw_macos_mouse_apply(&state.Gamepad.sThumbLX, &state.Gamepad.sThumbLY,
                                 &state.Gamepad.sThumbRX, &state.Gamepad.sThumbRY,
                                 &state.dwPacketNumber);
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
            uint64_t input_now = monotonic_ms();
            record_guided_input_locked(&state, input_now);
            apply_script_locked(&state, 0);
            apply_guided_locked(&state, 0, input_now);
            synthetic_seen = atomic_load_explicit(
                &s_trace_synthetic, memory_order_acquire) &&
                (state.Gamepad.wButtons &
                 (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN)) ==
                    (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN) &&
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255;
#endif
        } else {
            state.dwPacketNumber = ++s_packets[port];
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
            apply_script_locked(&state, (unsigned)port);
            apply_guided_locked(&state, (unsigned)port, monotonic_ms());
#endif
        }
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
    int port;
    if (!enabled()) {
        sub_0016DEFA_gen();
        return;
    }
    observed(7, "controller feedback");
    arguments(args, 2);
    pthread_mutex_lock(&s_lock);
    port = handle_port_locked(args[0]);
    if (port >= 0)
        result = BW_ERROR_NOT_SUPPORTED;
    pthread_mutex_unlock(&s_lock);
    invalid(result == BW_ERROR_NOT_SUPPORTED ? "controller feedback" : "feedback handle");
    finish(2, result);
}
