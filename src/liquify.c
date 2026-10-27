/*
 * liquify.c - Filter > Liquify (Verfluessigen) direkt auf der Arbeitsflaeche
 *
 * Verschiebungsfeld d je Pixel: Ergebnis(p) = Quelle(p - d(p)), bilinear,
 * vormultipliziert. Werkzeuge veraendern d nur im Pinselbereich, gerendert
 * wird nur dort. Ziel ist die aktive Ebene oder ihre Maske (doc_target).
 * Die Original-Kacheln werden beim ersten Anfassen gesichert: Abbrechen legt
 * sie zurueck, Anwenden gibt sie als ein Undo-Schritt ans Protokoll.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <math.h>
#include "apx.h"

LONG lq_tool, lq_size = 120, lq_pressure = 50;

static BOOL lq_on;
static Tile **lq_arr;           /* Ziel (Ebene oder Maske) */
static Tile **lq_orig;          /* Sicherung beim ersten Anfassen */
static UBYTE *lq_touched;
static ULONG *lq_src;           /* Ausgangsbild, vormultipliziert, w*h */
static float *lq_d;             /* Verschiebung (dx, dy) je Pixel */
static float *lq_tmp;           /* Kopie eines Bereichs von d (Forward Warp) */
static LONG lq_tmpn;
static float lq_lx, lq_ly;      /* letzte Pinsellage (Dokument) */
static BOOL lq_down;

BOOL liquify_active(void)
{
    return lq_on;
}

static ULONG premul(ULONG p)
{
    ULONG a = p >> 24, a2 = a + (a >> 7);
    if (a == 255)
        return p;
    return (a << 24) | ((((p & 0xFF00FF) * a2) >> 8) & 0xFF00FF) | ((((p & 0x00FF00) * a2) >> 8) & 0x00FF00);
}

static ULONG unpremul(ULONG a, ULONG r, ULONG g, ULONG b)
{
    if (!a)
        return 0;
    if (a < 255) {
        r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void free_all(void)
{
    if (lq_src) FreeVec(lq_src);
    if (lq_d) FreeVec(lq_d);
    if (lq_tmp) FreeVec(lq_tmp);
    if (lq_orig) FreeVec(lq_orig);
    if (lq_touched) FreeVec(lq_touched);
    lq_src = NULL;
    lq_d = lq_tmp = NULL;
    lq_tmpn = 0;
    lq_orig = NULL;
    lq_touched = NULL;
    lq_on = FALSE;
    lq_down = FALSE;
}

BOOL liquify_begin(void)
{
    LONG w = doc->w, h = doc->h, n = doc->tw * doc->th, x, y;
    if (lq_on)
        return TRUE;
    if (!doc_target_is_mask())
        doc_rasterize_text(doc->layers[doc->active]);
    lq_arr = doc_target();
    lq_src = AllocVec(w * h * 4, MEMF_ANY);
    lq_d = AllocVec(w * h * 2 * sizeof(float), MEMF_ANY | MEMF_CLEAR);
    lq_orig = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    lq_touched = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    if (!lq_src || !lq_d || !lq_orig || !lq_touched) {
        free_all();
        status_message("Not enough memory for Liquify");
        return FALSE;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            Tile *t = lq_arr[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
            lq_src[y * w + x] = t ? premul(t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)]) : 0;
        }
    lq_on = TRUE;
    doc_cache_below(doc->active);
    return TRUE;
}

/* Quelle bilinear an (u, v), vormultipliziert; ch in 1/65536 */
static void sample(float u, float v, ULONG *ch)
{
    LONG iu, iv, k, w = doc->w, h = doc->h;
    ULONG wu, wv, wt[4];
    u -= 0.5f;
    v -= 0.5f;
    iu = (LONG)floorf(u);
    iv = (LONG)floorf(v);
    /* ganzzahlige Gewichte, Summe genau 65536 */
    wu = (ULONG)((u - iu) * 256.0f);
    wv = (ULONG)((v - iv) * 256.0f);
    if (wu > 256) wu = 256;
    if (wv > 256) wv = 256;
    wt[0] = (256 - wu) * (256 - wv);
    wt[1] = wu * (256 - wv);
    wt[2] = (256 - wu) * wv;
    wt[3] = wu * wv;
    ch[0] = ch[1] = ch[2] = ch[3] = 0;
    for (k = 0; k < 4; k++) {
        LONG sx = iu + (k & 1), sy = iv + (k >> 1);
        ULONG p;
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx >= w) sx = w - 1;
        if (sy >= h) sy = h - 1;
        p = lq_src[sy * w + sx];
        ch[0] += (p >> 24) * wt[k];
        ch[1] += ((p >> 16) & 0xFF) * wt[k];
        ch[2] += ((p >> 8) & 0xFF) * wt[k];
        ch[3] += (p & 0xFF) * wt[k];
    }
}

/* Bereich neu rendern */
static void render_rect(LONG x0, LONG y0, LONG x1, LONG y1)
{
    LONG tx, ty, w = doc->w;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx, gx0 = tx << TILE_SHIFT, gy0 = ty << TILE_SHIFT, x, y;
            LONG ax0 = x0 > gx0 ? x0 : gx0, ay0 = y0 > gy0 ? y0 : gy0;
            LONG ax1 = x1 < gx0 + TILE_SIZE ? x1 : gx0 + TILE_SIZE, ay1 = y1 < gy0 + TILE_SIZE ? y1 : gy0 + TILE_SIZE;
            Tile *t = lq_arr[idx];
            if (!lq_touched[idx]) {
                lq_orig[idx] = t;       /* Original sichern, ab jetzt eigene Kopie */
                lq_touched[idx] = 1;
                t = t ? tile_clone(t) : tile_new();
                if (!t) {
                    lq_touched[idx] = 0;
                    continue;
                }
                lq_arr[idx] = t;
            } else if (!t) {
                if (!(t = tile_new()))
                    continue;
                lq_arr[idx] = t;
            }
            for (y = ay0; y < ay1; y++) {
                ULONG *row = t->px + ((y & TILE_MASK) << TILE_SHIFT);
                const float *d = lq_d + (y * w + ax0) * 2;
                for (x = ax0; x < ax1; x++, d += 2) {
                    ULONG ch[4];
                    if (d[0] == 0.0f && d[1] == 0.0f) {
                        ULONG p = lq_src[y * w + x];
                        row[x & TILE_MASK] = unpremul(p >> 24, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
                        continue;
                    }
                    sample(x + 0.5f - d[0], y + 0.5f - d[1], ch);
                    {
                        ULONG aa = (ch[0] + 32768) >> 16, r = (ch[1] + 32768) >> 16;
                        ULONG g = (ch[2] + 32768) >> 16, b = (ch[3] + 32768) >> 16;
                        if (aa > 255) aa = 255;
                        if (r > aa) r = aa;
                        if (g > aa) g = aa;
                        if (b > aa) b = aa;
                        row[x & TILE_MASK] = unpremul(aa, r, g, b);
                    }
                }
            }
            tile_update_flag(doc, t, tx, ty);
            doc_mark_dirty(tx, ty);
        }
}

/* ein Tupfer an (cx, cy); Bewegung (mx, my) seit dem letzten (Forward Warp) */
static void dab(float cx, float cy, float mx, float my)
{
    float r = lq_size * 0.5f, pr = lq_pressure / 100.0f;
    LONG x0 = (LONG)floorf(cx - r), y0 = (LONG)floorf(cy - r), x1 = (LONG)ceilf(cx + r) + 1;
    LONG y1 = (LONG)ceilf(cy + r) + 1, x, y, w = doc->w, bw, bh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    bw = x1 - x0;
    bh = y1 - y0;
    if (lq_tool == 0) {
        /* Forward Warp braucht das alte Feld im (verschobenen) Bereich */
        LONG mx0 = x0 - (LONG)fabsf(mx) - 2, my0 = y0 - (LONG)fabsf(my) - 2;
        LONG mx1 = x1 + (LONG)fabsf(mx) + 2, my1 = y1 + (LONG)fabsf(my) + 2, need;
        if (mx0 < 0) mx0 = 0;
        if (my0 < 0) my0 = 0;
        if (mx1 > doc->w) mx1 = doc->w;
        if (my1 > doc->h) my1 = doc->h;
        need = (mx1 - mx0) * (my1 - my0) * 2;
        if (need > lq_tmpn) {
            if (lq_tmp)
                FreeVec(lq_tmp);
            lq_tmp = AllocVec(need * sizeof(float), MEMF_ANY);
            lq_tmpn = lq_tmp ? need : 0;
            if (!lq_tmp)
                return;
        }
        for (y = my0; y < my1; y++)
            CopyMem(lq_d + (y * w + mx0) * 2, lq_tmp + ((y - my0) * (mx1 - mx0)) * 2, (mx1 - mx0) * 2 * sizeof(float));
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, q = (dx * dx + dy * dy) / (r * r), wt;
                LONG sx, sy;
                float *d = lq_d + (y * w + x) * 2, ox, oy;
                if (q >= 1.0f)
                    continue;
                wt = (1.0f - q) * (1.0f - q) * pr;
                /* Inhalt mit der Maus mitnehmen: d_neu(p) = d_alt(p - m) + m */
                sx = (LONG)floorf(x - mx * wt + 0.5f);
                sy = (LONG)floorf(y - my * wt + 0.5f);
                if (sx < mx0) sx = mx0;
                if (sy < my0) sy = my0;
                if (sx >= mx1) sx = mx1 - 1;
                if (sy >= my1) sy = my1 - 1;
                ox = lq_tmp[((sy - my0) * (mx1 - mx0) + (sx - mx0)) * 2];
                oy = lq_tmp[((sy - my0) * (mx1 - mx0) + (sx - mx0)) * 2 + 1];
                d[0] = ox + mx * wt;
                d[1] = oy + my * wt;
            }
    } else
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, q = (dx * dx + dy * dy) / (r * r), wt;
                float *d = lq_d + (y * w + x) * 2;
                if (q >= 1.0f)
                    continue;
                wt = (1.0f - q) * (1.0f - q) * pr;
                switch (lq_tool) {
                case 1:             /* Reconstruct */
                    d[0] *= 1.0f - wt * 0.3f;
                    d[1] *= 1.0f - wt * 0.3f;
                    break;
                case 2:             /* Pucker: von weiter aussen holen */
                    d[0] -= dx * wt * 0.06f;
                    d[1] -= dy * wt * 0.06f;
                    break;
                case 3:             /* Bloat: von weiter innen holen */
                    d[0] += dx * wt * 0.06f;
                    d[1] += dy * wt * 0.06f;
                    break;
                case 4: {           /* Twirl im Uhrzeigersinn */
                    float a = 0.08f * wt, c = cosf(a), s = sinf(a);
                    d[0] += dx - (c * dx + s * dy);
                    d[1] += dy - (-s * dx + c * dy);
                    break; }
                }
            }
    render_rect(x0, y0, x0 + bw, y0 + bh);
}

void liquify_mouse_down(float x, float y)
{
    if (!lq_on)
        return;
    lq_down = TRUE;
    lq_lx = x;
    lq_ly = y;
    if (lq_tool != 0)
        dab(x, y, 0, 0);
}

void liquify_mouse_move(float x, float y)
{
    float mx, my, dist, step;
    LONG n, k;
    if (!lq_on || !lq_down)
        return;
    mx = x - lq_lx;
    my = y - lq_ly;
    dist = sqrtf(mx * mx + my * my);
    step = lq_size * 0.15f;
    if (step < 2.0f)
        step = 2.0f;
    n = (LONG)(dist / step) + 1;
    for (k = 1; k <= n; k++) {
        float cx = lq_lx + mx * k / n, cy = lq_ly + my * k / n;
        dab(cx, cy, lq_tool == 0 ? mx / n : 0, lq_tool == 0 ? my / n : 0);
    }
    lq_lx = x;
    lq_ly = y;
}

/* gehaltene Maus: Pucker/Bloat/Twirl/Reconstruct wirken weiter */
void liquify_tick(void)
{
    if (lq_on && lq_down && lq_tool != 0)
        dab(lq_lx, lq_ly, 0, 0);
}

void liquify_mouse_up(void)
{
    lq_down = FALSE;
}

void liquify_apply(void)
{
    UndoRec *u;
    LONG i, n = doc->tw * doc->th;
    BOOL any = FALSE;
    if (!lq_on)
        return;
    doc_cache_drop();
    for (i = 0; i < n; i++)
        any |= lq_touched[i];
    if (any) {
        u = undo_begin_target("Liquify");
        for (i = 0; i < n; i++)
            if (lq_touched[i]) {
                Tile *t = lq_arr[i];
                if (t) {
                    tile_update_flag(doc, t, i % doc->tw, i / doc->tw);
                    if (t->flag == TF_EMPTY && !doc_target_is_mask()) {
                        FreeVec(t);
                        lq_arr[i] = NULL;
                    }
                }
                undo_add(u, i, lq_orig[i]);     /* Original gehoert jetzt dem Undo */
                lq_orig[i] = NULL;
            }
        undo_commit(u);
    }
    free_all();
}

void liquify_cancel(void)
{
    LONG i, n = doc->tw * doc->th;
    if (!lq_on)
        return;
    doc_cache_drop();
    for (i = 0; i < n; i++)
        if (lq_touched[i]) {
            if (lq_arr[i])
                FreeVec(lq_arr[i]);
            lq_arr[i] = lq_orig[i];
            doc_mark_dirty(i % doc->tw, i / doc->tw);
        }
    free_all();
}
