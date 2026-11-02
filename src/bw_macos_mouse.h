#ifndef BW_MACOS_MOUSE_H
#define BW_MACOS_MOUSE_H

#include <stddef.h>
#include <stdint.h>

typedef union SDL_Event SDL_Event;

typedef enum {
    BW_MOUSE_STICK_STEERING = 0,
    BW_MOUSE_STICK_AIM = 1,
} BwMouseStick;

typedef enum {
    BW_MOUSE_CAPTURE_HOLD = 0,
    BW_MOUSE_CAPTURE_TOGGLE = 1,
} BwMouseCapture;

typedef struct {
    int enabled;
    float sensitivity;
    int invert_y;
    int accelerated;
    BwMouseStick stick;
    BwMouseCapture capture;
} BwMouseConfig;

int bw_macos_mouse_init_from_env(char *err, size_t errlen);
void bw_macos_mouse_get_config(BwMouseConfig *config);
int bw_macos_mouse_set_config(const BwMouseConfig *config, char *err, size_t errlen);

/* Main SDL event thread only. */
void bw_macos_mouse_sdl_event(const SDL_Event *event);
void bw_macos_mouse_release_capture(void);
int bw_macos_mouse_captured(void);

/* Guest input thread. Consumes accumulated relative motion and merges it with
 * the larger-magnitude axis already supplied by keyboard/controller input. */
int bw_macos_mouse_apply(int16_t *left_x, int16_t *left_y,
                         int16_t *right_x, int16_t *right_y,
                         uint32_t *packet_number);

#endif
