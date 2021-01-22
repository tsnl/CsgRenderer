#pragma once

//
// Scalar
//

typedef double Wo_Scalar;

//
// Vec3
//

typedef struct Wo_Vec3 Wo_Vec3;
struct Wo_Vec3 {
    Wo_Scalar x;
    Wo_Scalar y;
    Wo_Scalar z;
};

inline static Wo_Vec3 wo_vec3_add(Wo_Vec3 v, Wo_Vec3 w);
inline static Wo_Vec3 wo_vec3_subtract(Wo_Vec3 v, Wo_Vec3 w);
inline static Wo_Vec3 wo_vec3_scale(Wo_Vec3 v, Wo_Scalar scale_by);
inline static Wo_Scalar wo_vec3_dot(Wo_Vec3 v, Wo_Vec3 w);
inline static Wo_Scalar wo_vec3_lengthsqr(Wo_Vec3 v);
inline static Wo_Scalar wo_vec3_length(Wo_Vec3 v);
inline static Wo_Vec3 wo_vec3_normalized(Wo_Vec3 v);

inline static Wo_Vec3 wo_vec3_0();

//
// Quaternion
// https://www.3dgep.com/understanding-quaternions/
//

typedef struct Wo_Quaternion Wo_Quaternion;
struct Wo_Quaternion {
    Wo_Scalar real;
    Wo_Vec3 imaginary;
};

inline static Wo_Quaternion wo_quaternion_identity();

// todo
