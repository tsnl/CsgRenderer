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

void glfw_error_handler(int code, char const* message);

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

    // Setting the error callback:
    glfwSetErrorCallback(glfw_error_handler);

    // Create a windowed mode window and its OpenGL context
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
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

    // first of many 'clocks'... should be abstracted.
    double interval_between_frametime_reports_sec = 1.0;
    // assume one report has been printed already, so we wait before the first report.
    // now reports are 'scheduled' every N seconds, with this counter incrementing N.
    size_t reports_printed_so_far = 1;
    // we sum the frame times and sq frame times, and display mean and stddev
    // see: https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
    size_t frames_counted_since_last_report = 0;
    double sum_of_frame_time_diffs_sec = 0.0;
    double sum_of_sq_frame_time_diffs_sec_sq = 0.0;

    while (!glfwWindowShouldClose(app->glfw_window))
    {
        // Updating:
        {
            // calculating elapsed time difference:
            double time_at_this_frame_sec = glfwGetTime();
            double time_difference_sec = time_at_this_frame_sec - time_at_last_frame_sec;
            assert(time_difference_sec >= 0 && "glfwGetTime not monotonic-- who called glfwSetTime?");
            time_at_last_frame_sec = time_at_this_frame_sec;

            // adding the difference to the 'running behind' count...
            total_running_behind_by_sec += time_difference_sec;
            
            // and then running updates at the desired resolution until we aren't running behind
            // by a frame anymore:
            while (app->extension_update_cb != NULL && total_running_behind_by_sec >= app->update_time_sec) {
                total_running_behind_by_sec -= app->update_time_sec;
                app->extension_update_cb(app, app->update_time_sec);
            }

            // updating frame time reports:
            frames_counted_since_last_report++;
            sum_of_frame_time_diffs_sec += time_difference_sec;
            sum_of_sq_frame_time_diffs_sec_sq += time_difference_sec * time_difference_sec;

            // printing frame-time reports:
            // print at most one per frame.
            if (time_at_this_frame_sec > reports_printed_so_far * interval_between_frametime_reports_sec) {
                reports_printed_so_far++;
                
                if (frames_counted_since_last_report == 1) {
                    // really bad! we just printed a report last frame, must be printing reports too frequently
                    printf("... See above (printing reports too frequently)\n");
                } else {
                    double sx2 = sum_of_sq_frame_time_diffs_sec_sq;
                    size_t sx = sum_of_frame_time_diffs_sec;
                    size_t n = frames_counted_since_last_report;

                    // using 'a naive algorithm to calculate estimated variance'
                    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
                    double fps = n / interval_between_frametime_reports_sec;
                    double mean_ft_sec = sx / n;
                    double stddev_ft = (
                        (sx2 - (sx * sx / n)) / 
                        (n - 1)
                    );
                    printf(
                        "[Wololo][Stats] | %zu frames / %.3lf sec = %.3lf fps | Avg. Frame-Time: %.3lf sec | Stddev. Frame-Time: %.3lf |\n",
                        n, interval_between_frametime_reports_sec, fps,
                        mean_ft_sec,
                        stddev_ft
                    );
                    
                    // resetting metrics:
                    frames_counted_since_last_report = 0;
                    sum_of_frame_time_diffs_sec = 0;
                    sum_of_sq_frame_time_diffs_sec_sq = 0;
                }
            }
        }

        // Rendering here using renderer:
        wo_renderer_draw_frame(app->renderer);
        
        // Swap front and back buffers
        // glfwSwapBuffers(app->glfw_window);

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

void glfw_error_handler(int code, char const* message) {
    printf("[GLFW] %s (%d)\n", message, code);
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

GLFWwindow* wo_app_glfw_window(Wo_App* app) {
    return app->glfw_window;
}
