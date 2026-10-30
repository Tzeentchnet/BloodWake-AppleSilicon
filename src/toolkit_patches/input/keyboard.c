#if defined(_WIN32)
#error "The SDL keyboard synchronization wrapper is only for POSIX hosts"
#endif

#include <pthread.h>

/* The window thread writes the pinned keyboard table while guest threads
 * poll it. Serialize both sides, including the packet counter. */
#define xbox_InputKeySet bw_upstream_InputKeySet
#define xbox_InputKeysClear bw_upstream_InputKeysClear
#define xbox_FramebufferKeyDown bw_upstream_FramebufferKeyDown
#define xbox_InputKeyboardState bw_upstream_InputKeyboardState
#define xbox_InputKeyboardPoll bw_upstream_InputKeyboardPoll
#include <keyboard.c>
#undef xbox_InputKeySet
#undef xbox_InputKeysClear
#undef xbox_FramebufferKeyDown
#undef xbox_InputKeyboardState
#undef xbox_InputKeyboardPoll

static pthread_mutex_t s_keyboard_lock = PTHREAD_MUTEX_INITIALIZER;

void xbox_InputKeySet(int vk, int down)
{
    pthread_mutex_lock(&s_keyboard_lock);
    bw_upstream_InputKeySet(vk, down);
    pthread_mutex_unlock(&s_keyboard_lock);
}

void xbox_InputKeysClear(void)
{
    pthread_mutex_lock(&s_keyboard_lock);
    bw_upstream_InputKeysClear();
    pthread_mutex_unlock(&s_keyboard_lock);
}

int xbox_FramebufferKeyDown(int vk)
{
    int result;
    pthread_mutex_lock(&s_keyboard_lock);
    result = bw_upstream_FramebufferKeyDown(vk);
    pthread_mutex_unlock(&s_keyboard_lock);
    return result;
}

void xbox_InputKeyboardState(XBOX_INPUT_STATE *state)
{
    pthread_mutex_lock(&s_keyboard_lock);
    bw_upstream_InputKeyboardState(state);
    pthread_mutex_unlock(&s_keyboard_lock);
}

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *state)
{
    int result;
    pthread_mutex_lock(&s_keyboard_lock);
    result = bw_upstream_InputKeyboardPoll(state);
    pthread_mutex_unlock(&s_keyboard_lock);
    return result;
}
