# Wololo Renderer

A ray-tracing renderer using Constructive Solid Geometry (CSG) to represent scenes and 
Spatiotemporal Variance-Guided Filtering (SVGF) for single sample-per-pixel ray-tracing.

Despite the complicated names, should make creating procedural assets easier, produce 
high-quality rendered images, and allow me to explore temporal filtering.

# Build

- Download, then install the Vulkan SDK (platform-specific)
    -   Note: on macOS, ensure you configure the environment variables `VULKAN_SDK` and `DYLD_LIBRARY_PATH` 
        correctly..
        See `https://vulkan.lunarg.com/doc/view/1.1.108.0/mac/getting_started.html`

- Install CMake.
- Use CMake to configure and build:
    ```
    $ cmake .
    $ cmake --build . -j8
    ```

# Try It Out

- Run the following snippet in your console:
    ```
    $ ./wololo_demo
    ```