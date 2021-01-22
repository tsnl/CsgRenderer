#include "app.h"

#include "platform.h"
#include "renderer/renderer.h"

#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>

//
// App implementation:
//

Wo_App* app_new(
    double updates_per_sec,
    uint32_t window_width, uint32_t window_height,
    char const* window_caption,
    Wo_InitCallbackPtr extension_init_cb,
    Wo_UpdateCallbackPtr extension_update_cb,
    Wo_DeInitCallbackPtr extension_de_init_cb
);
bool app_run(Wo_App* app);

struct Wo_App {
    Wo_InitCallbackPtr extension_init_cb;
    Wo_UpdateCallbackPtr extension_update_cb;
    Wo_DeInitCallbackPtr extension_de_init_cb;

    GLFWwindow* glfw_window;
    Wo_Renderer* renderer;

    double updates_per_sec;
    double update_time_sec;
    
    uint32_t window_width;
    uint32_t window_height;
    char const* window_caption;
};

bool the_app_in_use = false;
Wo_App the_app;

Wo_App* app_new(
    double updates_per_sec,
    uint32_t window_width, uint32_t window_height,
    char const* window_caption,
    Wo_InitCallbackPtr extension_init_cb,
    Wo_UpdateCallbackPtr extension_update_cb,
    Wo_DeInitCallbackPtr extension_de_init_cb
) {
    assert(!the_app_in_use);
    the_app_in_use = true;
    Wo_App* app = &the_app; {
        app->extension_init_cb = extension_init_cb;
        app->extension_update_cb = extension_update_cb;
        app->extension_de_init_cb = extension_de_init_cb;
        
        app->glfw_window = NULL;   
        app->renderer = NULL;

        app->window_width = window_width;
        app->window_height = window_height;
        app->window_caption = strdup(window_caption);

        app->updates_per_sec = updates_per_sec;
        app->update_time_sec = 1.0 / app->updates_per_sec;
    }
    return app;
}

bool app_run(Wo_App* app) {
    // Initialize GLFW
    int glfw_init_status = glfwInit();
    if (!glfw_init_status) {
        printf("- Could not initialize GLFW.\n");
        return false;
    }

    // Create a windowed mode window and its OpenGL context
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    app->glfw_window = glfwCreateWindow(
        app->window_width, app->window_height, 
        app->window_caption, 
        NULL, NULL
    );
    if (app->glfw_window == NULL) {
        printf("- Could not create a GLFW window.\n");
        glfwTerminate();
        return false;
    }

    // Make the window's context current
    glfwMakeContextCurrent(app->glfw_window);

    // Environment set up!

    // Extension init:
    if (app->extension_init_cb != NULL) {
        bool extension_init_ok = app->extension_init_cb(
            app,
            app->window_width, 
            app->window_height, 
            app->window_caption, 
            app->update_time_sec
        );
        if (extension_init_ok) {
            printf(
                "Initializing '%s' {w=%u, h=%u} @ %lf updates per second\n", 
                app->window_caption, app->window_width, app->window_height, app->updates_per_sec
            );
        } else {
            printf("- Extension init failed.\n");
            return false;
        }
    }

    // Loop until the user closes the window
    double time_at_last_frame_sec = glfwGetTime();
    double total_running_behind_by_sec = 0.0;
    while (!glfwWindowShouldClose(app->glfw_window))
    {
        // Updating:
        {
            // calculating elapsed time difference:
            double time_at_this_frame_sec = glfwGetTime();
            double time_difference_sec = time_at_this_frame_sec - time_at_last_frame_sec;
            assert(time_difference_sec >= 0 && "glfwGetTime not monotonic-- who called glfwSetTime?");

            // adding the difference to the 'running behind' count...
            total_running_behind_by_sec += time_difference_sec;

            // and then running updates at the desired resolution until we aren't running behind
            // by a frame anymore:
            while (app->extension_update_cb != NULL && total_running_behind_by_sec >= app->update_time_sec) {
                total_running_behind_by_sec -= app->update_time_sec;
                app->extension_update_cb(app, app->update_time_sec);
            }
        }

        // TODO: Render here (using renderer)
        // cf https://vulkan-tutorial.com/Drawing_a_triangle/Setup/Base_code
        
        // Swap front and back buffers
        glfwSwapBuffers(app->glfw_window);

        // Poll for and process events
        glfwPollEvents();
    }

    // Extension quit:
    if (app->extension_de_init_cb != NULL) {
        app->extension_de_init_cb(app);
    }

    glfwTerminate();
    return true;
}

void app_swap_scene(Wo_App* app, Wo_Renderer* renderer) {
    app->renderer = renderer;
}

//
// Interface:
//

Wo_App* wo_app_new(
    double max_updates_per_sec,
    uint32_t window_width, uint32_t window_height,
    char const* window_caption,
    Wo_InitCallbackPtr opt_init_cb,
    Wo_UpdateCallbackPtr opt_update_cb,
    Wo_DeInitCallbackPtr opt_de_init_cb
) {
    return app_new(
        max_updates_per_sec,
        window_width, window_height, window_caption,
        opt_init_cb, opt_update_cb, opt_de_init_cb
    );
}

bool wo_app_run(Wo_App* app) {
    return app_run(app);
}

void wo_app_swap_scene(Wo_App* app_ref, Wo_Renderer* new_scene_renderer) {
    return app_swap_scene(app_ref, new_scene_renderer);
}
