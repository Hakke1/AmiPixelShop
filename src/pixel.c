/*
 * pixel.c - heisse Pixelschleifen (C-Referenz, spaeter Assembler)
 *
 * Formate:
 *   Ebenen-Kachel  ARGB straight alpha
 *   flat           ARGB premultiplied
 *   Backbuffer     ARGB, Alpha immer 0xFF
 */
#include <exec/types.h>
#include <string.h>
#include <math.h>
#include <proto/exec.h>
#include "apx.h"

ULONG inv_tab[256];             /* 2^16 / n, fuer Division per Multiplikation */

/* Schachbrett-Tabellen: grau * (1 - a) je Alpha, schon auf alle drei Kanaele
 * verteilt (fuer die Assembler-Anzeige) */
ULONG chk_tab_ff[256], chk_tab_cc[256];

static UBYTE softlight_d[256];  /* W3C: D(cb) fuer Weiches Licht */

/* Aktive Varianten: Assembler, sofern vorhanden (siehe pixel_asm.s) */
void (*span_show_mix)(ULONG *, const ULONG *, LONG, LONG, LONG) = span_show_mix_asm;
void (*span_show_mix_map)(ULONG *, const ULONG *, const LONG *, LONG, LONG, LONG) = span_show_mix_map_asm;
/* Ebene: gcc ist hier so gut wie Handarbeit (Benchmark), daher C */
void (*span_layer_over)(ULONG *, const ULONG *, LONG, ULONG) = span_layer_over_c;
void (*blur_h)(const ULONG *, ULONG *, LONG, LONG, LONG, ULONG *) = blur_h_asm;
void (*blur_v)(const ULONG *, ULONG *, LONG, LONG, LONG, ULONG *) = blur_v_asm;

void pixel_init(void)
{
    LONG i;
    inv_tab[0] = 0;
    for (i = 1; i < 256; i++)
        inv_tab[i] = (65536 + i / 2) / i;
    for (i = 0; i < 256; i++) {
        /* W3C Soft Light: D(x) = x <= 0.25 ? ((16x - 12)x + 4)x : sqrt(x) */
        float x = i / 255.0f, dv = x <= 0.25f ? ((16.0f * x - 12.0f) * x + 4.0f) * x : sqrtf(x);
        softlight_d[i] = (UBYTE)(dv * 255.0f + 0.5f);
    }
    for (i = 0; i < 256; i++) {
        ULONG a = i + (i >> 7);
        chk_tab_ff[i] = ((0xFF * (256 - a)) >> 8) * 0x010101;
        chk_tab_cc[i] = ((0xCC * (256 - a)) >> 8) * 0x010101;
    }
}

/* Schachbrett 8x8 in Bildschirmkoordinaten. cx = x relativ zum Bildursprung,
 * cyp = Zeilenparitaet (0/1). */
void span_checker(ULONG *d, LONG n, LONG cx, LONG cyp)
{
    while (n > 0) {
        LONG k = 8 - (cx & 7), i;
        ULONG c = (((cx >> 3) ^ cyp) & 1) ? 0xFFCCCCCC : 0xFFFFFFFF;
        if (k > n)
            k = n;
        for (i = 0; i < k; i++)
            d[i] = c;
        d += k;
        cx += k;
        n -= k;
    }
}

/* premultiplied ueber Schachbrett: d = p + grau * (1 - a) */
void span_show_mix_c(ULONG *d, const ULONG *s, LONG n, LONG cx, LONG cyp)
{
    LONG i;
    for (i = 0; i < n; i++) {
        ULONG p = s[i];
        ULONG a = p >> 24;
        ULONG g = ((((cx + i) >> 3) ^ cyp) & 1) ? 0xCC : 0xFF;
        a += a >> 7;
        g = (g * (256 - a)) >> 8;
        d[i] = (p + g * 0x010101) | 0xFF000000;
    }
}

void span_show_mix_map_c(ULONG *d, const ULONG *row, const LONG *xmap, LONG n, LONG cx, LONG cyp)
{
    LONG i;
    for (i = 0; i < n; i++) {
        ULONG p = row[xmap[i]];
        ULONG a = p >> 24;
        ULONG g = ((((cx + i) >> 3) ^ cyp) & 1) ? 0xCC : 0xFF;
        a += a >> 7;
        g = (g * (256 - a)) >> 8;
        d[i] = (p + g * 0x010101) | 0xFF000000;
    }
}

void span_copy_map(ULONG *d, const ULONG *row, const LONG *xmap, LONG n)
{
    LONG i;
    for (i = 0; i < n; i++)
        d[i] = row[xmap[i]];
}

/* d = premul(s * op) */
void span_layer_first(ULONG *d, const ULONG *s, LONG n, ULONG op)
{
    LONG i;
    op += op >> 7;
    for (i = 0; i < n; i++) {
        ULONG p = s[i];
        ULONG a = ((p >> 24) * op) >> 8, a2;
        a2 = a + (a >> 7);
        d[i] = (a << 24)
             | ((((p & 0xFF00FF) * a2) >> 8) & 0xFF00FF)
             | ((((p & 0x00FF00) * a2) >> 8) & 0x00FF00);
    }
}

/* d = premul(s * op) + d * (1 - sa)    (d premultiplied) */
void span_layer_over_c(ULONG *d, const ULONG *s, LONG n, ULONG op)
{
    LONG i;
    op += op >> 7;
    for (i = 0; i < n; i++) {
        ULONG p = s[i], q, a, a2, inv;
        a = ((p >> 24) * op) >> 8;
        if (!a)
            continue;
        if (a == 255) {         /* deckend bei 100 %: Ergebnis ist das Pixel selbst */
            d[i] = p;
            continue;
        }
        a2 = a + (a >> 7);
        inv = 256 - a2;
        q = d[i];
        d[i] = ((a << 24)
                | ((((p & 0xFF00FF) * a2) >> 8) & 0xFF00FF)
                | ((((p & 0x00FF00) * a2) >> 8) & 0x00FF00))
             + ((((q & 0xFF00FF) * inv) >> 8) & 0xFF00FF)
             + ((((q >> 8) & 0xFF00FF) * inv) & 0xFF00FF00);
    }
}

/*
 * Pinseltupfer mit Strich-Puffer (wie Photoshop):
 *   cov  = Deckung des Strichs bisher, waechst mit jedem Tupfer (Fluss)
 *   px   = orig ueber (Farbe, cov * Deckkraft * Auswahl)
 * So kann ein Strich nie deckender werden als die eingestellte Deckkraft.
 */
void span_dab(ULONG *px, const ULONG *orig, UBYTE *cov, const UBYTE *mask,
              const UBYTE *sel, LONG n, ULONG color, ULONG flow, ULONG opacity, LONG erase)
{
    ULONG cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    LONG i;
    flow += flow >> 7;
    opacity += opacity >> 7;
    for (i = 0; i < n; i++) {
        ULONG m = mask[i], c, sa, o, da;
        if (!m)
            continue;
        m = (m * flow) >> 8;
        c = cov[i];
        c += ((255 - c) * m * 257 + 0x8080) >> 16;
        cov[i] = (UBYTE)c;
        sa = (c * opacity) >> 8;
        if (sel)
            sa = (sa * sel[i] * 257 + 0x8080) >> 16;
        o = orig[i];
        da = o >> 24;
        if (erase) {
            da = (da * (255 - sa) * 257 + 0x8080) >> 16;
            px[i] = (da << 24) | (o & 0xFFFFFF);
        } else {
            ULONG oa, inv;
            da = (da * (255 - sa) * 257 + 0x8080) >> 16;
            oa = sa + da;
            if (!oa) {
                px[i] = o;
                continue;
            }
            inv = inv_tab[oa];
            px[i] = (oa << 24)
                  | (((cr * sa + ((o >> 16) & 0xFF) * da) * inv >> 16) << 16)
                  | (((cg * sa + ((o >> 8) & 0xFF) * da) * inv >> 16) << 8)
                  | ((cb * sa + (o & 0xFF) * da) * inv >> 16);
        }
    }
}

/* ---- Box-Unschaerfe (fuer Gauss), Daten vormultipliziert ---- */

/* waagerechte Box, src -> dst, Rand wird fortgesetzt */
/* Zeile in pad kopieren und links/rechts um r mit dem Randpixel auffuellen:
 * danach braucht die Fensterschleife keine Randabfragen (Spruenge sind auf
 * Emu68 teuer). pad muss w + 2r + 1 Eintraege fassen. */
static void pad_row(const ULONG *s, ULONG *pad, LONG w, LONG r)
{
    LONG i;
    ULONG first = s[0], last = s[w - 1];
    for (i = 0; i < r; i++)
        pad[i] = first;
    CopyMemQuick((APTR)s, pad + r, w * 4);
    for (i = r + w; i <= w + 2 * r; i++)
        pad[i] = last;
}

void blur_h_c(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *pad)
{
    ULONG inv = 65536 / (2 * r + 1);
    LONG x, y, i;
    for (y = 0; y < h; y++) {
        const ULONG *po = pad, *pi = pad + 2 * r + 1;
        ULONG *d = dst + y * w;
        ULONG sa = 0, sr = 0, sg = 0, sb = 0;
        pad_row(src + y * w, pad, w, r);
        for (i = 0; i <= 2 * r; i++) {
            ULONG v = pad[i];
            sa += v >> 24; sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF;
        }
        for (x = 0; x < w; x++) {
            ULONG vo = *po++, vi = *pi++;
            d[x] = (((sa * inv + 32768) >> 16) << 24) | (((sr * inv + 32768) >> 16) << 16)
                 | (((sg * inv + 32768) >> 16) << 8) | ((sb * inv + 32768) >> 16);
            sa += (vi >> 24) - (vo >> 24);
            sr += ((vi >> 16) & 0xFF) - ((vo >> 16) & 0xFF);
            sg += ((vi >> 8) & 0xFF) - ((vo >> 8) & 0xFF);
            sb += (vi & 0xFF) - (vo & 0xFF);
        }
    }
}

/* senkrechte Box mit Spaltensummen (zeilenweise, cache-freundlich) */
void blur_v_c(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *sums)
{
    ULONG inv = 65536 / (2 * r + 1);
    LONG x, y, i;
    ULONG *sa = sums, *sr = sums + w, *sg = sums + 2 * w, *sb = sums + 3 * w;
    memset(sums, 0, 4 * w * sizeof(ULONG));
    for (i = -r; i <= r; i++) {
        const ULONG *s = src + (i < 0 ? 0 : i >= h ? h - 1 : i) * w;
        for (x = 0; x < w; x++) {
            ULONG v = s[x];
            sa[x] += v >> 24; sr[x] += (v >> 16) & 0xFF; sg[x] += (v >> 8) & 0xFF; sb[x] += v & 0xFF;
        }
    }
    for (y = 0; y < h; y++) {
        ULONG *d = dst + y * w;
        const ULONG *so = src + (y - r < 0 ? 0 : y - r) * w;
        const ULONG *si = src + (y + r + 1 >= h ? h - 1 : y + r + 1) * w;
        for (x = 0; x < w; x++) {
            ULONG vo = so[x], vi = si[x];
            d[x] = (((sa[x] * inv + 32768) >> 16) << 24) | (((sr[x] * inv + 32768) >> 16) << 16)
                 | (((sg[x] * inv + 32768) >> 16) << 8) | ((sb[x] * inv + 32768) >> 16);
            sa[x] += (vi >> 24) - (vo >> 24);
            sr[x] += ((vi >> 16) & 0xFF) - ((vo >> 16) & 0xFF);
            sg[x] += ((vi >> 8) & 0xFF) - ((vo >> 8) & 0xFF);
            sb[x] += (vi & 0xFF) - (vo & 0xFF);
        }
    }
}


/* ---- Fuellmethoden ----
 * Hintergrund d premultiplied, Quelle s straight. Je Methode eine eigene
 * Schleife (kein switch pro Pixel). Kanalwerte 0..255.
 */
const char *blend_names[BM_COUNT] = {
    "Normal",
    "Darken", "Multiply", "Color Burn", "Linear Burn",
    "Lighten", "Screen", "Color Dodge", "Linear Dodge (Add)",
    "Overlay", "Soft Light", "Hard Light", "Vivid Light",
    "Linear Light", "Pin Light", "Hard Mix",
    "Difference", "Exclusion", "Subtract", "Divide",
    "Hue", "Saturation", "Color", "Luminosity"
};

#define DIV255(x) ((((x) + 128) + (((x) + 128) >> 8)) >> 8)
#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))


static inline LONG f_burn(LONG b, LONG s)  { return s == 0 ? (b == 255 ? 255 : 0) : 255 - CL((255 - b) * 255 / s); }
static inline LONG f_dodge(LONG b, LONG s) { return s == 255 ? (b == 0 ? 0 : 255) : CL(b * 255 / (255 - s)); }

#define F_DARKEN(b, s)      ((b) < (s) ? (b) : (s))
#define F_MULTIPLY(b, s)    DIV255((b) * (s))
#define F_COLORBURN(b, s)   f_burn(b, s)
#define F_LINEARBURN(b, s)  CL((b) + (s) - 255)
#define F_LIGHTEN(b, s)     ((b) > (s) ? (b) : (s))
#define F_SCREEN(b, s)      ((b) + (s) - DIV255((b) * (s)))
#define F_COLORDODGE(b, s)  f_dodge(b, s)
#define F_LINEARDODGE(b, s) CL((b) + (s))
#define F_HARDLIGHT(b, s)   ((s) < 128 ? DIV255(2 * (b) * (s)) : 255 - DIV255(2 * (255 - (b)) * (255 - (s))))
#define F_OVERLAY(b, s)     F_HARDLIGHT(s, b)
#define F_SOFTLIGHT(b, s)   ((s) < 128 ? (b) - DIV255(DIV255((255 - 2 * (s)) * (b)) * (255 - (b))) \
                                       : (b) + DIV255((2 * (s) - 255) * (softlight_d[b] - (b))))
#define F_VIVIDLIGHT(b, s)  ((s) < 128 ? f_burn(b, 2 * (s)) : f_dodge(b, 2 * (s) - 255))
#define F_LINEARLIGHT(b, s) CL((b) + 2 * (s) - 255)
#define F_PINLIGHT(b, s)    ((s) < 128 ? ((b) < 2 * (s) ? (b) : 2 * (s)) \
                                       : ((b) > 2 * (s) - 255 ? (b) : 2 * (s) - 255))
#define F_HARDMIX(b, s)     ((b) + (s) >= 255 ? 255 : 0)
#define F_DIFFERENCE(b, s)  ((b) > (s) ? (b) - (s) : (s) - (b))
#define F_EXCLUSION(b, s)   ((b) + (s) - 2 * DIV255((b) * (s)))
#define F_SUBTRACT(b, s)    CL((b) - (s))
#define F_DIVIDE(b, s)      ((s) == 0 ? ((b) == 0 ? 0 : 255) : CL((b) * 255 / (s)))

/* Ergebnis (premultiplied) = as(1-ab)cs + as*ab*B(cb,cs) + (1-as)*cb_premul */
#define BLEND_FN(NAME, F)                                                       \
static void NAME(ULONG *d, const ULONG *s, LONG n, ULONG op)                    \
{                                                                               \
    LONG i;                                                                     \
    for (i = 0; i < n; i++) {                                                   \
        ULONG p = s[i], q, as, ab, ar, inv, out;                                \
        LONG sh;                                                                \
        as = DIV255((p >> 24) * op);                                            \
        if (!as)                                                                \
            continue;                                                           \
        q = d[i];                                                               \
        ab = q >> 24;                                                           \
        if (ab == 255) {                                                        \
            /* Untergrund deckend (Normalfall): keine Rueckrechnung noetig */    \
            LONG ia = 255 - as;                                                 \
            out = 0xFF000000;                                                   \
            for (sh = 0; sh < 24; sh += 8) {                                    \
                LONG cs = (p >> sh) & 0xFF, qc = (q >> sh) & 0xFF;              \
                LONG m = F(qc, cs);                                             \
                out |= (ULONG)DIV255(qc * ia + m * (LONG)as) << sh;             \
            }                                                                   \
            d[i] = out;                                                         \
            continue;                                                           \
        }                                                                       \
        ar = as + DIV255(ab * (255 - as));                                      \
        inv = ab ? inv_tab[ab] * 255 : 0;  /* premul -> straight */             \
        out = ar << 24;                                                         \
        for (sh = 0; sh < 24; sh += 8) {                                        \
            LONG cs = (p >> sh) & 0xFF, qc = (q >> sh) & 0xFF;                  \
            LONG cb = ab == 255 ? qc : (LONG)((qc * inv + 32768) >> 16);        \
            LONG m, v;                                                          \
            if (cb > 255) cb = 255;                                             \
            m = F(cb, cs);                                                      \
            v = DIV255(as * DIV255((255 - ab) * cs + ab * m)) + DIV255((255 - as) * qc); \
            if (v > (LONG)ar) v = ar;                                           \
            out |= (ULONG)v << sh;                                              \
        }                                                                       \
        d[i] = out;                                                             \
    }                                                                           \
}

BLEND_FN(bl_darken, F_DARKEN)
BLEND_FN(bl_multiply, F_MULTIPLY)
BLEND_FN(bl_colorburn, F_COLORBURN)
BLEND_FN(bl_linearburn, F_LINEARBURN)
BLEND_FN(bl_lighten, F_LIGHTEN)
BLEND_FN(bl_screen, F_SCREEN)
BLEND_FN(bl_colordodge, F_COLORDODGE)
BLEND_FN(bl_lineardodge, F_LINEARDODGE)
BLEND_FN(bl_overlay, F_OVERLAY)
BLEND_FN(bl_softlight, F_SOFTLIGHT)
BLEND_FN(bl_hardlight, F_HARDLIGHT)
BLEND_FN(bl_vividlight, F_VIVIDLIGHT)
BLEND_FN(bl_linearlight, F_LINEARLIGHT)
BLEND_FN(bl_pinlight, F_PINLIGHT)
BLEND_FN(bl_hardmix, F_HARDMIX)
BLEND_FN(bl_difference, F_DIFFERENCE)
BLEND_FN(bl_exclusion, F_EXCLUSION)
BLEND_FN(bl_subtract, F_SUBTRACT)
BLEND_FN(bl_divide, F_DIVIDE)

/* ---- nicht trennbare Methoden (W3C): Lum/Sat/ClipColor auf 0..255 ---- */

static inline LONG lum3(LONG r, LONG g, LONG b) { return (r * 77 + g * 151 + b * 28 + 128) >> 8; }

static void clip_color(LONG *c)
{
    LONG l = lum3(c[0], c[1], c[2]), n = c[0], x = c[0], k;
    for (k = 1; k < 3; k++) {
        if (c[k] < n) n = c[k];
        if (c[k] > x) x = c[k];
    }
    if (n < 0 && l != n)
        for (k = 0; k < 3; k++)
            c[k] = l + (c[k] - l) * l / (l - n);
    if (x > 255 && x != l)
        for (k = 0; k < 3; k++)
            c[k] = l + (c[k] - l) * (255 - l) / (x - l);
}

static void set_lum(LONG *c, LONG l)
{
    LONG d = l - lum3(c[0], c[1], c[2]), k;
    for (k = 0; k < 3; k++)
        c[k] += d;
    clip_color(c);
}

static LONG sat3(const LONG *c)
{
    LONG n = c[0], x = c[0], k;
    for (k = 1; k < 3; k++) {
        if (c[k] < n) n = c[k];
        if (c[k] > x) x = c[k];
    }
    return x - n;
}

static void set_sat(LONG *c, LONG s)
{
    LONG imax = 0, imin = 0, imid, k;
    for (k = 1; k < 3; k++) {
        if (c[k] > c[imax]) imax = k;
        if (c[k] < c[imin]) imin = k;
    }
    if (imax == imin) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    imid = 3 - imax - imin;
    c[imid] = (c[imid] - c[imin]) * s / (c[imax] - c[imin]);
    c[imax] = s;
    c[imin] = 0;
}

static void bl_nonsep(ULONG *d, const ULONG *s, LONG n, ULONG op, LONG mode)
{
    LONG i;
    for (i = 0; i < n; i++) {
        ULONG p = s[i], q, as, ab, ar, inv, out;
        LONG cs[3], cb[3], m[3], k;
        as = DIV255((p >> 24) * op);
        if (!as)
            continue;
        q = d[i];
        ab = q >> 24;
        ar = as + DIV255(ab * (255 - as));
        inv = ab ? inv_tab[ab] * 255 : 0;
        for (k = 0; k < 3; k++) {
            LONG sh = 16 - 8 * k, qc = (q >> sh) & 0xFF;
            cs[k] = (p >> sh) & 0xFF;
            cb[k] = ab == 255 ? qc : (LONG)((qc * inv + 32768) >> 16);
            if (cb[k] > 255) cb[k] = 255;
        }
        switch (mode) {
        case BM_HUE:            /* Farbton der Quelle, Saettigung+Helligkeit des Hintergrunds */
            m[0] = cs[0]; m[1] = cs[1]; m[2] = cs[2];
            set_sat(m, sat3(cb));
            set_lum(m, lum3(cb[0], cb[1], cb[2]));
            break;
        case BM_SATURATION:
            m[0] = cb[0]; m[1] = cb[1]; m[2] = cb[2];
            set_sat(m, sat3(cs));
            set_lum(m, lum3(cb[0], cb[1], cb[2]));
            break;
        case BM_COLOR:
            m[0] = cs[0]; m[1] = cs[1]; m[2] = cs[2];
            set_lum(m, lum3(cb[0], cb[1], cb[2]));
            break;
        default:                /* Luminosity */
            m[0] = cb[0]; m[1] = cb[1]; m[2] = cb[2];
            set_lum(m, lum3(cs[0], cs[1], cs[2]));
            break;
        }
        out = ar << 24;
        for (k = 0; k < 3; k++) {
            LONG sh = 16 - 8 * k, qc = (q >> sh) & 0xFF, mm = CL(m[k]), v;
            v = DIV255(as * DIV255((255 - ab) * cs[k] + ab * mm)) + DIV255((255 - as) * qc);
            if (v > (LONG)ar) v = ar;
            out |= (ULONG)v << sh;
        }
        d[i] = out;
    }
}

static void (*const blend_fns[BM_COUNT])(ULONG *, const ULONG *, LONG, ULONG) = {
    NULL,
    bl_darken, bl_multiply, bl_colorburn, bl_linearburn,
    bl_lighten, bl_screen, bl_colordodge, bl_lineardodge,
    bl_overlay, bl_softlight, bl_hardlight, bl_vividlight, bl_linearlight, bl_pinlight, bl_hardmix,
    bl_difference, bl_exclusion, bl_subtract, bl_divide,
    NULL, NULL, NULL, NULL      /* nicht trennbar: bl_nonsep */
};

void span_blend(ULONG *d, const ULONG *s, LONG n, ULONG op, LONG mode)
{
    if (mode <= BM_NORMAL || mode >= BM_COUNT)
        span_layer_over(d, s, n, op);
    else if (mode >= BM_HUE)
        bl_nonsep(d, s, n, op, mode);
    else
        blend_fns[mode](d, s, n, op);
}
