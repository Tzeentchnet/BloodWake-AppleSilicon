#define pthread_create bw_test_pthread_create
#include "../../src/toolkit_patches/video/fb_present_sdl.c"

int bw_test_presenter_running(void)
{
    return atomic_load_explicit(&s_running, memory_order_seq_cst);
}
