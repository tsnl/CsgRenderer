#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "wololo/wmath.h"
#include "wololo/app.h"

//
// A Renderer produces an image from a system of images.
//

typedef struct Wo_Renderer Wo_Renderer;
typedef uint32_t Wo_Node;
typedef uint32_t Wo_Material;

Wo_Renderer* wo_renderer_new(Wo_App* app, char const* name, size_t max_node_count);
void wo_renderer_del(Wo_Renderer* renderer);

typedef struct Wo_Node_Argument Wo_Node_Argument;
struct Wo_Node_Argument {
    Wo_Quaternion orientation;
    Wo_Vec3 offset;
    Wo_Node node;
};
Wo_Node wo_renderer_add_sphere_node(Wo_Renderer* renderer, Wo_Scalar radius);
Wo_Node wo_renderer_add_infinite_planar_partition_node(Wo_Renderer* renderer, Wo_Vec3 outward_facing_normal);
Wo_Node wo_renderer_add_union_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right);
Wo_Node wo_renderer_add_intersection_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right);
Wo_Node wo_renderer_add_difference_of_node(Wo_Renderer* renderer, Wo_Node_Argument left, Wo_Node_Argument right);

bool wo_renderer_isroot(Wo_Renderer* renderer, Wo_Node node);
