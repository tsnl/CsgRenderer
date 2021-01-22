#pragma once

#include <stdint.h>
#include <stdbool.h>

//
// forward
//

typedef struct Wo_Renderer Wo_Renderer;

//
// interface
//

typedef struct Wo_App Wo_App;

typedef bool(*Wo_InitCallbackPtr)(Wo_App* app, uint32_t window_width, uint32_t window_height, char const* window_caption, double target_frame_time_sec);
typedef void(*Wo_UpdateCallbackPtr)(Wo_App* app, double elapsed_time_in_sec);
typedef void(*Wo_DeInitCallbackPtr)(Wo_App* app);

Wo_App* wo_app_new(
    double target_updates_per_sec,
    uint32_t window_width, uint32_t window_height,
    char const* window_caption,
    Wo_InitCallbackPtr opt_init_cb,
    Wo_UpdateCallbackPtr opt_update_cb,
    Wo_DeInitCallbackPtr opt_de_init_cb
);
bool wo_app_run(Wo_App* app_ref);

void wo_app_swap_scene(Wo_App* app_ref, Wo_Renderer* new_scene_renderer);
