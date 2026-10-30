#if defined(_WIN32)
#error "The SDL input wrapper is only for POSIX hosts"
#endif

#define xbox_InputSdlInit bw_upstream_InputSdlInit
#define xbox_InputSdlEvent bw_upstream_InputSdlEvent
#include <keyboard_sdl.c>
#undef xbox_InputSdlInit
#undef xbox_InputSdlEvent

#include "xinput_xbox.h"

extern void bw_InputRefreshControllers(void);

void xbox_InputSdlInit(void)
{
    bw_upstream_InputSdlInit();
    xbox_InputInit();
}

void xbox_InputSdlEvent(const SDL_Event *event)
{
    bw_upstream_InputSdlEvent(event);
    if (event->type == SDL_CONTROLLERDEVICEADDED ||
        event->type == SDL_CONTROLLERDEVICEREMOVED ||
        event->type == SDL_CONTROLLERDEVICEREMAPPED ||
        event->type == SDL_JOYDEVICEADDED ||
        event->type == SDL_JOYDEVICEREMOVED)
        bw_InputRefreshControllers();
}
