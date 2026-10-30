#if defined(_WIN32)
#error "The SDL controller hotplug wrapper is only for POSIX hosts"
#endif

#include <pthread.h>
#include <stdio.h>

/* Reuse the pinned implementation while serializing its controller state.
 * The SDL main thread refreshes devices through bw_InputRefreshControllers. */
#define xbox_InputInit bw_upstream_InputInit
#define xbox_InputGetState bw_upstream_InputGetState
#define xbox_InputSetState bw_upstream_InputSetState
#define xbox_InputIsConnected bw_upstream_InputIsConnected
#define xbox_InputGetCapabilities bw_upstream_InputGetCapabilities
#define xbox_InputSetMainPump bw_upstream_InputSetMainPump
#define xbox_InputAnyOpen bw_upstream_InputAnyOpen
#include <xinput_device.c>
#undef xbox_InputInit
#undef xbox_InputGetState
#undef xbox_InputSetState
#undef xbox_InputIsConnected
#undef xbox_InputGetCapabilities
#undef xbox_InputSetMainPump
#undef xbox_InputAnyOpen

static pthread_mutex_t s_input_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_JoystickID s_failed_open = -1;
static int s_initialized;

static void refresh_controllers(void)
{
    int count;

    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        return;

    SDL_LockJoysticks();
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
        }
    }

    count = SDL_NumJoysticks();
    if (count < 0) {
        fprintf(stderr, "  [INPUT] cannot enumerate controllers: %s\n", SDL_GetError());
        SDL_UnlockJoysticks();
        return;
    }
    for (int device = 0; device < count; device++) {
        SDL_JoystickID id;
        int slot, free_slot = -1, already_open = 0;

        if (!SDL_IsGameController(device))
            continue;
        id = SDL_JoystickGetDeviceInstanceID(device);
        if (id < 0) {
            fprintf(stderr, "  [INPUT] cannot identify controller: %s\n", SDL_GetError());
            continue;
        }
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
            if (!g_pads[slot]) {
                if (free_slot < 0)
                    free_slot = slot;
            } else if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[slot])) == id) {
                already_open = 1;
                break;
            }
        }
        if (already_open || free_slot < 0)
            continue;
        g_pads[free_slot] = SDL_GameControllerOpen(device);
        g_controller_connected[free_slot] = (g_pads[free_slot] != NULL);
        if (!g_pads[free_slot]) {
            if (s_failed_open != id) {
                fprintf(stderr, "  [INPUT] cannot open controller %d: %s\n",
                        (int)id, SDL_GetError());
                s_failed_open = id;
            }
        } else if (s_failed_open == id) {
            s_failed_open = -1;
        }
    }
    SDL_UnlockJoysticks();
}

void xbox_InputInit(void)
{
    pthread_mutex_lock(&s_input_lock);
    if (!s_initialized) {
        bw_upstream_InputInit();
        if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
            fprintf(stderr, "  [INPUT] SDL controller initialization failed: %s\n", SDL_GetError());
        else {
            refresh_controllers();
            s_initialized = 1;
        }
    }
    pthread_mutex_unlock(&s_input_lock);
}

void bw_InputRefreshControllers(void)
{
    pthread_mutex_lock(&s_input_lock);
    if (s_initialized)
        refresh_controllers();
    pthread_mutex_unlock(&s_input_lock);
}

DWORD xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *state)
{
    DWORD result;
    pthread_mutex_lock(&s_input_lock);
    result = bw_upstream_InputGetState(port, state);
    pthread_mutex_unlock(&s_input_lock);
    return result;
}

DWORD xbox_InputSetState(DWORD port, const XBOX_VIBRATION *vibration)
{
    DWORD result;
    pthread_mutex_lock(&s_input_lock);
    result = bw_upstream_InputSetState(port, vibration);
    pthread_mutex_unlock(&s_input_lock);
    return result;
}

BOOL xbox_InputIsConnected(DWORD port)
{
    BOOL result;
    pthread_mutex_lock(&s_input_lock);
    result = bw_upstream_InputIsConnected(port);
    pthread_mutex_unlock(&s_input_lock);
    return result;
}

DWORD xbox_InputGetCapabilities(DWORD port, DWORD flags, XBOX_INPUT_CAPABILITIES *caps)
{
    DWORD result;
    pthread_mutex_lock(&s_input_lock);
    result = bw_upstream_InputGetCapabilities(port, flags, caps);
    pthread_mutex_unlock(&s_input_lock);
    return result;
}

void xbox_InputSetMainPump(int on)
{
    pthread_mutex_lock(&s_input_lock);
    bw_upstream_InputSetMainPump(on);
    pthread_mutex_unlock(&s_input_lock);
}

int xbox_InputAnyOpen(void)
{
    int result;
    pthread_mutex_lock(&s_input_lock);
    result = bw_upstream_InputAnyOpen();
    pthread_mutex_unlock(&s_input_lock);
    return result;
}
