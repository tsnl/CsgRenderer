#include "app.h"

#include "platform.h"

#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>

//
// App implementation:
//

struct Wo_App {
    Wo_InitCallbackPtr extension_init_cb;
    Wo_UpdateCallbackPtr extension_update_cb;
    Wo_QuitCallbackPtr extension_quit_cb;

    GLFWwindow* glfw_window;

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
    Wo_QuitCallbackPtr extension_quit_cb
) {
    assert(!the_app_in_use);
    the_app_in_use = true;
    Wo_App* app_ref = &the_app; {
        app_ref->extension_init_cb = extension_init_cb;
        app_ref->extension_update_cb = extension_update_cb;
        app_ref->extension_quit_cb = extension_quit_cb;
        
        app_ref->glfw_window = NULL;   

        app_ref->window_width = window_width;
        app_ref->window_height = window_height;
        app_ref->window_caption = strdup(window_caption);

        app_ref->updates_per_sec = updates_per_sec;
        app_ref->update_time_sec = 1.0 / app_ref->updates_per_sec;
    }
    return app_ref;
}

bool app_run(Wo_App* app_ref) {
    // Initialize GLFW
    int glfw_init_status = glfwInit();
    if (!glfw_init_status) {
        printf("- Could not initialize GLFW.\n");
        return false;
    }

    // Create a windowed mode window and its OpenGL context
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    app_ref->glfw_window = glfwCreateWindow(
        app_ref->window_width, app_ref->window_height, 
        app_ref->window_caption, 
        NULL, NULL
    );
    if (app_ref->glfw_window == NULL) {
        printf("- Could not create a GLFW window.\n");
        glfwTerminate();
        return false;
    }

    // Extension init:
    bool extension_init_ok = app_ref->extension_init_cb(
        app_ref,
        app_ref->window_width, 
        app_ref->window_height, 
        app_ref->window_caption, 
        app_ref->update_time_sec
    );
    if (!extension_init_ok) {
        printf("- Extension init failed.\n");
        return false;
    }

    // Make the window's context current
    glfwMakeContextCurrent(app_ref->glfw_window);

    // Loop until the user closes the window
    double time_at_last_frame_sec = glfwGetTime();
    double total_running_behind_by_sec = 0.0;
    while (!glfwWindowShouldClose(app_ref->glfw_window))
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
            while (total_running_behind_by_sec >= app_ref->update_time_sec) {
                total_running_behind_by_sec -= app_ref->update_time_sec;
                app_ref->extension_update_cb(app_ref, app_ref->update_time_sec);
            }
        }

        // TODO: Render here
        // cf https://vulkan-tutorial.com/Drawing_a_triangle/Setup/Base_code
        
        // Swap front and back buffers
        glfwSwapBuffers(app_ref->glfw_window);

        // Poll for and process events
        glfwPollEvents();
    }

    // Extension quit:
    app_ref->extension_quit_cb(app_ref);

    glfwTerminate();
    return true;
}

//
// Interface:
//

Wo_App* wo_app_new(
    double max_updates_per_sec,
    uint32_t window_width, uint32_t window_height,
    char const* window_caption,
    Wo_InitCallbackPtr init_cb,
    Wo_UpdateCallbackPtr update_cb,
    Wo_QuitCallbackPtr quit_cb
) {
    return app_new(
        max_updates_per_sec,
        window_width, window_height, window_caption,
        init_cb, update_cb, quit_cb
    );
}

bool wo_app_run(Wo_App* app_ref) {
    return app_run(app_ref);
}