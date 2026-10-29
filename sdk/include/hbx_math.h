/*
 * hbx_math.h - small freestanding float helpers (no libm).
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#ifndef HBX_MATH_H
#define HBX_MATH_H

#define HBX_PI 3.14159265358979323846f
#define HBX_TAU 6.28318530717958647692f

float hbx_sinf(float x);
float hbx_cosf(float x);
float hbx_sqrtf(float x);
float hbx_fabsf(float x);
float hbx_floorf(float x);
float hbx_minf(float a, float b);
float hbx_maxf(float a, float b);
float hbx_clampf(float value, float low, float high);
float hbx_lerpf(float a, float b, float t);
float hbx_smoothstepf(float edge0, float edge1, float x);
float hbx_fmodf(float value, float modulus);

#endif
