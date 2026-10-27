/*
 * fmath.h - Mathe ohne Bibliotheksaufrufe
 *
 * Die libnix-Funktionen (floorf, ceilf, sinf ...) rufen die Amiga-Libraries
 * mathieeesingbas/-trans auf, und die zerstoeren die FPU-Register fp2-fp7.
 * gcc haelt dort aber Zwischenwerte ueber den Aufruf hinweg - Ergebnis waren
 * falsche Koordinaten. Deshalb hier alles inline: Rundung per Ganzzahl,
 * Transzendente direkt mit FPU-Befehlen (68040: per FPSP emuliert, Emu68 nativ).
 * Muss nach <math.h> eingebunden werden.
 */
#ifndef FMATH_H
#define FMATH_H

static inline float apx_floorf(float x)
{
    long i = (long)x;               /* schneidet Richtung 0 ab */
    return (float)(x < (float)i ? i - 1 : i);
}

static inline float apx_ceilf(float x)
{
    long i = (long)x;
    return (float)(x > (float)i ? i + 1 : i);
}

static inline float apx_sinf(float x)
{
    float r;
    __asm__("fsin.x %1,%0" : "=f"(r) : "f"(x));
    return r;
}

static inline float apx_cosf(float x)
{
    float r;
    __asm__("fcos.x %1,%0" : "=f"(r) : "f"(x));
    return r;
}

static inline float apx_atanf(float x)
{
    float r;
    __asm__("fatan.x %1,%0" : "=f"(r) : "f"(x));
    return r;
}

static inline float apx_atan2f(float y, float x)
{
    const float pi = 3.14159265f;
    if (x > 0.0f)
        return apx_atanf(y / x);
    if (x < 0.0f)
        return apx_atanf(y / x) + (y >= 0.0f ? pi : -pi);
    return y > 0.0f ? pi * 0.5f : y < 0.0f ? -pi * 0.5f : 0.0f;
}

static inline float apx_acosf(float x)
{
    float t;
    __asm__("fsqrt.x %1,%0" : "=f"(t) : "f"(1.0f - x * x));
    return apx_atan2f(t, x);
}

static inline float apx_powf(float x, float y)
{
    float l, r;
    if (x <= 0.0f)
        return 0.0f;
    __asm__("flogn.x %1,%0" : "=f"(l) : "f"(x));
    l *= y;
    __asm__("fetox.x %1,%0" : "=f"(r) : "f"(l));
    return r;
}

static inline float apx_sqrtf(float x)
{
    float r;
    __asm__("fsqrt.x %1,%0" : "=f"(r) : "f"(x));
    return r;
}

static inline float apx_fabsf(float x)
{
    float r;
    __asm__("fabs.x %1,%0" : "=f"(r) : "f"(x));
    return r;
}

#define floorf apx_floorf
#define ceilf  apx_ceilf
#define sinf   apx_sinf
#define cosf   apx_cosf
#define atan2f apx_atan2f
#define powf   apx_powf
#define sqrtf  apx_sqrtf
#define sqrt(x) ((double)apx_sqrtf((float)(x)))
#define fabsf  apx_fabsf

#endif
