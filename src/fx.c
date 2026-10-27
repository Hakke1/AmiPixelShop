/*
 * fx.c - Ebenenstile: Schlagschatten, Schein nach aussen, Kontur
 *
 * Nicht-destruktiv: Aus dem Alpha der Ebene (mit Maske) entstehen je Effekt
 * Kacheln in l->fxc[] (ARGB straight, eine Farbe, Alpha = Effekt). Beim
 * Zusammensetzen liegen sie unter der Ebene (Schatten: Multiplizieren,
 * Schein: Negativ multiplizieren, Kontur: Normal).
 * Neu gerechnet wird nur um geaenderte Kacheln herum (Umriss der
 * schmutzigen Kacheln plus Effektradius); fx_valid = FALSE erzwingt alles.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "apx.h"

BOOL fx_active(const LayerFx *f)
{
    return f && (f->sh_on || f->gl_on || f->st_on);
}

static LONG fx_radius(const LayerFx *f)
{
    LONG r = 0;
    if (f->sh_on)
        r = f->sh_dist + f->sh_size;
    if (f->gl_on && f->gl_size > r)
        r = f->gl_size;
    if (f->st_on && f->st_size > r)
        r = f->st_size;
    return r + 2;
}

void fx_free_cache(Layer *l, Doc *d)
{
    LONG k, i;
    for (k = 0; k < FX_COUNT; k++)
        if (l->fxc[k]) {
            for (i = 0; i < d->tw * d->th; i++)
                if (l->fxc[k][i])
                    FreeVec(l->fxc[k][i]);
            FreeVec(l->fxc[k]);
            l->fxc[k] = NULL;
        }
    l->fx_valid = FALSE;
}

/* Effekt-Alpha (Puffer bx0/by0, Breite bw) in die Kacheln tx0..tx1/ty0..ty1 */
static void store(Tile **arr, const UBYTE *buf, LONG bx0, LONG by0, LONG bw,
                  LONG tx0, LONG ty0, LONG tx1, LONG ty1, ULONG color, LONG opacity, BOOL on)
{
    LONG tx, ty, x, y;
    ULONG op = opacity * 255 / 100;
    op += op >> 7;
    color &= 0xFFFFFF;
    for (ty = ty0; ty <= ty1; ty++)
        for (tx = tx0; tx <= tx1; tx++) {
            LONG idx = ty * doc->tw + tx, ox = tx << TILE_SHIFT, oy = ty << TILE_SHIFT;
            LONG vw = doc->w - ox, vh = doc->h - oy;
            Tile *t = arr[idx];
            ULONG any = 0;
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            if (on) {
                for (y = 0; y < vh && !any; y++) {
                    const UBYTE *b = buf + (oy + y - by0) * bw + (ox - bx0);
                    for (x = 0; x < vw; x++)
                        any |= b[x];
                }
                any = any && op;
            }
            if (!any) {
                if (t) {
                    FreeVec(t);
                    arr[idx] = NULL;
                }
                continue;
            }
            if (!t && !(t = arr[idx] = tile_new()))
                continue;
            for (y = 0; y < vh; y++) {
                const UBYTE *b = buf + (oy + y - by0) * bw + (ox - bx0);
                ULONG *p = t->px + (y << TILE_SHIFT);
                for (x = 0; x < vw; x++)
                    p[x] = (((b[x] * op) >> 8) << 24) | color;
            }
            tile_update_flag(doc, t, tx, ty);
        }
}

/* eine Stufe Dilatation: 3x3-Quadrat oder Kreuz (abwechselnd ~ Achteck) */
static BOOL dilate1(const UBYTE *s, UBYTE *d, LONG w, LONG h, BOOL square)
{
    LONG x, y;
    BOOL ch = FALSE;
    for (y = 0; y < h; y++) {
        const UBYTE *r = s + y * w, *u = y > 0 ? r - w : r, *dn = y < h - 1 ? r + w : r;
        UBYTE *o = d + y * w;
        for (x = 0; x < w; x++) {
            LONG xl = x > 0 ? x - 1 : x, xr = x < w - 1 ? x + 1 : x;
            UBYTE m = r[x];
            if (r[xl] > m) m = r[xl];
            if (r[xr] > m) m = r[xr];
            if (u[x] > m) m = u[x];
            if (dn[x] > m) m = dn[x];
            if (square) {
                if (u[xl] > m) m = u[xl];
                if (u[xr] > m) m = u[xr];
                if (dn[xl] > m) m = dn[xl];
                if (dn[xr] > m) m = dn[xr];
            }
            ch |= m != r[x];
            o[x] = m;
        }
    }
    return ch;
}

static void blur3(UBYTE *b, LONG w, LONG h, LONG size)
{
    LONG r = (size + 2) / 3, k;
    if (size < 1)
        return;
    for (k = 0; k < 3; k++)
        mask_blur(b, w, 0, 0, w, h, r);
}

/* Effekte der Ebene fuer die Kacheln tx0..tx1/ty0..ty1 neu rechnen */
static void fx_update(Layer *l, LONG tx0, LONG ty0, LONG tx1, LONG ty1)
{
    LayerFx *f = l->fx;
    LONG R = fx_radius(f), n = doc->tw * doc->th, k, x, y;
    LONG X0 = tx0 << TILE_SHIFT, Y0 = ty0 << TILE_SHIFT;
    LONG X1 = (tx1 + 1) << TILE_SHIFT, Y1 = (ty1 + 1) << TILE_SHIFT;
    LONG bx0, by0, bx1, by1, bw, bh;
    UBYTE *a, *t1, *t2;

    LONG cx0 = doc->tw, cy0 = doc->th, cx1 = -1, cy1 = -1, rt, tx, ty;

    for (k = 0; k < FX_COUNT; k++)
        if (!l->fxc[k] && !(l->fxc[k] = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR)))
            return;
    /* nur um den Inhalt der Ebene herum rechnen; ausserhalb ist alles leer */
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            Tile *t = l->tiles[ty * doc->tw + tx];
            if (t && t->flag != TF_EMPTY) {
                if (tx < cx0) cx0 = tx;
                if (tx > cx1) cx1 = tx;
                if (ty < cy0) cy0 = ty;
                if (ty > cy1) cy1 = ty;
            }
        }
    rt = (R + TILE_SIZE - 1) >> TILE_SHIFT;
    cx0 -= rt; cy0 -= rt; cx1 += rt; cy1 += rt;
    for (ty = ty0; ty <= ty1; ty++)
        for (tx = tx0; tx <= tx1; tx++)
            if (tx < cx0 || tx > cx1 || ty < cy0 || ty > cy1)
                for (k = 0; k < FX_COUNT; k++)
                    if (l->fxc[k][ty * doc->tw + tx]) {
                        FreeVec(l->fxc[k][ty * doc->tw + tx]);
                        l->fxc[k][ty * doc->tw + tx] = NULL;
                    }
    if (tx0 < cx0) tx0 = cx0;
    if (ty0 < cy0) ty0 = cy0;
    if (tx1 > cx1) tx1 = cx1;
    if (ty1 > cy1) ty1 = cy1;
    if (tx0 > tx1 || ty0 > ty1)
        return;
    X0 = tx0 << TILE_SHIFT;
    Y0 = ty0 << TILE_SHIFT;
    X1 = (tx1 + 1) << TILE_SHIFT;
    Y1 = (ty1 + 1) << TILE_SHIFT;
    if (X1 > doc->w) X1 = doc->w;
    if (Y1 > doc->h) Y1 = doc->h;
    bx0 = X0 - R > 0 ? X0 - R : 0;
    by0 = Y0 - R > 0 ? Y0 - R : 0;
    bx1 = X1 + R < doc->w ? X1 + R : doc->w;
    by1 = Y1 + R < doc->h ? Y1 + R : doc->h;
    bw = bx1 - bx0;
    bh = by1 - by0;
    a = AllocVec(bw * bh, MEMF_ANY);
    t1 = AllocVec(bw * bh, MEMF_ANY);
    t2 = AllocVec(bw * bh, MEMF_ANY);
    if (!a || !t1 || !t2)
        goto out;

    /* Alpha der Ebene, mit Maske */
    for (y = by0; y < by1; y++) {
        UBYTE *d = a + (y - by0) * bw;
        for (x = bx0; x < bx1; x++) {
            LONG idx = (y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT);
            LONG o = ((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK);
            Tile *t = l->tiles[idx];
            ULONG v = t ? t->px[o] >> 24 : 0;
            if (v && l->mask && l->mask_on && l->mask[idx])
                v = (v * (l->mask[idx]->px[o] & 0xFF) * 257 + 0x8080) >> 16;
            d[x - bx0] = (UBYTE)v;
        }
    }

    /* Schlagschatten: versetzt und weichgezeichnet */
    if (f->sh_on) {
        float ang = f->sh_angle * (3.14159265f / 180.0f);
        LONG dx = (LONG)floorf(-cosf(ang) * f->sh_dist + 0.5f);
        LONG dy = (LONG)floorf(sinf(ang) * f->sh_dist + 0.5f);
        for (y = 0; y < bh; y++)
            for (x = 0; x < bw; x++) {
                LONG sx = x - dx, sy = y - dy;
                t1[y * bw + x] = (sx >= 0 && sy >= 0 && sx < bw && sy < bh) ? a[sy * bw + sx] : 0;
            }
        blur3(t1, bw, bh, f->sh_size);
    }
    store(l->fxc[FX_SHADOW], t1, bx0, by0, bw, tx0, ty0, tx1, ty1, f->sh_color, f->sh_opacity, f->sh_on);

    /* Schein: weichgezeichnet und verstaerkt */
    if (f->gl_on) {
        CopyMem(a, t1, bw * bh);
        blur3(t1, bw, bh, f->gl_size);
        for (k = 0; k < bw * bh; k++)
            t1[k] = t1[k] >= 128 ? 255 : t1[k] * 2;
    }
    store(l->fxc[FX_GLOW], t1, bx0, by0, bw, tx0, ty0, tx1, ty1, f->gl_color, f->gl_opacity, f->gl_on);

    /* Kontur aussen: Alpha um size Pixel ausgedehnt (liegt unter der Ebene) */
    if (f->st_on) {
        CopyMem(a, t1, bw * bh);
        for (k = 0; k < f->st_size; k++) {
            if (!dilate1(t1, t2, bw, bh, k & 1))
                break;          /* nichts mehr zu wachsen (z. B. ganz deckend) */
            CopyMem(t2, t1, bw * bh);
        }
    }
    store(l->fxc[FX_STROKE], t1, bx0, by0, bw, tx0, ty0, tx1, ty1, f->st_color, f->st_opacity, f->st_on);
out:
    if (a) FreeVec(a);
    if (t1) FreeVec(t1);
    if (t2) FreeVec(t2);
}

/* vor dem Zusammensetzen (doc_process_dirty): Effekte nachziehen */
void fx_process(void)
{
    LONG i, tx, ty, tx0 = doc->tw, ty0 = doc->th, tx1 = -1, ty1 = -1;
    BOOL any = FALSE;
    for (i = 0; i < doc->nlayers; i++)
        if (doc->layers[i]->visible && fx_active(doc->layers[i]->fx))
            any = TRUE;
    if (!any)
        return;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++)
            if (doc->dirty[ty * doc->tw + tx]) {
                if (tx < tx0) tx0 = tx;
                if (tx > tx1) tx1 = tx;
                if (ty < ty0) ty0 = ty;
                if (ty > ty1) ty1 = ty;
            }
    for (i = 0; i < doc->nlayers; i++) {
        Layer *l = doc->layers[i];
        LONG rt, a0, b0, a1, b1;
        if (!l->visible || !fx_active(l->fx)) {
            l->fx_valid = FALSE;    /* beim Einblenden ganz neu */
            continue;
        }
        if (!l->fx_valid || !l->fxc[0]) {
            fx_update(l, 0, 0, doc->tw - 1, doc->th - 1);
            l->fx_valid = TRUE;
            doc_mark_all_dirty();
            continue;
        }
        if (tx1 < 0)
            continue;
        rt = (fx_radius(l->fx) + TILE_SIZE - 1) >> TILE_SHIFT;
        a0 = tx0 - rt > 0 ? tx0 - rt : 0;
        b0 = ty0 - rt > 0 ? ty0 - rt : 0;
        a1 = tx1 + rt < doc->tw - 1 ? tx1 + rt : doc->tw - 1;
        b1 = ty1 + rt < doc->th - 1 ? ty1 + rt : doc->th - 1;
        fx_update(l, a0, b0, a1, b1);
        for (ty = b0; ty <= b1; ty++)
            for (tx = a0; tx <= a1; tx++)
                doc_mark_dirty(tx, ty);
    }
}

/* ---- Dialog ---- */

static Layer *fd_layer;
static LayerFx fd_edit;

static void fx_set(Layer *l, const LayerFx *f)
{
    if (fx_active(f)) {
        if (!l->fx && !(l->fx = AllocVec(sizeof(LayerFx), MEMF_ANY)))
            return;
        *l->fx = *f;
    } else if (l->fx) {
        FreeVec(l->fx);
        l->fx = NULL;
    }
    fx_free_cache(l, doc);
    doc_mark_all_dirty();
}

static void fd_preview(void)
{
    fx_set(fd_layer, &fd_edit);
    doc_process_dirty();        /* hier rechnen, damit die Zeit gemessen wird */
}

void fx_dialog(void)
{
    static const LayerFx defaults = {
        0, 75, 120, 5, 5, 0x000000,
        0, 75, 10, 0xFFFFBE,
        0, 3, 100, 0xFF0000
    };
    LayerFx orig;
    BOOL had;
    UndoRec *u;
    DlgSlider ds[] = {
        { "Drop Shadow", &fd_edit.sh_on, DLG_CHECK, DLG_CHECK, "", 1 },
        { "Opacity:", &fd_edit.sh_opacity, 0, 100, "%", 1 },
        { "Angle:", &fd_edit.sh_angle, -180, 180, "°", 1 },
        { "Distance:", &fd_edit.sh_dist, 0, 100, "px", 1 },
        { "Size:", &fd_edit.sh_size, 0, 100, "px", 1 },
        { "Color:", (LONG *)&fd_edit.sh_color, DLG_COLOR, DLG_COLOR, "", 1 },
        { "Outer Glow", &fd_edit.gl_on, DLG_CHECK, DLG_CHECK, "", 1 },
        { "Opacity:", &fd_edit.gl_opacity, 0, 100, "%", 1 },
        { "Size:", &fd_edit.gl_size, 0, 100, "px", 1 },
        { "Color:", (LONG *)&fd_edit.gl_color, DLG_COLOR, DLG_COLOR, "", 1 },
        { "Stroke", &fd_edit.st_on, DLG_CHECK, DLG_CHECK, "", 1 },
        { "Size:", &fd_edit.st_size, 1, 50, "px", 1 },
        { "Opacity:", &fd_edit.st_opacity, 0, 100, "%", 1 },
        { "Color:", (LONG *)&fd_edit.st_color, DLG_COLOR, DLG_COLOR, "", 1 },
    };

    fd_layer = doc->layers[doc->active];
    had = fd_layer->fx != NULL;
    orig = had ? *fd_layer->fx : defaults;
    fd_edit = orig;
    if (!had)
        fd_edit.sh_on = 1;      /* neu: mit Schlagschatten beginnen */
    if (!dialog_run("Layer Style", ds, sizeof(ds) / sizeof(ds[0]), fd_preview)) {
        fx_set(fd_layer, had ? &orig : NULL);
        return;
    }
    /* Undo: alten Zustand herstellen, Eintrag beginnen, neuen setzen */
    fx_set(fd_layer, had ? &orig : NULL);
    u = undo_begin_struct("Layer Style");
    fx_set(fd_layer, &fd_edit);
    undo_commit(u);
}

void fx_clear(void)
{
    Layer *l = doc->layers[doc->active];
    UndoRec *u;
    if (!l->fx)
        return;
    u = undo_begin_struct("Clear Layer Style");
    fx_set(l, NULL);
    undo_commit(u);
}
