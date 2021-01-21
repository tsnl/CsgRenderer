# See:
# https://vulkan-tutorial.com/Development_environment

cmake_minimum_required(VERSION 3.0.0)

project(Wololo LANGUAGES C VERSION 0.0.0)
set(PROJECT_C_STANDARD 11)

# fixme: need OpenGL as well just to get GLFW to build...
find_package(Vulkan REQUIRED FATAL_ERROR)

add_subdirectory(dep/glfw)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Ignoring GLFW docs")
set(GLFW_INSTALL OFF CACHE BOOL "Ignoring GLFW install (lib only)")
set(GLFW_BUILD_STATIC ON)

add_library(
    wololo STATIC
    src/wololo/glfw.h
    src/wololo/wololo.c
    src/wololo/wololo.h
)

add_executable(
    wololo_demo
    src/wololo_demo/main.c
)

# Linking/including GLFW:
include_directories(
    src/
    dep/glfw/include/
    ${Vulkan_INCLUDE_DIR}
)
target_link_libraries(
    wololo glfw ${GLFW_LIBRARIES} Vulkan::Vulkan
)
target_link_libraries(
    wololo_demo wololo
)