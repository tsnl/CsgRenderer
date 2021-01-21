#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <wololo/wololo.h>

bool test1_init_cb(
    Wo_App* app, 
    uint32_t window_width, uint32_t window_height, 
    char const* window_name, 
    double target_frame_time_sec
);
void test1_update_cb(Wo_App* app, double elapsed_time_in_sec);
void test1_quit_cb(Wo_App* app);

int main_test1() {
    Wo_App* app_ref = wo_app_new(
        60.0,
        600, 450, "Test 1",
        test1_init_cb, test1_update_cb, test1_quit_cb
    );
    bool ok = wo_app_run(app_ref);
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
    printf("Initializing '%s' {w=%u, h=%u} @ %lf ups\n", window_name, window_width, window_height, target_frame_time_sec);
    return true;
}
void test1_update_cb(Wo_App* app, double elapsed_time_in_sec) {
    // printf("Hello!\n");
}
void test1_quit_cb(Wo_App* app) {
    printf("Quitting...");
}

int main() {
    return main_test1();
}