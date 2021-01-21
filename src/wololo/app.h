#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct Wo_App Wo_App;

typedef bool(*Wo_InitCallbackPtr)(Wo_App* app, uint32_t window_width, uint32_t window_height, char const* window_caption, double target_frame_time_sec);
typedef void(*Wo_UpdateCallbackPtr)(Wo_App* app, double elapsed_time_in_sec);
typedef void(*Wo_QuitCallbackPtr)(Wo_App* app);

Wo_App* wo_app_new(
    double target_updates_per_sec,
    uint32_t window_width, uint32_t window_height,
    char const* window_caption,
    Wo_InitCallbackPtr init_cb,
    Wo_UpdateCallbackPtr update_cb,
    Wo_QuitCallbackPtr quit_cb
);
bool wo_app_run(Wo_App* app_ref);
