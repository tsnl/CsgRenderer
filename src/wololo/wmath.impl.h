#pragma once

#include <math.h>

#include "wmath.decl.h"

//
// Vec3
//

inline static Wo_Vec3 wo_vec3_add(Wo_Vec3 v, Wo_Vec3 w) {
    Wo_Vec3 r = {
        .x = v.x + w.x,
        .y = v.y + w.y,
        .z = v.z + w.z
    };
    return r;
}
inline static Wo_Vec3 wo_vec3_subtract(Wo_Vec3 v, Wo_Vec3 w) {
    Wo_Vec3 r = {
        .x = v.x - w.x,
        .y = v.y - w.y,
        .z = v.z - w.z
    };
    return r;
}
inline static Wo_Vec3 wo_vec3_scale(Wo_Vec3 v, Wo_Scalar scale_by) {
    Wo_Vec3 r = {
        .x = v.x * scale_by,
        .y = v.y * scale_by,
        .z = v.z * scale_by
    };
    return r;
}
inline static Wo_Scalar wo_vec3_dot(Wo_Vec3 v, Wo_Vec3 w) {
    return (
        v.x * w.x +
        v.y * w.y +
        v.z * w.z
    );
}
inline static Wo_Scalar wo_vec3_lengthsqr(Wo_Vec3 v) {
    return wo_vec3_dot(v,v);
}
inline static Wo_Scalar wo_vec3_length(Wo_Vec3 v) {
    return sqrt(wo_vec3_lengthsqr(v));
}
inline static Wo_Vec3 wo_vec3_normalized(Wo_Vec3 v) {
    Wo_Scalar length = wo_vec3_lengthsqr(v);
    if (length == 0.0) {
        return v;
    } else {
        return wo_vec3_scale(v, 1.0/length);
    }
}

inline static Wo_Vec3 wo_vec3_0() {
    Wo_Vec3 zero = {0,0,0};
    return zero;
}

//
// Quaternion
//


inline static Wo_Quaternion wo_quaternion_identity() {
    Wo_Quaternion quat = {1,wo_vec3_0()};
    return quat;
}