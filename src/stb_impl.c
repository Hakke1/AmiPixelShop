/*
 * stb_impl.c - Implementierung von stb_image / stb_image_write (Public Domain)
 */
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STBIW_ASSERT(x) ((void)0)
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* Deflate von zlib statt dem einfachen von stb: kleiner und schneller */
#include <stdlib.h>
#include "zlib.h"
static unsigned char *apx_zlib_compress(unsigned char *data, int len, int *out_len, int quality)
{
    uLongf dl = compressBound(len);
    unsigned char *out = malloc(dl);
    (void)quality;
    if (!out)
        return NULL;
    if (compress2(out, &dl, data, len, 6) != Z_OK) {
        free(out);
        return NULL;
    }
    *out_len = (int)dl;
    return out;
}
#define STBIW_ZLIB_COMPRESS apx_zlib_compress

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* stb_truetype: Mathe ueber fmath.h (keine Amiga-Mathe-Libraries) */
#include <math.h>
#include "fmath.h"
#define STBTT_ifloor(x)   ((int)apx_floorf((float)(x)))
#define STBTT_iceil(x)    ((int)apx_ceilf((float)(x)))
#define STBTT_sqrt(x)     apx_sqrtf((float)(x))
#define STBTT_pow(x, y)   apx_powf((float)(x), (float)(y))
#define STBTT_fmod(x, y)  ((x) - (float)(long)((x) / (y)) * (y))
#define STBTT_cos(x)      apx_cosf((float)(x))
#define STBTT_acos(x)     apx_acosf((float)(x))
#define STBTT_fabs(x)     apx_fabsf((float)(x))
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
