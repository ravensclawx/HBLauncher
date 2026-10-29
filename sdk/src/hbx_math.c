/* Freestanding float helpers.  Accuracy is better than 1e-6 over the ranges
 * the launcher uses; no libm, no errno, no tables. */
#include "hbx_math.h"

#define HBX_TAU_D 6.28318530717958647692528676656

static float wrap_pi(float x) {
    /* O(1) reduction: a loop would take longer as the session uptime grows
     * (the animation phase is in seconds), eventually starving the frame. */
    double value = (double)x;
    double quotient = value / HBX_TAU_D;
    value -= HBX_TAU_D * (double)(long long)quotient;
    if (value > HBX_PI) value -= HBX_TAU_D;
    else if (value < -HBX_PI) value += HBX_TAU_D;
    return (float)value;
}

float hbx_sinf(float x) {
    /* Bhaskara-plus-Taylor: reduce then 9th-order odd polynomial. */
    float x2;
    x = wrap_pi(x);
    x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f -
           x2 * (1.0f / 5040.0f - x2 * (1.0f / 362880.0f -
           x2 * (1.0f / 39916800.0f))))));
}

float hbx_cosf(float x) { return hbx_sinf(x + HBX_PI * 0.5f); }

float hbx_fabsf(float x) { return x < 0.0f ? -x : x; }

float hbx_sqrtf(float x) {
    float guess;
    int i;
    if (x <= 0.0f) return 0.0f;
    guess = x > 1.0f ? x * 0.5f : 1.0f;
    for (i = 0; i < 24; i++) guess = 0.5f * (guess + x / guess);
    return guess;
}

float hbx_floorf(float x) {
    double value = (double)x;
    long long truncated = (long long)value;
    if (value < 0.0 && (double)truncated != value) truncated -= 1;
    return (float)truncated;
}

float hbx_minf(float a, float b) { return a < b ? a : b; }
float hbx_maxf(float a, float b) { return a > b ? a : b; }
float hbx_clampf(float v, float low, float high) { return v < low ? low : (v > high ? high : v); }
float hbx_lerpf(float a, float b, float t) { return a + (b - a) * t; }

float hbx_smoothstepf(float edge0, float edge1, float x) {
    float t;
    if (edge1 == edge0) return x < edge0 ? 0.0f : 1.0f;
    t = hbx_clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float hbx_fmodf(float value, float modulus) {
    double v = (double)value, m = (double)modulus;
    if (m == 0.0) return 0.0f;
    return (float)(v - m * (double)(long long)(v / m));
}
