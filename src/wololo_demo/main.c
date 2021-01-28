#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <wololo/wololo.h>
#include <wololo/wmath.h>
#include <wololo/renderer/renderer.h>

bool test1_init_cb(
    Wo_App* app, 
    uint32_t window_width, uint32_t window_height, 
    char const* window_name, 
    double target_frame_time_sec
);
void test1_update_cb(Wo_App* app, double elapsed_time_in_sec);
void test1_de_init_cb(Wo_App* app);

int main_test1() {
    Wo_App* app = wo_app_new(
        60.0,
        1280, 720, "Test 1",
        test1_init_cb, test1_update_cb, test1_de_init_cb
    );
    bool ok = wo_app_run(app);
    if (ok) {
        return 0;
    } else {
        return -1;
    }
}
bool test1_init_cb(
    Wo_App* app, 
    uint32_t window_width, uint32_t window_height, 
    char const* window_name, 
    double target_frame_time_sec
) {
    size_t max_item_count = 8;
    Wo_Renderer* renderer = wo_renderer_new(app, "Test1Render", max_item_count);
    if (renderer != NULL) {
        Wo_Node sphere1 = wo_renderer_add_sphere_node(renderer, 1.0);
        Wo_Node sphere2 = wo_renderer_add_sphere_node(renderer, 1.0);
        Wo_Node blob = wo_renderer_add_union_of_node(renderer,
            (Wo_Node_Argument) {wo_quaternion_identity(), wo_vec3_0(), sphere1},
            (Wo_Node_Argument) {wo_quaternion_identity(), wo_vec3_0(), sphere2}
        );
        printf("Sphere1 is root: %d\nSphere2 is root: %d\nBlob is root: %d\n", 
            wo_renderer_isroot(renderer,sphere1),
            wo_renderer_isroot(renderer,sphere2),
            wo_renderer_isroot(renderer,blob)
        );
        wo_app_swap_scene(app, renderer);
    } else {
        printf("[Test1] Failed to create renderer!\n");
    }
    return true;
}
void test1_update_cb(Wo_App* app, double elapsed_time_in_sec) {
    // printf("Hello!\n");
}
void test1_de_init_cb(Wo_App* app) {
    printf("Quitting...\n");
}

int main() {
    return main_test1();
}