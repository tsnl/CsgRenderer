# Wololo Renderer

A ray-tracing renderer using Computational Solid Geometry (CSG) to represent scenes and 
Spatiotemporal Variance-Guided Filtering (SVGF) for single sample-per-pixel ray-tracing.

Despite the complicated names, should make creating procedural assets easier, produce 
high-quality rendered images, and allow me to explore temporal filtering.

# Build

- Download, then install the Vulkan SDK (platform-specific)
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