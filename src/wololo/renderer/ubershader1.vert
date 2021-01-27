// Source:
// https://vulkan-tutorial.com/Drawing_a_triangle/Graphics_pipeline_basics/Shader_modules

#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec3 fragColor;

vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2(+1.0, +1.0),
    vec2(-1.0, +1.0),

    vec2(-1.0, -1.0),
    vec2(+1.0, -1.0),
    vec2(+1.0, +1.0)
);

vec3 colors[6] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(1.0, 1.0, 0.0),

    vec3(0.0, 0.0, 1.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 1.0, 1.0)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}