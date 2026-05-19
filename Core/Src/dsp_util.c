#include "dsp_util.h"

#define DSP_PI       3.14159265358979f
#define DSP_TWO_PI   6.28318530717959f
#define DSP_INV_TWO_PI 0.159154943091895f
#define DSP_HALF_PI  1.57079632679490f

/* 5-term Taylor of sin, accurate to ~5e-7 in [-π/2, π/2]. Nested Horner form
   to share x² computations:
     sin(x) ≈ x (1 - x²/6 (1 - x²/20 (1 - x²/42 (1 - x²/72)))) */
static inline float taylor_sin(float x)
{
    float x2 = x * x;
    float t  = 1.0f - x2 * (1.0f / 72.0f);
    t = 1.0f - x2 * (1.0f / 42.0f) * t;
    t = 1.0f - x2 * (1.0f / 20.0f) * t;
    t = 1.0f - x2 * (1.0f / 6.0f)  * t;
    return x * t;
}

float dsp_util_sinf(float x)
{
    /* Reduce to [-π, π] by subtracting the nearest multiple of 2π. */
    int n = (int)(x * DSP_INV_TWO_PI + (x >= 0.0f ? 0.5f : -0.5f));
    x -= DSP_TWO_PI * (float)n;
    /* Reflect into [-π/2, π/2] where Taylor is most accurate. */
    if (x >  DSP_HALF_PI) x =  DSP_PI - x;
    else if (x < -DSP_HALF_PI) x = -DSP_PI - x;
    return taylor_sin(x);
}

float dsp_util_cosf(float x)
{
    return dsp_util_sinf(x + DSP_HALF_PI);
}
