/*
 * paint.c - Pinsel und Radiergummi
 *
 * Jeder Strich hat einen Deckungspuffer (cov) je beruehrter Kachel und
 * eine Kopie der Originalkachel. Das Ergebnis wird immer aus dem Original
 * neu berechnet (span_dab). Die Originale wandern am Ende ins Undo.
 * Kopierstempel: wie der Pinsel, die Farbe kommt aber je Pixel aus der
 * Quelle (versetzt um cl_ox/cl_oy). Gelesen wird der Zustand vor dem Strich
 * (orig_map), damit der Stempel nicht sein eigenes Ergebnis kopiert.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <math.h>
#include "apx.h"

LONG brush_size = 30, brush_hard = 50, brush_opacity = 100, brush_flow = 100;
ULONG fg_color = 0x000000, bg_color = 0xFFFFFF;

static UBYTE *bmask;
static LONG bmask_size;

static Tile **orig_map;         /* Kopie der Originalkachel je Index */
static UBYTE **cov_map;         /* Deckungspuffer je Index */
static UBYTE *touched_flag;
static LONG *touched;
static LONG ntouched, map_tiles;
static Tile zero_tile;          /* Ersatz fuer "Kachel gab es nicht" */

static BOOL s_active, s_erase, s_clone, s_alpha_lock, s_heal, s_hist;
static Tile **hb_src;            /* Protokollpinsel: Kachel im Quellzustand */
static UBYTE *hb_cur;            /* 1 = im Quellzustand wie jetzt (aktuelle Kachel nehmen) */
static LONG s_fx;               /* FX_*: Retusche-Pinsel, 0 = normal */
LONG retouch_strength = 50, retouch_mode = 0, toning_mode = 0, toning_exposure = 50;
static ULONG *fx_tmp, *fx_smudge;
static LONG fx_tmp_size;
static BOOL fx_smudge_init;
BOOL clone_aligned = TRUE;
BOOL heal_ai;                   /* Reparaturpinsel: Bereich per KI fuellen */
static BOOL cl_src_set, cl_off_set;
static float cl_sx, cl_sy;
static LONG cl_ox, cl_oy;
static ULONG cl_row[TILE_SIZE];
static ULONG s_color;            /* Farbe des Strichs (bei Maske: Grauwert) */
static Tile **s_arr;             /* Ziel: Ebenen- oder Maskenkacheln */
static float lastx, lasty, carry;

void brush_rebuild(void)
{
    LONG x, y, s = brush_size;
    float r = s * 0.5f, h = brush_hard / 100.0f;
    if (bmask)
        FreeVec(bmask);
    bmask = AllocVec(s * s, MEMF_ANY);
    bmask_size = bmask ? s : 0;
    if (!bmask)
        return;
    for (y = 0; y < s; y++)
        for (x = 0; x < s; x++) {
            float fx = x + 0.5f - r, fy = y + 0.5f - r;
            float d = sqrtf(fx * fx + fy * fy), v;
            /* Kante 1 px weich auslaufen lassen (Kantenglaettung) */
            if (h >= 0.999f || s <= 2)
                v = r + 0.5f - d;
            else {
                float hr = r * h;
                if (d <= hr)
                    v = 1.0f;
                else if (d >= r)
                    v = 0.0f;
                else {
                    float t = (d - hr) / (r - hr);
                    v = 1.0f - t * t * (3.0f - 2.0f * t);
                }
            }
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            bmask[y * s + x] = (UBYTE)(v * 255.0f + 0.5f);
        }
}

static BOOL ensure_maps(void)
{
    LONG n = doc->tw * doc->th;
    if (map_tiles == n)
        return TRUE;
    if (orig_map) FreeVec(orig_map);
    if (cov_map) FreeVec(cov_map);
    if (touched) FreeVec(touched);
    if (touched_flag) FreeVec(touched_flag);
    orig_map = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    cov_map = AllocVec(n * sizeof(UBYTE *), MEMF_ANY | MEMF_CLEAR);
    touched = AllocVec(n * sizeof(LONG), MEMF_ANY);
    touched_flag = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    map_tiles = (orig_map && cov_map && touched && touched_flag) ? n : 0;
    return map_tiles != 0;
}

/* Quellpixel im Zustand vor dem Strich */
static ULONG clone_src(LONG x, LONG y)
{
    LONG idx;
    Tile *t;
    if (x < 0 || y < 0 || x >= doc->w || y >= doc->h)
        return 0;
    idx = (y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT);
    if (s_hist && !hb_cur[idx])
        t = hb_src[idx];
    else
        t = touched_flag[idx] ? orig_map[idx] : s_arr[idx];
    return t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
}

/* wie span_dab, Farbe und Deckkraft aus src je Pixel */
static void span_dab_src(ULONG *px, const ULONG *orig, UBYTE *cov, const UBYTE *mask,
                         const UBYTE *sel, LONG n, const ULONG *src, ULONG flow, ULONG opacity)
{
    LONG i;
    flow += flow >> 7;
    opacity += opacity >> 7;
    for (i = 0; i < n; i++) {
        ULONG m = mask[i], c, sa, o, s, da, oa, inv;
        if (!m)
            continue;
        m = (m * flow) >> 8;
        c = cov[i];
        c += ((255 - c) * m * 257 + 0x8080) >> 16;
        cov[i] = (UBYTE)c;
        s = src[i];
        sa = (c * opacity) >> 8;
        sa = (sa * (s >> 24) * 257 + 0x8080) >> 16;
        if (sel)
            sa = (sa * sel[i] * 257 + 0x8080) >> 16;
        o = orig[i];
        da = ((o >> 24) * (255 - sa) * 257 + 0x8080) >> 16;
        oa = sa + da;
        if (!oa) {
            px[i] = o;
            continue;
        }
        inv = inv_tab[oa];
        px[i] = (oa << 24)
              | (((((s >> 16) & 0xFF) * sa + ((o >> 16) & 0xFF) * da) * inv >> 16) << 16)
              | (((((s >> 8) & 0xFF) * sa + ((o >> 8) & 0xFF) * da) * inv >> 16) << 8)
              | (((s & 0xFF) * sa + (o & 0xFF) * da) * inv >> 16);
    }
}

/* aktuelles Pixel (Rand fortgesetzt) */
static ULONG cur_px(LONG x, LONG y)
{
    Tile *t;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= doc->w) x = doc->w - 1;
    if (y >= doc->h) y = doc->h - 1;
    t = s_arr[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
    return t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
}

static ULONG mix_px(ULONG a, ULONG b, ULONG k)     /* k 0..256 */
{
    ULONG r = 0;
    LONG sh;
    for (sh = 0; sh < 32; sh += 8) {
        LONG ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        r |= (ULONG)(ca + (((cb - ca) * (LONG)k) >> 8)) << sh;
    }
    return r;
}

/* Retusche-Tupfer: wirkt auf die aktuellen Pixel (verstaerkt sich beim Ueberstreichen) */
static void dab_fx(float cx, float cy)
{
    LONG s = bmask_size;
    LONG bx0 = (LONG)floorf(cx - s * 0.5f + 0.5f), by0 = (LONG)floorf(cy - s * 0.5f + 0.5f);
    LONG x0 = bx0, y0 = by0, x1 = bx0 + s, y1 = by0 + s, x, y, tx, ty, w;
    ULONG str = retouch_strength * 256 / 100;
    if (!bmask)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (fx_tmp_size < s * s) {
        if (fx_tmp) FreeVec(fx_tmp);
        if (fx_smudge) FreeVec(fx_smudge);
        fx_tmp = AllocVec(s * s * 4, MEMF_ANY);
        fx_smudge = AllocVec(s * s * 4, MEMF_ANY);
        fx_tmp_size = fx_tmp && fx_smudge ? s * s : 0;
        fx_smudge_init = FALSE;
        if (!fx_tmp_size)
            return;
    }
    w = x1 - x0;
    /* Kacheln vorbereiten (Original fuers Undo sichern) */
    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx;
            if (!touched_flag[idx]) {
                Tile *t = s_arr[idx];
                orig_map[idx] = t ? tile_clone(t) : NULL;
                if (!t && !(s_arr[idx] = tile_new()))
                    continue;
                touched_flag[idx] = 1;
                touched[ntouched++] = idx;
            }
            doc_mark_dirty(tx, ty);
        }
    /* 1. Ergebnis berechnen */
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            LONG mi = (y - by0) * s + (x - bx0), oi = (y - y0) * w + (x - x0);
            ULONG m = bmask[mi], p = cur_px(x, y), out = p, k;
            if (doc->hassel)
                m = (m * doc->sel[y * doc->w + x] * 257 + 0x8080) >> 16;
            if (!m) {
                fx_tmp[oi] = p;
                continue;
            }
            switch (s_fx) {
            case FXT_BLUR:
            case FXT_SHARPEN: {
                /* 3x3-Mittel, gewichtet mit Alpha */
                ULONG sa = 0, sr = 0, sg = 0, sb = 0;
                LONG i, j;
                for (j = -1; j <= 1; j++)
                    for (i = -1; i <= 1; i++) {
                        ULONG q = cur_px(x + i, y + j), a = q >> 24;
                        sa += a;
                        sr += ((q >> 16) & 0xFF) * a;
                        sg += ((q >> 8) & 0xFF) * a;
                        sb += (q & 0xFF) * a;
                    }
                if (sa) {
                    ULONG avg = ((sa / 9) << 24) | ((sr / sa) << 16) | ((sg / sa) << 8) | (sb / sa);
                    if (s_fx == FXT_BLUR)
                        out = avg;
                    else {
                        /* schaerfen: vom Mittel weg */
                        LONG sh;
                        out = p & 0xFF000000;
                        for (sh = 0; sh < 24; sh += 8) {
                            LONG c = (p >> sh) & 0xFF, a = (avg >> sh) & 0xFF, v = c + (c - a);
                            out |= (ULONG)(v < 0 ? 0 : v > 255 ? 255 : v) << sh;
                        }
                    }
                }
                k = (m * str) >> 8;
                out = mix_px(p, out, k);
                break; }
            case FXT_SMUDGE:
                /* Farbe vom vorigen Tupfer mitziehen */
                if (!fx_smudge_init)
                    fx_smudge[mi] = p;
                k = (m * str) >> 8;
                out = mix_px(p, fx_smudge[mi], k);
                break;
            case FXT_DODGE:
            case FXT_BURN: {
                /* Mitten aufhellen / abdunkeln */
                LONG sh, e = toning_exposure * (LONG)m / 255;
                out = p & 0xFF000000;
                for (sh = 0; sh < 24; sh += 8) {
                    LONG c = (p >> sh) & 0xFF, d = c * (255 - c) / 255 * e / 600;   /* Tupfer ueberlappen ~6x */
                    c += s_fx == FXT_DODGE ? d : -d;
                    out |= (ULONG)(c < 0 ? 0 : c > 255 ? 255 : c) << sh;
                }
                break; }
            }
            fx_tmp[oi] = out;
        }
    /* 2. zurueckschreiben; Wischfinger nimmt das Ergebnis als neue Farbe mit */
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            Tile *t = s_arr[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
            ULONG v = fx_tmp[(y - y0) * w + (x - x0)];
            if (t) {
                if (s_alpha_lock)
                    v = (cur_px(x, y) & 0xFF000000) | (v & 0xFFFFFF);
                t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] = v;
                t->flag = TF_MIX;
            }
            if (s_fx == FXT_SMUDGE)
                fx_smudge[(y - by0) * s + (x - bx0)] = v;
        }
    if (s_fx == FXT_SMUDGE && !fx_smudge_init) {
        /* Teile ausserhalb des Bildes: mit dem Rand fuellen */
        for (y = 0; y < s; y++)
            for (x = 0; x < s; x++)
                if (bx0 + x < x0 || bx0 + x >= x1 || by0 + y < y0 || by0 + y >= y1)
                    fx_smudge[y * s + x] = cur_px(bx0 + x, by0 + y);
    }
    fx_smudge_init = TRUE;
}

static void dab(float cx, float cy)
{
    LONG s = bmask_size;
    LONG bx0 = (LONG)floorf(cx - s * 0.5f + 0.5f);
    LONG by0 = (LONG)floorf(cy - s * 0.5f + 0.5f);
    LONG x0 = bx0, y0 = by0, x1 = bx0 + s, y1 = by0 + s, tx, ty;
    ULONG flow = brush_flow * 255 / 100, opac = brush_opacity * 255 / 100;

    if (s_fx) {
        dab_fx(cx, cy);
        return;
    }
    if (!bmask)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx;
            LONG tx0 = tx << TILE_SHIFT, ty0 = ty << TILE_SHIFT;
            LONG ax0 = x0 > tx0 ? x0 : tx0, ay0 = y0 > ty0 ? y0 : ty0;
            LONG ax1 = x1 < tx0 + TILE_SIZE ? x1 : tx0 + TILE_SIZE;
            LONG ay1 = y1 < ty0 + TILE_SIZE ? y1 : ty0 + TILE_SIZE;
            Tile *t = s_arr[idx];
            const ULONG *orig;
            LONG y;

            if (!touched_flag[idx]) {
                if (s_erase && (!t || t->flag == TF_EMPTY))
                    continue;   /* nichts zu radieren */
                if (!(cov_map[idx] = AllocVec(TILE_PIX, MEMF_ANY | MEMF_CLEAR)))
                    continue;
                orig_map[idx] = t ? tile_clone(t) : NULL;
                if (t && !orig_map[idx]) {
                    FreeVec(cov_map[idx]);
                    cov_map[idx] = NULL;
                    continue;
                }
                if (!t) {
                    if (!(t = tile_new())) {
                        FreeVec(cov_map[idx]);
                        cov_map[idx] = NULL;
                        continue;
                    }
                    s_arr[idx] = t;
                }
                touched_flag[idx] = 1;
                touched[ntouched++] = idx;
            }
            t->flag = TF_MIX;   /* genau bestimmt am Strichende */
            orig = orig_map[idx] ? orig_map[idx]->px : zero_tile.px;

            for (y = ay0; y < ay1; y++) {
                LONG off = ((y - ty0) << TILE_SHIFT) + (ax0 - tx0);
                if (s_clone) {
                    LONG i;
                    for (i = 0; i < ax1 - ax0; i++)
                        cl_row[i] = clone_src(ax0 + i + cl_ox, y + cl_oy);
                    span_dab_src(t->px + off, orig + off, cov_map[idx] + off,
                                 bmask + (y - by0) * s + (ax0 - bx0),
                                 doc->hassel ? doc->sel + y * doc->w + ax0 : NULL,
                                 ax1 - ax0, cl_row, flow, opac);
                    continue;
                }
                span_dab(t->px + off, orig + off, cov_map[idx] + off,
                         bmask + (y - by0) * s + (ax0 - bx0),
                         doc->hassel ? doc->sel + y * doc->w + ax0 : NULL,
                         ax1 - ax0, s_color, flow, opac, s_erase);
                if (s_alpha_lock) {
                    LONG k;
                    for (k = 0; k < ax1 - ax0; k++)
                        t->px[off + k] = (orig[off + k] & 0xFF000000) | (t->px[off + k] & 0xFFFFFF);
                }
            }
            doc_mark_dirty(tx, ty);
        }
}

static void stroke_start(float x, float y, BOOL erase, BOOL clone)
{
    if (!ensure_maps() || !bmask)
        return;
    s_active = TRUE;
    s_clone = clone;
    if (!clone)
        s_hist = FALSE;
    s_alpha_lock = (doc->layers[doc->active]->lock & LOCK_ALPHA) && !doc_target_is_mask();
    fx_smudge_init = FALSE;
    s_erase = erase;
    s_color = fg_color;
    if (doc_target_is_mask()) {
        /* in der Maske: Grau malen; Radiergummi malt mit der Hintergrundfarbe */
        s_color = mask_gray(erase ? bg_color : fg_color) * 0x010101;
        s_erase = FALSE;
    } else
        doc_rasterize_text(doc->layers[doc->active]);
    if (s_heal)                 /* Reparatur: Bereich dunkel bzw. (KI) rosa markieren */
        s_color = heal_ai ? 0xE8508C : 0x000000;
    s_arr = doc_target();
    ntouched = 0;
    lastx = x;
    lasty = y;
    carry = 0.0f;
    dab(x, y);
}

void stroke_begin(float x, float y, BOOL erase)
{
    s_fx = 0;
    s_heal = FALSE;
    stroke_start(x, y, erase, FALSE);
}

/* ---- Protokollpinsel ---- */

/* Quellzustand: Kacheln der Zielebene nach Protokolleintrag src (0 = Ausgangszustand) */
static BOOL hist_prepare(LONG src)
{
    LONG n = doc->tw * doc->th, i, vis = 0;
    Layer *l = doc->layers[doc->active];
    BOOL mask = doc_target_is_mask();
    UndoRec *r, *start = NULL;
    if (hb_src) FreeVec(hb_src);
    if (hb_cur) FreeVec(hb_cur);
    hb_src = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    hb_cur = AllocVec(n, MEMF_ANY);
    if (!hb_src || !hb_cur)
        return FALSE;
    memset(hb_cur, 1, n);
    /* erster Eintrag nach dem Quellzustand (innerhalb des Angewendeten) */
    if (!doc->ucur)
        return TRUE;            /* nichts angewendet: Quelle = jetzt */
    for (r = doc->uhead; r; r = r->next) {
        if (!r->chain)
            vis++;
        if (vis > src) {
            start = r;
            break;
        }
        if (r == doc->ucur)
            return TRUE;        /* Quelle liegt nicht vor dem jetzigen Zustand */
    }
    /* vom Quellzustand vorwaerts: der erste Eintrag, der eine Kachel aenderte,
     * haelt deren Inhalt im Quellzustand */
    for (r = start; r; r = r->next) {
        if (r->layer == l && !r->on_mask == !mask)
            for (i = 0; i < r->n; i++) {
                LONG idx = r->idx[i];
                if (hb_cur[idx]) {
                    hb_cur[idx] = 0;
                    hb_src[idx] = r->tiles[i];
                }
            }
        if (r == doc->ucur)
            break;
    }
    return TRUE;
}

void history_brush_begin(float x, float y)
{
    s_fx = 0;
    s_heal = FALSE;
    if (!hist_prepare(doc->hist_src))
        return;
    s_hist = TRUE;
    cl_ox = cl_oy = 0;
    stroke_start(x, y, FALSE, TRUE);
}

/* ---- Bereichsreparatur (Spot Healing) ---- */

void heal_begin(float x, float y)
{
    s_fx = 0;
    s_heal = TRUE;
    stroke_start(x, y, FALSE, FALSE);
}

static ULONG heal_get(LONG x, LONG y)
{
    Tile *t;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= doc->w) x = doc->w - 1;
    if (y >= doc->h) y = doc->h - 1;
    t = s_arr[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
    return t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
}

static UBYTE heal_cov(LONG x, LONG y)
{
    LONG idx;
    if (x < 0 || y < 0 || x >= doc->w || y >= doc->h)
        return 0;
    idx = (y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT);
    return touched_flag[idx] && cov_map[idx] ? cov_map[idx][((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
}

/* Am Strichende: Vorschau zuruecknehmen, passende Textur aus der Umgebung holen
 * und Farbe/Helligkeit zum Rand hin angleichen (Randdifferenz als Membran) */
static void heal_apply(void)
{
    LONG i, x, y, bx0 = doc->w, by0 = doc->h, bx1 = -1, by1 = -1, m = 3;
    LONG ex0, ey0, ex1, ey1, ew, eh, best_ox = 0, best_oy = 0, k, it, iters;
    float best = 1e30f, *dd;
    ULONG *src;
    UBYTE *cv;
    /* 1. Vorschau zuruecknehmen */
    for (i = 0; i < ntouched; i++) {
        LONG idx = touched[i];
        Tile *t = s_arr[idx];
        if (!t)
            continue;
        if (orig_map[idx])
            CopyMem(orig_map[idx]->px, t->px, sizeof(t->px));
        else
            memset(t->px, 0, sizeof(t->px));
    }
    /* 2. Umriss des Bereichs */
    for (i = 0; i < ntouched; i++) {
        LONG idx = touched[i], ox = (idx % doc->tw) << TILE_SHIFT, oy = (idx / doc->tw) << TILE_SHIFT;
        if (!cov_map[idx])
            continue;
        for (y = 0; y < TILE_SIZE; y++)
            for (x = 0; x < TILE_SIZE; x++)
                if (cov_map[idx][(y << TILE_SHIFT) | x]) {
                    if (ox + x < bx0) bx0 = ox + x;
                    if (ox + x > bx1) bx1 = ox + x;
                    if (oy + y < by0) by0 = oy + y;
                    if (oy + y > by1) by1 = oy + y;
                }
    }
    if (bx1 < bx0)
        return;
    ex0 = bx0 - m; ey0 = by0 - m; ex1 = bx1 + m + 1; ey1 = by1 + m + 1;
    if (ex0 < 0) ex0 = 0;
    if (ey0 < 0) ey0 = 0;
    if (ex1 > doc->w) ex1 = doc->w;
    if (ey1 > doc->h) ey1 = doc->h;
    ew = ex1 - ex0;
    eh = ey1 - ey0;
    /* 3. beste Quelle in der Naehe: Rand (Ring) moeglichst aehnlich */
    {
        LONG sz = (bx1 - bx0 > by1 - by0 ? bx1 - bx0 : by1 - by0) + 6, dist;
        static const BYTE dirs[8][2] = { {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {-1,1}, {1,-1}, {-1,-1} };
        for (dist = sz; dist <= sz * 2; dist += sz / 2 + 1)
            for (k = 0; k < 8; k++) {
                LONG ox = dirs[k][0] * dist, oy = dirs[k][1] * dist;
                float ssd = 0.0f;
                LONG cnt = 0;
                if (ex0 + ox < 0 || ey0 + oy < 0 || ex1 + ox > doc->w || ey1 + oy > doc->h)
                    continue;
                for (y = ey0; y < ey1; y++)
                    for (x = ex0; x < ex1; x++) {
                        ULONG a, b;
                        LONG sh;
                        if (heal_cov(x, y))
                            continue;
                        a = heal_get(x, y);
                        b = heal_get(x + ox, y + oy);
                        for (sh = 0; sh < 32; sh += 8) {
                            LONG d = (LONG)((a >> sh) & 0xFF) - (LONG)((b >> sh) & 0xFF);
                            ssd += (float)(d * d);
                        }
                        cnt++;
                    }
                /* auch die Quelle selbst darf nicht im Bereich liegen */
                for (y = ey0; y < ey1 && ssd < best; y += 2)
                    for (x = ex0; x < ex1; x += 2)
                        if (heal_cov(x + ox, y + oy)) {
                            ssd = 1e30f;
                            break;
                        }
                if (cnt && ssd / cnt < best) {
                    best = ssd / cnt;
                    best_ox = ox;
                    best_oy = oy;
                }
            }
    }
    if (best >= 1e29f)
        return;                 /* keine Quelle gefunden */
    /* 4. Textur holen, Randdifferenz innen glaetten (Membran) */
    src = AllocVec(ew * eh * 4, MEMF_ANY);
    cv = AllocVec(ew * eh, MEMF_ANY);
    dd = AllocVec(ew * eh * 4 * sizeof(float), MEMF_ANY);
    if (!src || !cv || !dd)
        goto out;
    for (y = 0; y < eh; y++)
        for (x = 0; x < ew; x++) {
            LONG o = y * ew + x;
            ULONG a = heal_get(ex0 + x, ey0 + y), b = heal_get(ex0 + x + best_ox, ey0 + y + best_oy);
            src[o] = b;
            cv[o] = heal_cov(ex0 + x, ey0 + y);
            for (k = 0; k < 4; k++) {
                LONG sh = k * 8;
                dd[o * 4 + k] = cv[o] ? 0.0f : (float)((LONG)((a >> sh) & 0xFF) - (LONG)((b >> sh) & 0xFF));
            }
        }
    iters = (ew > eh ? ew : eh) * 2;
    if (iters > 400) iters = 400;
    for (it = 0; it < iters; it++)
        for (y = 0; y < eh; y++)
            for (x = 0; x < ew; x++) {
                LONG o = y * ew + x;
                LONG l = x > 0 ? o - 1 : o, r = x < ew - 1 ? o + 1 : o;
                LONG u = y > 0 ? o - ew : o, d = y < eh - 1 ? o + ew : o;
                if (!cv[o])
                    continue;
                for (k = 0; k < 4; k++) {
                    float v = (dd[l * 4 + k] + dd[r * 4 + k] + dd[u * 4 + k] + dd[d * 4 + k]) * 0.25f;
                    /* Ueberrelaxation beschleunigt die Konvergenz */
                    dd[o * 4 + k] += (v - dd[o * 4 + k]) * 1.8f;
                }
            }
    /* 5. Ergebnis mit der Deckung einblenden */
    for (y = 0; y < eh; y++)
        for (x = 0; x < ew; x++) {
            LONG o = y * ew + x, gx = ex0 + x, gy = ey0 + y;
            Tile *t;
            ULONG a, out = 0;
            if (!cv[o])
                continue;
            t = s_arr[(gy >> TILE_SHIFT) * doc->tw + (gx >> TILE_SHIFT)];
            if (!t)
                continue;
            a = t->px[((gy & TILE_MASK) << TILE_SHIFT) | (gx & TILE_MASK)];
            for (k = 0; k < 4; k++) {
                LONG sh = k * 8, h = (LONG)(((src[o] >> sh) & 0xFF) + dd[o * 4 + k] + 0.5f), c = (a >> sh) & 0xFF;
                if (h < 0) h = 0;
                if (h > 255) h = 255;
                c += (h - c) * (LONG)cv[o] / 255;
                out |= (ULONG)c << sh;
            }
            t->px[((gy & TILE_MASK) << TILE_SHIFT) | (gx & TILE_MASK)] = out;
        }
out:
    if (src) FreeVec(src);
    if (cv) FreeVec(cv);
    if (dd) FreeVec(dd);
}

/* Retusche-Pinsel: Weichzeichner, Scharfzeichner, Wischfinger, Abwedler, Nachbelichter */
void stroke_begin_fx(float x, float y, LONG fx)
{
    s_fx = fx;
    stroke_start(x, y, FALSE, FALSE);
}

/* ---- Kopierstempel ---- */

void clone_set_source(float x, float y)
{
    cl_sx = x;
    cl_sy = y;
    cl_src_set = TRUE;
    cl_off_set = FALSE;
}

BOOL clone_has_source(void)
{
    return cl_src_set;
}

/* Ausgerichtet: Versatz bleibt ab dem ersten Strich fest; sonst beginnt
 * jeder Strich wieder am Quellpunkt */
void clone_begin(float x, float y)
{
    s_fx = 0;
    s_heal = FALSE;
    s_hist = FALSE;
    if (!cl_off_set || !clone_aligned) {
        cl_ox = (LONG)floorf(cl_sx - x + 0.5f);
        cl_oy = (LONG)floorf(cl_sy - y + 0.5f);
        cl_off_set = TRUE;
    }
    stroke_start(x, y, FALSE, TRUE);
}

void stroke_to(float x, float y)
{
    float dx, dy, dist, spacing, t;
    if (!s_active)
        return;
    dx = x - lastx;
    dy = y - lasty;
    dist = sqrtf(dx * dx + dy * dy);
    spacing = brush_size * 0.15f;
    if (spacing < 1.0f)
        spacing = 1.0f;
    t = spacing - carry;
    while (t <= dist) {
        float f = t / dist;
        dab(lastx + dx * f, lasty + dy * f);
        t += spacing;
    }
    carry = dist - (t - spacing);
    lastx = x;
    lasty = y;
}

/* KI-Reparatur: Vorschau zuruecknehmen, Strich als Maske an die KI geben */
static void heal_ai_end(void)
{
    UBYTE *m = AllocVec(doc->w * doc->h, MEMF_ANY | MEMF_CLEAR);
    LONG i, x, y;
    for (i = 0; i < ntouched; i++) {
        LONG idx = touched[i], ox = (idx % doc->tw) << TILE_SHIFT, oy = (idx / doc->tw) << TILE_SHIFT;
        Tile *t = s_arr[idx];
        if (orig_map[idx]) {
            CopyMem(orig_map[idx]->px, t->px, sizeof(t->px));
            FreeVec(orig_map[idx]);
            orig_map[idx] = NULL;
        } else if (t) {
            FreeVec(t);         /* Kachel entstand erst durch den Strich */
            s_arr[idx] = NULL;
        }
        if (m && cov_map[idx])
            for (y = 0; y < TILE_SIZE && oy + y < doc->h; y++)
                for (x = 0; x < TILE_SIZE && ox + x < doc->w; x++)
                    m[(oy + y) * doc->w + ox + x] = cov_map[idx][(y << TILE_SHIFT) | x];
        FreeVec(cov_map[idx]);
        cov_map[idx] = NULL;
        touched_flag[idx] = 0;
        doc_mark_dirty(idx % doc->tw, idx / doc->tw);
    }
    ntouched = 0;
    if (m) {
        ai_start_mask(m);
        FreeVec(m);
    } else {
        status_message("Not enough memory");
    }
}

void stroke_end(void)
{
    UndoRec *u;
    LONG i;
    if (!s_active)
        return;
    s_active = FALSE;
    if (s_heal && heal_ai) {
        s_heal = FALSE;
        heal_ai_end();
        return;
    }
    if (s_heal)
        heal_apply();
    {
        static const char *fxn[6] = { "", "Blur Tool", "Sharpen Tool", "Smudge Tool", "Dodge Tool", "Burn Tool" };
        u = undo_begin_target(s_heal ? "Spot Healing Brush" : s_hist ? "History Brush" : s_fx ? fxn[s_fx] :
                              s_clone ? "Clone Stamp" : s_erase ? "Eraser" : "Brush Tool");
        s_hist = FALSE;
    }
    for (i = 0; i < ntouched; i++) {
        LONG idx = touched[i];
        Tile *t = s_arr[idx];
        undo_add(u, idx, orig_map[idx]);
        orig_map[idx] = NULL;
        FreeVec(cov_map[idx]);
        cov_map[idx] = NULL;
        touched_flag[idx] = 0;
        tile_update_flag(doc, t, idx % doc->tw, idx / doc->tw);
        doc_mark_dirty(idx % doc->tw, idx / doc->tw);
    }
    ntouched = 0;
    undo_commit(u);
}
