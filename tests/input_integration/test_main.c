#include "keyboard_sdl.h"
#include "usb_gamepad.h"
#include "xinput_xbox.h"

#include <SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

extern void xbox_InputSetMainPump(int on);

static void check(int condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s (%s)\n", description, SDL_GetError());
        failures++;
    }
}

static void key(Uint32 type, SDL_Keycode code)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.key.keysym.sym = code;
    xbox_InputSdlEvent(&event);
}

static void focus_lost(void)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    xbox_InputSdlEvent(&event);
}

static void pump_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
        xbox_InputSdlEvent(&event);
}

static void report(uint8_t *out)
{
    memset(out, 0xff, 20);
    check(usb_gamepad_report(0, out, 20) == 20, "20-byte USB report");
    check(out[0] == 0 && out[1] == 20, "USB report header");
}

static void keyboard_test(void)
{
    uint8_t out[20];

    xbox_InputSdlInit();
    key(SDL_KEYDOWN, SDLK_RETURN);
    key(SDL_KEYDOWN, SDLK_DOWN);
    key(SDL_KEYDOWN, SDLK_z);
    report(out);
    check((out[2] & (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN)) ==
              (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN),
          "Return and Down reach the USB report");
    check(out[4 + XBOX_BUTTON_A] == 255, "Z reaches the A pressure byte");

    key(SDL_KEYUP, SDLK_RETURN);
    key(SDL_KEYUP, SDLK_DOWN);
    key(SDL_KEYUP, SDLK_z);
    report(out);
    check(out[2] == 0 && out[4 + XBOX_BUTTON_A] == 0,
          "key releases reach the USB report");

    key(SDL_KEYDOWN, SDLK_RETURN);
    focus_lost();
    report(out);
    check((out[2] & XBOX_GAMEPAD_START) == 0, "focus loss releases keys");
}

static void keyboard_disabled_test(void)
{
    uint8_t out[20];
    xbox_InputSdlInit();
    key(SDL_KEYDOWN, SDLK_RETURN);
    key(SDL_KEYDOWN, SDLK_z);
    report(out);
    check((out[2] & XBOX_GAMEPAD_START) == 0 && out[4 + XBOX_BUTTON_A] == 0,
          "explicitly disabled keyboard does not drive the USB report");
    key(SDL_KEYUP, SDLK_RETURN);
    key(SDL_KEYUP, SDLK_z);
}

static int controller_test(int keyboard_enabled)
{
    uint8_t out[20];
    SDL_Joystick *joystick;
    int device;

    if (SDL_NumJoysticks() != 0) {
        fprintf(stderr, "SKIP: disconnect physical controllers for this isolated virtual-pad test\n");
        return 77;
    }
    device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                       SDL_CONTROLLER_AXIS_MAX,
                                       SDL_CONTROLLER_BUTTON_MAX, 0);
    if (device < 0) {
        fprintf(stderr, "Cannot attach virtual controller: %s\n", SDL_GetError());
        return 1;
    }
    check(SDL_IsGameController(device), "virtual joystick is a game controller");
    joystick = SDL_JoystickOpen(device);
    if (!joystick) {
        fprintf(stderr, "Cannot open virtual controller: %s\n", SDL_GetError());
        SDL_JoystickDetachVirtual(device);
        return 1;
    }

    xbox_InputSdlInit();
    pump_events();
    check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_START, 1) == 0,
          "press virtual Start");
    check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 1) == 0,
          "press virtual Down");
    key(SDL_KEYDOWN, SDLK_z);
    SDL_JoystickUpdate();
    report(out);
    check((out[2] & (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN)) ==
              (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_DOWN),
          "controller Start and Down reach the USB report");
    check(out[4 + XBOX_BUTTON_A] == (keyboard_enabled ? 255 : 0),
          keyboard_enabled ? "keyboard A is merged with the connected controller" :
                             "disabled keyboard does not affect the connected controller");

    key(SDL_KEYUP, SDLK_z);
    check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_START, 0) == 0,
          "release virtual Start");
    check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0) == 0,
          "release virtual Down");
    SDL_JoystickUpdate();
    report(out);
    check(out[2] == 0 && out[4 + XBOX_BUTTON_A] == 0,
          "controller and keyboard releases reach the USB report");

    check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1) == 0,
          "press virtual A");
    check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_B, 1) == 0,
          "press virtual B");
    SDL_JoystickUpdate();
    report(out);
    check(out[4 + XBOX_BUTTON_A] == 255 && out[4 + XBOX_BUTTON_B] == 255,
          "controller A and B reach the USB report");

    check(SDL_JoystickDetachVirtual(device) == 0, "detach virtual controller");
    pump_events();
    key(SDL_KEYDOWN, SDLK_RETURN);
    report(out);
    check(((out[2] & XBOX_GAMEPAD_START) != 0) == keyboard_enabled,
          keyboard_enabled ? "keyboard remains available after controller disconnection" :
                             "keyboard remains disabled after controller disconnection");
    key(SDL_KEYUP, SDLK_RETURN);
    SDL_JoystickClose(joystick);
    return failures ? 1 : 0;
}

static int hotplug_test(void)
{
    uint8_t out[20];

    if (SDL_NumJoysticks() != 0) {
        fprintf(stderr, "SKIP: disconnect physical controllers for this isolated virtual-pad test\n");
        return 77;
    }
    xbox_InputSdlInit();
    for (int attempt = 0; attempt < 2; attempt++) {
        int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                               SDL_CONTROLLER_AXIS_MAX,
                                               SDL_CONTROLLER_BUTTON_MAX, 0);
        if (device < 0) {
            fprintf(stderr, "Cannot attach virtual controller: %s\n", SDL_GetError());
            return 1;
        }
        SDL_Joystick *joystick = SDL_JoystickOpen(device);
        if (!joystick) {
            fprintf(stderr, "Cannot open virtual controller: %s\n", SDL_GetError());
            SDL_JoystickDetachVirtual(device);
            return 1;
        }
        check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_START, 1) == 0,
              "press hotplugged Start");
        SDL_JoystickUpdate();
        pump_events();
        report(out);
        check((out[2] & XBOX_GAMEPAD_START) != 0,
              attempt == 0 ? "controller connected after input initialization works" :
                             "controller reconnected after disconnection works");
        check(SDL_JoystickDetachVirtual(device) == 0, "detach hotplugged controller");
        pump_events();
        SDL_JoystickClose(joystick);
        report(out);
        check((out[2] & XBOX_GAMEPAD_START) == 0,
              "disconnected controller no longer reports held buttons");
    }
    return failures ? 1 : 0;
}

static int slots_test(void)
{
    uint8_t first[20], second[20];
    SDL_Joystick *pads[3];
    int devices[3];

    if (SDL_NumJoysticks() != 0) {
        fprintf(stderr, "SKIP: disconnect physical controllers for this isolated virtual-pad test\n");
        return 77;
    }
    xbox_InputSdlInit();
    for (int i = 0; i < 2; i++) {
        devices[i] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                               SDL_CONTROLLER_AXIS_MAX,
                                               SDL_CONTROLLER_BUTTON_MAX, 0);
        if (devices[i] < 0 || !(pads[i] = SDL_JoystickOpen(devices[i]))) {
            fprintf(stderr, "Cannot attach or open virtual controller: %s\n", SDL_GetError());
            return 1;
        }
    }
    check(SDL_JoystickSetVirtualButton(pads[0], SDL_CONTROLLER_BUTTON_START, 1) == 0,
          "press first controller Start");
    check(SDL_JoystickSetVirtualButton(pads[1], SDL_CONTROLLER_BUTTON_B, 1) == 0,
          "press second controller B");
    SDL_JoystickUpdate();
    pump_events();
    report(first);
    check(usb_gamepad_report(1, second, sizeof(second)) == 20, "second USB report");
    check((first[2] & XBOX_GAMEPAD_START) != 0 && second[4 + XBOX_BUTTON_B] == 255,
          "two controllers use distinct ports");

    check(SDL_JoystickDetachVirtual(devices[0]) == 0, "detach first controller");
    pump_events();
    SDL_JoystickClose(pads[0]);
    check(usb_gamepad_report(1, second, sizeof(second)) == 20 &&
              second[4 + XBOX_BUTTON_B] == 255,
          "remaining controller keeps its port");

    devices[2] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                           SDL_CONTROLLER_AXIS_MAX,
                                           SDL_CONTROLLER_BUTTON_MAX, 0);
    if (devices[2] < 0 || !(pads[2] = SDL_JoystickOpen(devices[2]))) {
        fprintf(stderr, "Cannot attach or open replacement controller: %s\n", SDL_GetError());
        return 1;
    }
    check(SDL_JoystickSetVirtualButton(pads[2], SDL_CONTROLLER_BUTTON_A, 1) == 0,
          "press replacement controller A");
    SDL_JoystickUpdate();
    pump_events();
    report(first);
    check(usb_gamepad_report(1, second, sizeof(second)) == 20, "second USB report after reconnect");
    check(first[4 + XBOX_BUTTON_A] == 255 && second[4 + XBOX_BUTTON_B] == 255,
          "replacement uses the free port without moving the remaining controller");

    check(SDL_JoystickDetachVirtual(devices[2]) == 0, "detach replacement controller");
    SDL_JoystickClose(pads[2]);
    check(SDL_JoystickDetachVirtual(0) == 0, "detach remaining controller");
    SDL_JoystickClose(pads[1]);
    return failures ? 1 : 0;
}

typedef struct PollState {
    atomic_int stop;
    atomic_int polls;
    atomic_int errors;
} PollState;

static void *poll_reports(void *arg)
{
    PollState *state = arg;
    uint8_t out[20];
    while (!atomic_load(&state->stop)) {
        if (usb_gamepad_report(0, out, sizeof(out)) != 20)
            atomic_fetch_add(&state->errors, 1);
        atomic_fetch_add(&state->polls, 1);
    }
    return NULL;
}

static int concurrent_test(void)
{
    PollState state = {0};
    pthread_t thread;

    if (SDL_NumJoysticks() != 0) {
        fprintf(stderr, "SKIP: disconnect physical controllers for this isolated virtual-pad test\n");
        return 77;
    }
    xbox_InputSdlInit();
    xbox_InputSetMainPump(1);
    if (pthread_create(&thread, NULL, poll_reports, &state) != 0) {
        fprintf(stderr, "Cannot start input polling thread\n");
        return 1;
    }
    for (int i = 0; i < 30; i++) {
        int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                               SDL_CONTROLLER_AXIS_MAX,
                                               SDL_CONTROLLER_BUTTON_MAX, 0);
        SDL_Joystick *joystick = device >= 0 ? SDL_JoystickOpen(device) : NULL;
        if (!joystick) {
            fprintf(stderr, "Cannot attach or open virtual controller: %s\n", SDL_GetError());
            failures++;
            break;
        }
        check(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1) == 0,
              "press virtual A during polling");
        key(SDL_KEYDOWN, SDLK_RETURN);
        pump_events();
        SDL_Delay(1);
        key(SDL_KEYUP, SDLK_RETURN);
        check(SDL_JoystickDetachVirtual(device) == 0, "detach controller during polling");
        SDL_JoystickClose(joystick);
        pump_events();
    }
    atomic_store(&state.stop, 1);
    pthread_join(thread, NULL);
    xbox_InputSetMainPump(0);
    check(atomic_load(&state.polls) > 0, "worker polled USB reports");
    check(atomic_load(&state.errors) == 0, "concurrent USB reports retain their size");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    int result;
    if (argc != 2 || (strcmp(argv[1], "keyboard") && strcmp(argv[1], "keyboard-disabled") &&
                      strcmp(argv[1], "controller") && strcmp(argv[1], "controller-only") &&
                      strcmp(argv[1], "hotplug") && strcmp(argv[1], "slots") &&
                      strcmp(argv[1], "concurrent"))) {
        fprintf(stderr, "Usage: %s keyboard|keyboard-disabled|controller|controller-only|"
                        "hotplug|slots|concurrent\n", argv[0]);
        return 1;
    }
    unsetenv("RECOMP_PAD_PRESS");
    unsetenv("RECOMP_PAD_SCRIPT");
    unsetenv("RECOMP_PAD_LIVE");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    xbox_InputSetMainPump(1);
    if ((!strcmp(argv[1], "keyboard") || !strcmp(argv[1], "keyboard-disabled")) &&
        SDL_NumJoysticks() != 0) {
        fprintf(stderr, "SKIP: disconnect physical controllers for this isolated keyboard test\n");
        result = 77;
    } else if (!strcmp(argv[1], "keyboard")) {
        keyboard_test();
        result = failures ? 1 : 0;
    } else if (!strcmp(argv[1], "keyboard-disabled")) {
        keyboard_disabled_test();
        result = failures ? 1 : 0;
    } else if (!strcmp(argv[1], "controller")) {
        result = controller_test(1);
    } else if (!strcmp(argv[1], "controller-only")) {
        result = controller_test(0);
    } else if (!strcmp(argv[1], "hotplug")) {
        result = hotplug_test();
    } else if (!strcmp(argv[1], "slots")) {
        result = slots_test();
    } else {
        result = concurrent_test();
    }
    SDL_Quit();
    return result;
}
