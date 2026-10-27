/*
 * quant.h - Farbreduktion (Paletten, Dithering, EHB, HAM)
 *
 * Reines C ohne Amiga-Abhaengigkeiten (auf dem Mac testbar: QUANT_HOST).
 */
#ifndef QUANT_H
#define QUANT_H

#ifdef QUANT_HOST
typedef int LONG;
typedef unsigned int ULONG;
typedef unsigned short UWORD;
typedef unsigned char UBYTE;
typedef short BOOL;
#define TRUE 1
#define FALSE 0
#else
#include <exec/types.h>
#endif

enum { QP_ADAPTIVE, QP_OCS, QP_WORKBENCH, QP_GRAY };            /* Palette */
enum { QM_NORMAL, QM_EHB, QM_HAM6, QM_HAM8 };                   /* Bildschirmmodus */
enum { QD_NONE, QD_FS, QD_ATKINSON, QD_ORDERED };               /* Dithering */

typedef struct {
    LONG palmode;               /* QP_* */
    LONG ncolors;               /* 2..256 (bei QM_NORMAL) */
    LONG mode;                  /* QM_* */
    LONG dither;                /* QD_* */
    LONG amount;                /* Dither-Staerke 0..100 */
    LONG optimize;              /* Palette mit k-Means verfeinern */
} QuantOpts;

typedef struct {
    ULONG col[256];             /* 0x00RRGGBB */
    LONG n;                     /* Eintraege (EHB: 64, HAM6: 16, HAM8: 64) */
    LONG mode;                  /* QM_* */
    BOOL ocs;                   /* 12-Bit-Palette (4 Bit je Kanal) */
} QPalette;

/* Palette aus Pixeln (ARGB straight; Alpha < 128 zaehlt nicht) bilden */
BOOL quant_palette(const ULONG *px, LONG n, const QuantOpts *o, QPalette *pal);
/* wie quant_palette, mit Bildgeometrie (HAM: Palette auf Farbspruenge optimiert) */
BOOL quant_palette_img(const ULONG *px, LONG w, LONG h, const QuantOpts *o, QPalette *pal);

/* Pixel auf die Palette abbilden (an Ort und Stelle, Alpha wird 0 oder 255).
 * idx (optional): je Pixel der Wert fuer die Bitplanes (Index bzw. HAM-Code). */
BOOL quant_map(ULONG *px, LONG w, LONG h, LONG stride, const QuantOpts *o, const QPalette *pal, UBYTE *idx);

/* Anzahl der Bitplanes fuer die Palette */
LONG quant_planes(const QPalette *pal);

#endif
