#version 450
#extension GL_ARB_separate_shader_objects : enable

// layout(location = 0) in vec3 frag_color;
// FIXME: discard this component
layout(location = 0) in  vec3 _discard;
layout(location = 0) out vec4 out_color;

// Adding a shared Fragment Uniform Buffer Object:
// the primary way to communicate between CPU and GPU.
// see:
// https://vulkan-tutorial.com/Uniform_buffers/Descriptor_layout_and_buffer
layout(binding = 0) uniform FragmentUniformBufferObject {
    float time_since_start_sec;
    float resolution_x;
    float resolution_y;
} fubo;

vec2 resolution = vec2(fubo.resolution_x, fubo.resolution_y);
float aspect_ratio = resolution.x / resolution.y;

// 'st' coordinates are coordinates in [0,1]x[0,1] for
// the screen.
// x = 0 => left edge, x = 1 => right edge
// y = 0 => top edge,  y = 1 => bottom edge
vec2 st = vec2(
    (gl_FragCoord.x / resolution.x),
    1.0 - (gl_FragCoord.y / resolution.y)
);

// 'pq' coordinates are coordinates in resolution space.
ivec2 pq = ivec2(
    st.x * resolution.x,
    st.y * resolution.y
);

// Color constants:
vec3 COLOR_black = vec3(0,0,0);
vec3 COLOR_white = vec3(1,1,1);
vec3 COLOR_sky_blue = vec3(0.5, 0.7, 1.0);

//
//
// Raytracing:
// https://raytracing.github.io/books/RayTracingInOneWeekend.html#rays,asimplecamera,andbackground
//
//

// camera config:
float focal_length = 1.0;

vec3 origin = vec3(0,0,0);
vec3 horizontal = vec3(aspect_ratio, 0, 0);
vec3 vertical = vec3(0, 1.0, 0);
vec3 lower_left_corner = (
    origin 
    - (horizontal / 2)
    - (vertical / 2)
    - vec3(0, 0, focal_length)
);

struct RT_Ray {
    vec3 origin_pt;
    vec3 direction;
};

RT_Ray rt_ray(vec3 origin_pt, vec3 direction) {
    RT_Ray ray;
    ray.origin_pt = origin_pt;
    ray.direction = normalize(direction);
    return ray;
}

RT_Ray rt_fragment_ray() {
    return RT_Ray(
        origin,
        lower_left_corner +
        st.x * horizontal +
        st.y * vertical
        - origin
    );
}

float hit_sphere(vec3 center, float radius, RT_Ray ray) {
    vec3 oc = ray.origin_pt - center;
    float a = dot(ray.direction, ray.direction);
    float b = 2.0 * dot(oc, ray.direction);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = b*b - 4*a*c;
    if (discriminant < 0) {
        return -1.0;
    } else {
        return (-b -sqrt(discriminant)) / (2.0*a);
    }
}

vec3 ray_color(RT_Ray ray) {
    // checking sphere #1:
    {
        vec3 sphere_pos = vec3(0,0,-1);
        float amplitude = 2;
        float omega = 2 * 3.1415 / 4;
        sphere_pos.y = amplitude * sin(omega * fubo.time_since_start_sec);
        sphere_pos.z -= 10;
        float radius = 0.5;
        float t = hit_sphere(sphere_pos, radius, ray);
        if (t > 0.0) {
            vec3 normal = normalize(ray.direction * t - sphere_pos);
            return (
                0.5 * (normal + vec3(1.0, 1.0, 1.0))
            );
        }
    }

    // else background:
    {
        vec3 unit_direction = normalize(ray.direction);
        float t = unit_direction.y;
        return (
            (1.0 - t) * COLOR_white +
            (t)       * COLOR_sky_blue
        );
    }
}

//
//
// Debug View 1:
// Displays 'st' coordinates:
//
//

void ep_debug_view_1() {
    out_color = vec4(
        st.x, st.y,
        0, 1.0
    );
}

//
//
// ep_rt1_1
// - output #1 from RayTracing in one weekend
//
//

void ep_rt1_1() {
    out_color = vec4(
        ray_color(rt_fragment_ray()),
        1.0
    );
}

//
//
// Global entry point:
//
//

void main() {
    // ep_debug_view_1();
    ep_rt1_1();
}
