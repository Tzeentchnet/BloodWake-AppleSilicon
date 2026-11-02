#include "bw_macos_mouse.h"

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static BwMouseConfig s_config = {
    .sensitivity = 1.0f,
    .stick = BW_MOUSE_STICK_STEERING,
    .capture = BW_MOUSE_CAPTURE_HOLD,
};
static int64_t s_dx, s_dy;
static uint32_t s_generation;
static int s_focused = 1;
static int s_captured;

static void error_text(char *err, size_t errlen, const char *text)
{
    if (!err || !errlen)
        return;
    snprintf(err, errlen, "%s", text);
    err[errlen - 1] = 0;
}

static int valid_config(const BwMouseConfig *config, char *err, size_t errlen)
{
    if (!config || config->sensitivity != config->sensitivity ||
        config->sensitivity < 0.25f || config->sensitivity > 4.0f) {
        error_text(err, errlen, "mouse sensitivity must be from 0.25 to 4");
        return 0;
    }
    if ((config->enabled != 0 && config->enabled != 1) ||
        (config->invert_y != 0 && config->invert_y != 1) ||
        (config->accelerated != 0 && config->accelerated != 1) ||
        (config->stick != BW_MOUSE_STICK_STEERING &&
         config->stick != BW_MOUSE_STICK_AIM) ||
        (config->capture != BW_MOUSE_CAPTURE_HOLD &&
         config->capture != BW_MOUSE_CAPTURE_TOGGLE)) {
        error_text(err, errlen, "mouse configuration has an invalid choice");
        return 0;
    }
    return 1;
}

static int parse_bool(const char *name, int *value, char *err, size_t errlen)
{
    const char *text = getenv(name);
    if (!text || !*text)
        return 0;
    if (!strcmp(text, "0") || !strcmp(text, "1")) {
        *value = text[0] == '1';
        return 0;
    }
    if (err && errlen) {
        snprintf(err, errlen, "%s must be 0 or 1", name);
        err[errlen - 1] = 0;
    }
    return -1;
}

int bw_macos_mouse_init_from_env(char *err, size_t errlen)
{
    BwMouseConfig config = {
        .sensitivity = 1.0f,
        .stick = BW_MOUSE_STICK_STEERING,
        .capture = BW_MOUSE_CAPTURE_HOLD,
    };
    const char *text;
    char *end;
    if (err && errlen)
        err[0] = 0;
    if (parse_bool("BW_MOUSE", &config.enabled, err, errlen) != 0 ||
        parse_bool("BW_MOUSE_INVERT_Y", &config.invert_y, err, errlen) != 0)
        return -1;
    text = getenv("BW_MOUSE_SENSITIVITY");
    if (text && *text) {
        config.sensitivity = strtof(text, &end);
        if (!end || *end || config.sensitivity < 0.25f || config.sensitivity > 4.0f) {
            error_text(err, errlen, "BW_MOUSE_SENSITIVITY must be from 0.25 to 4");
            return -1;
        }
    }
    text = getenv("BW_MOUSE_RESPONSE");
    if (text && *text) {
        if (!strcasecmp(text, "linear"))
            config.accelerated = 0;
        else if (!strcasecmp(text, "accelerated"))
            config.accelerated = 1;
        else {
            error_text(err, errlen, "BW_MOUSE_RESPONSE must be linear or accelerated");
            return -1;
        }
    }
    text = getenv("BW_MOUSE_STICK");
    if (text && *text) {
        if (!strcasecmp(text, "steering"))
            config.stick = BW_MOUSE_STICK_STEERING;
        else if (!strcasecmp(text, "aim"))
            config.stick = BW_MOUSE_STICK_AIM;
        else {
            error_text(err, errlen, "BW_MOUSE_STICK must be steering or aim");
            return -1;
        }
    }
    text = getenv("BW_MOUSE_CAPTURE");
    if (text && *text) {
        if (!strcasecmp(text, "hold"))
            config.capture = BW_MOUSE_CAPTURE_HOLD;
        else if (!strcasecmp(text, "toggle"))
            config.capture = BW_MOUSE_CAPTURE_TOGGLE;
        else {
            error_text(err, errlen, "BW_MOUSE_CAPTURE must be hold or toggle");
            return -1;
        }
    }
    if (!valid_config(&config, err, errlen))
        return -1;
    pthread_mutex_lock(&s_lock);
    s_config = config;
    s_dx = s_dy = 0;
    s_generation = 0;
    s_focused = 1;
    s_captured = 0;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void bw_macos_mouse_get_config(BwMouseConfig *config)
{
    if (!config)
        return;
    pthread_mutex_lock(&s_lock);
    *config = s_config;
    pthread_mutex_unlock(&s_lock);
}

int bw_macos_mouse_set_config(const BwMouseConfig *config, char *err, size_t errlen)
{
    int release;
    if (err && errlen)
        err[0] = 0;
    if (!valid_config(config, err, errlen))
        return -1;
    pthread_mutex_lock(&s_lock);
    release = s_captured && !config->enabled;
    s_config = *config;
    if (!config->enabled)
        s_dx = s_dy = 0;
    pthread_mutex_unlock(&s_lock);
    if (release)
        bw_macos_mouse_release_capture();
    return 0;
}

static void set_capture(int captured)
{
    int enabled, changed;
    pthread_mutex_lock(&s_lock);
    enabled = s_config.enabled && s_focused;
    captured = captured && enabled;
    changed = captured != s_captured;
    pthread_mutex_unlock(&s_lock);
    if (!changed)
        return;
    if (SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE) != 0) {
        fprintf(stderr, "[BW-MOUSE] could not %s relative mouse mode: %s\n",
                captured ? "enter" : "leave", SDL_GetError());
        return;
    }
    SDL_ShowCursor(captured ? SDL_DISABLE : SDL_ENABLE);
    pthread_mutex_lock(&s_lock);
    s_captured = captured;
    s_dx = s_dy = 0;
    s_generation++;
    pthread_mutex_unlock(&s_lock);
}

void bw_macos_mouse_release_capture(void)
{
    set_capture(0);
}

int bw_macos_mouse_captured(void)
{
    int captured;
    pthread_mutex_lock(&s_lock);
    captured = s_captured;
    pthread_mutex_unlock(&s_lock);
    return captured;
}

void bw_macos_mouse_sdl_event(const SDL_Event *event)
{
    int toggle = 0, hold = 0, release = 0;
    if (!event)
        return;
    if (event->type == SDL_MOUSEMOTION) {
        pthread_mutex_lock(&s_lock);
        if (s_config.enabled && s_focused && s_captured) {
            s_dx += event->motion.xrel;
            s_dy += event->motion.yrel;
            if (s_dx > 1000000) s_dx = 1000000;
            if (s_dx < -1000000) s_dx = -1000000;
            if (s_dy > 1000000) s_dy = 1000000;
            if (s_dy < -1000000) s_dy = -1000000;
            s_generation++;
        }
        pthread_mutex_unlock(&s_lock);
        return;
    }
    if ((event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) &&
        event->button.button == SDL_BUTTON_RIGHT) {
        pthread_mutex_lock(&s_lock);
        if (s_config.enabled) {
            if (s_config.capture == BW_MOUSE_CAPTURE_TOGGLE &&
                event->type == SDL_MOUSEBUTTONDOWN)
                toggle = s_captured ? -1 : 1;
            else if (s_config.capture == BW_MOUSE_CAPTURE_HOLD)
                hold = event->type == SDL_MOUSEBUTTONDOWN ? 1 : -1;
        }
        pthread_mutex_unlock(&s_lock);
        if (toggle > 0)
            set_capture(1);
        else if (toggle < 0)
            set_capture(0);
        else if (hold > 0)
            set_capture(1);
        else if (hold < 0)
            set_capture(0);
        return;
    }
    if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE)
        release = 1;
    if (event->type == SDL_WINDOWEVENT) {
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            pthread_mutex_lock(&s_lock);
            s_focused = 1;
            pthread_mutex_unlock(&s_lock);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                   event->window.event == SDL_WINDOWEVENT_HIDDEN ||
                   event->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            pthread_mutex_lock(&s_lock);
            s_focused = 0;
            s_dx = s_dy = 0;
            pthread_mutex_unlock(&s_lock);
            release = 1;
        }
    }
    if (release)
        set_capture(0);
}

static int16_t scaled_axis(int64_t delta, float sensitivity, int accelerated)
{
    float value = (float)delta * sensitivity * 2048.0f;
    if (accelerated) {
        float magnitude = delta < 0 ? (float)-delta : (float)delta;
        if (magnitude > 32.0f)
            magnitude = 32.0f;
        value *= 1.0f + magnitude / 32.0f;
    }
    if (value > 32767.0f)
        return 32767;
    if (value < -32767.0f)
        return -32767;
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static void merge_axis(int16_t *axis, int16_t mouse)
{
    int current_abs = *axis < 0 ? -(int)*axis : *axis;
    int mouse_abs = mouse < 0 ? -(int)mouse : mouse;
    if (mouse_abs > current_abs)
        *axis = mouse;
}

int bw_macos_mouse_apply(int16_t *left_x, int16_t *left_y,
                         int16_t *right_x, int16_t *right_y,
                         uint32_t *packet_number)
{
    BwMouseConfig config;
    int64_t dx, dy;
    uint32_t generation;
    int16_t x, y;
    if (!left_x || !left_y || !right_x || !right_y || !packet_number)
        return 0;
    pthread_mutex_lock(&s_lock);
    config = s_config;
    dx = s_dx;
    dy = s_dy;
    generation = s_generation;
    s_dx = s_dy = 0;
    pthread_mutex_unlock(&s_lock);
    if (!config.enabled)
        return 0;
    *packet_number += generation;
    if (!dx && !dy)
        return 0;
    x = scaled_axis(dx, config.sensitivity, config.accelerated);
    y = scaled_axis(config.invert_y ? dy : -dy,
                    config.sensitivity, config.accelerated);
    if (config.stick == BW_MOUSE_STICK_AIM) {
        merge_axis(right_x, x);
        merge_axis(right_y, y);
    } else {
        merge_axis(left_x, x);
        merge_axis(left_y, y);
    }
    return 1;
}
