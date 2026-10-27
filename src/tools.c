/*
 * tools.c - Verlaufswerkzeug und Fuellwerkzeug
 *
 * Beide malen ueber paint_area: Quellfarbe je Pixel (mit Alpha) wird mit
 * Deckung (Flood-Maske bzw. ganze Flaeche) x Auswahl x Deckkraft "normal"
 * ueber die Zielkacheln gelegt. Ziel ist die Ebene oder ihre Maske.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <stdio.h>
#include "apx.h"

LONG grad_type = GRAD_LINEAR, grad_mode = GRAD_FG_BG, grad_opacity = 100;
BOOL grad_reverse = FALSE;
LONG bucket_tol = 32, bucket_opacity = 100;
BOOL bucket_contig = TRUE, bucket_aa = TRUE;

/* Fuellt Bereich x0..x1/y0..y1 mit src(x, y); cov: Deckung je Dokumentpixel oder NULL */
static BOOL pa_nosel;            /* Stroke: Auswahl nicht als Maske benutzen */

static void paint_area(const char *name, LONG x0, LONG y0, LONG x1, LONG y1,
                       const UBYTE *cov, ULONG (*src)(LONG x, LONG y), ULONG opac)
{
    BOOL usesel = doc->hassel && !pa_nosel;
    BOOL to_mask = doc_target_is_mask();
    Tile **arr;
    UndoRec *u;
    LONG tx, ty, x, y;

    if (!to_mask)
        doc_rasterize_text(doc->layers[doc->active]);
    arr = doc_target();
    if (usesel) {
        if (x0 < doc->selx0) x0 = doc->selx0;
        if (y0 < doc->sely0) y0 = doc->sely0;
        if (x1 > doc->selx1) x1 = doc->selx1;
        if (y1 > doc->sely1) y1 = doc->sely1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    opac += opac >> 7;
    u = undo_begin_target(name);
    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx, ox = tx << TILE_SHIFT, oy = ty << TILE_SHIFT;
            LONG ax0 = x0 > ox ? x0 : ox, ay0 = y0 > oy ? y0 : oy;
            LONG ax1 = x1 < ox + TILE_SIZE ? x1 : ox + TILE_SIZE;
            LONG ay1 = y1 < oy + TILE_SIZE ? y1 : oy + TILE_SIZE;
            Tile *t = arr[idx];
            BOOL any = FALSE;
            /* betroffen? (Deckung und Auswahl) */
            for (y = ay0; y < ay1 && !any; y++)
                for (x = ax0; x < ax1; x++) {
                    LONG i = y * doc->w + x;
                    if ((!cov || cov[i]) && (!usesel || doc->sel[i])) {
                        any = TRUE;
                        break;
                    }
                }
            if (!any)
                continue;
            undo_add(u, idx, t ? tile_clone(t) : NULL);
            if (!t) {
                if (!(t = tile_new()))
                    continue;
                arr[idx] = t;
            }
            for (y = ay0; y < ay1; y++) {
                ULONG *p = t->px + ((y - oy) << TILE_SHIFT) - ox;
                for (x = ax0; x < ax1; x++) {
                    LONG i = y * doc->w + x;
                    ULONG w = cov ? cov[i] : 255, s, sa, o, da, oa, inv;
                    if (usesel)
                        w = (w * doc->sel[i] * 257 + 0x8080) >> 16;
                    if (!w)
                        continue;
                    s = src(x, y);
                    if (to_mask)
                        s = (s & 0xFF000000) | (mask_gray(s) * 0x010101);
                    sa = ((((s >> 24) * w * 257 + 0x8080) >> 16) * opac) >> 8;
                    if (!sa)
                        continue;
                    o = p[x];
                    da = ((o >> 24) * (255 - sa) * 257 + 0x8080) >> 16;
                    oa = sa + da;
                    inv = inv_tab[oa];
                    p[x] = (oa << 24)
                         | (((((s >> 16) & 0xFF) * sa + ((o >> 16) & 0xFF) * da) * inv >> 16) << 16)
                         | (((((s >> 8) & 0xFF) * sa + ((o >> 8) & 0xFF) * da) * inv >> 16) << 8)
                         | (((s & 0xFF) * sa + (o & 0xFF) * da) * inv >> 16);
                }
            }
            tile_update_flag(doc, t, tx, ty);
            doc_mark_dirty(tx, ty);
        }
    undo_commit(u);
}

/* ---- Verlauf ---- */

static float g_ax, g_ay, g_dx, g_dy, g_inv, g_len;
static ULONG g_c0, g_c1;
static BOOL g_trans;
static const UBYTE bayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static ULONG grad_src(LONG x, LONG y)
{
    float px = x + 0.5f - g_ax, py = y + 0.5f - g_ay, t, d;
    LONG r, g, b, a, k;
    if (grad_type == GRAD_RADIAL)
        t = sqrtf(px * px + py * py) * g_len;
    else
        t = (px * g_dx + py * g_dy) * g_inv;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (grad_reverse)
        t = 1.0f - t;
    /* geordnetes Dithering gegen Treppenstufen */
    d = (bayer4[((y & 3) << 2) | (x & 3)] - 7.5f) * (1.0f / 16.0f);
    if (g_trans) {
        a = (LONG)(255.0f * (1.0f - t) + d + 0.5f);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        return ((ULONG)a << 24) | (g_c0 & 0xFFFFFF);
    }
    r = (LONG)(((g_c0 >> 16) & 0xFF) + ((LONG)((g_c1 >> 16) & 0xFF) - (LONG)((g_c0 >> 16) & 0xFF)) * t + d + 0.5f);
    g = (LONG)(((g_c0 >> 8) & 0xFF) + ((LONG)((g_c1 >> 8) & 0xFF) - (LONG)((g_c0 >> 8) & 0xFF)) * t + d + 0.5f);
    b = (LONG)((g_c0 & 0xFF) + ((LONG)(g_c1 & 0xFF) - (LONG)(g_c0 & 0xFF)) * t + d + 0.5f);
    k = r | g | b;
    if (k & ~0xFF) {
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
    }
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void tool_gradient(float x0, float y0, float x1, float y1)
{
    float l2;
    g_ax = x0;
    g_ay = y0;
    g_dx = x1 - x0;
    g_dy = y1 - y0;
    l2 = g_dx * g_dx + g_dy * g_dy;
    if (l2 < 1.0f)
        return;
    g_inv = 1.0f / l2;
    g_len = 1.0f / sqrtf(l2);
    g_c0 = fg_color;
    g_c1 = bg_color;
    g_trans = grad_mode == GRAD_FG_TRANS;
    paint_area("Gradient", 0, 0, doc->w, doc->h, NULL, grad_src, grad_opacity * 255 / 100);
}

/* ---- Fuellwerkzeug ---- */

static ULONG bucket_src(LONG x, LONG y)
{
    return 0xFF000000 | fg_color;
}

void tool_bucket(LONG x, LONG y)
{
    LONG bb[4];
    UBYTE *m;
    if (x < 0 || y < 0 || x >= doc->w || y >= doc->h)
        return;
    if (doc->hassel && !doc->sel[y * doc->w + x])
        return;                 /* Klick ausserhalb der Auswahl: nichts */
    if (!(m = wand_mask(doc_target(), x, y, bucket_tol, bucket_contig, bucket_aa, bb))) {
        status_message("Not enough memory");
        return;
    }
    paint_area("Paint Bucket", bb[0], bb[1], bb[2], bb[3], m, bucket_src, bucket_opacity * 255 / 100);
    FreeVec(m);
}

/* ---- Formen: Rechteck, Ellipse, Linie (je eine neue Ebene) ---- */

LONG shape_type = SHAPE_RECT, shape_stroke_w = 0, shape_weight = 4;
ULONG shape_fill = 0x3C78D8, shape_stroke = 0x000000;
BOOL shape_fill_on = TRUE;
static LONG shape_count[3];


static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Abstand zur Kante (negativ innen), x/y relativ zur Mitte */
static float shape_dist(float x, float y, float rx, float ry)
{
    if (shape_type == SHAPE_RECT) {
        float dx = fabsf(x) - rx, dy = fabsf(y) - ry;
        if (dx > 0.0f && dy > 0.0f)
            return sqrtf(dx * dx + dy * dy);
        return dx > dy ? dx : dy;
    } else {
        /* Ellipse: naechster Randpunkt iterativ (3 Schritte reichen sichtbar),
         * die Gradienten-Naeherung unterschaetzt weit draussen */
        float px = fabsf(x), py = fabsf(y), tx = 0.70710678f, ty = 0.70710678f, d;
        float a = rx, b = ry;
        LONG k;
        for (k = 0; k < 3; k++) {
            float ex = (a * a - b * b) * tx * tx * tx / a, ey = (b * b - a * a) * ty * ty * ty / b;
            float qx = px - ex, qy = py - ey, r, q, t;
            r = sqrtf((a * tx - ex) * (a * tx - ex) + (b * ty - ey) * (b * ty - ey));
            q = sqrtf(qx * qx + qy * qy);
            if (q < 1e-6f)
                break;
            tx = clamp01((qx * r / q + ex) / a);
            ty = clamp01((qy * r / q + ey) / b);
            t = sqrtf(tx * tx + ty * ty);
            if (t < 1e-6f)
                break;
            tx /= t;
            ty /= t;
        }
        d = sqrtf((px - a * tx) * (px - a * tx) + (py - b * ty) * (py - b * ty));
        return (x * x) / (a * a) + (y * y) / (b * b) < 1.0f ? -d : d;
    }
}

static ULONG over_px(ULONG d, ULONG c, float a)
{
    ULONG sa = (ULONG)(a * 255.0f + 0.5f), da, oa, inv;
    if (!sa)
        return d;
    da = ((d >> 24) * (255 - sa) * 257 + 0x8080) >> 16;
    oa = sa + da;
    inv = inv_tab[oa];
    return (oa << 24)
         | (((((c >> 16) & 0xFF) * sa + ((d >> 16) & 0xFF) * da) * inv >> 16) << 16)
         | (((((c >> 8) & 0xFF) * sa + ((d >> 8) & 0xFF) * da) * inv >> 16) << 8)
         | (((c & 0xFF) * sa + (d & 0xFF) * da) * inv >> 16);
}

void tool_shape(float x0, float y0, float x1, float y1)
{
    static const char *names[3] = { "Rectangle", "Ellipse", "Line" };
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, rx = fabsf(x1 - x0) * 0.5f, ry = fabsf(y1 - y0) * 0.5f;
    float hw = (shape_type == SHAPE_LINE ? shape_weight : shape_stroke_w) * 0.5f;
    float lx = x1 - x0, ly = y1 - y0, ll = lx * lx + ly * ly;
    LONG bx0, by0, bx1, by1, tx, ty, x, y, i;
    char name[32];
    Layer *l;
    UndoRec *u;

    if (doc->nlayers >= MAX_LAYERS)
        return;
    if (shape_type == SHAPE_LINE ? ll < 1.0f : (rx < 0.5f || ry < 0.5f))
        return;
    bx0 = (LONG)floorf((x0 < x1 ? x0 : x1) - hw - 2.0f);
    by0 = (LONG)floorf((y0 < y1 ? y0 : y1) - hw - 2.0f);
    bx1 = (LONG)ceilf((x0 > x1 ? x0 : x1) + hw + 2.0f);
    by1 = (LONG)ceilf((y0 > y1 ? y0 : y1) + hw + 2.0f);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > doc->w) bx1 = doc->w;
    if (by1 > doc->h) by1 = doc->h;
    sprintf(name, "%s %ld", names[shape_type], (long)++shape_count[shape_type]);
    if (!(l = layer_new(doc, name)))
        return;
    for (ty = by0 >> TILE_SHIFT; ty <= (by1 - 1) >> TILE_SHIFT && by1 > by0; ty++)
        for (tx = bx0 >> TILE_SHIFT; tx <= (bx1 - 1) >> TILE_SHIFT && bx1 > bx0; tx++) {
            LONG ox = tx << TILE_SHIFT, oy = ty << TILE_SHIFT;
            Tile *t = tile_new();
            if (!t)
                continue;
            for (y = 0; y < TILE_SIZE; y++) {
                LONG gy = oy + y;
                if (gy < by0 || gy >= by1)
                    continue;
                for (x = 0; x < TILE_SIZE; x++) {
                    LONG gx = ox + x;
                    float px = gx + 0.5f, py = gy + 0.5f;
                    ULONG p = 0;
                    if (gx < bx0 || gx >= bx1)
                        continue;
                    if (shape_type == SHAPE_LINE) {
                        /* Abstand zur Strecke */
                        float t = ((px - x0) * lx + (py - y0) * ly) / ll, dx, dy;
                        if (t < 0.0f) t = 0.0f;
                        if (t > 1.0f) t = 1.0f;
                        dx = px - (x0 + t * lx);
                        dy = py - (y0 + t * ly);
                        p = over_px(p, shape_fill, clamp01(hw + 0.5f - sqrtf(dx * dx + dy * dy)));
                    } else {
                        float d = shape_dist(px - cx, py - cy, rx, ry);
                        if (shape_fill_on)
                            p = over_px(p, shape_fill, clamp01(0.5f - d));
                        if (hw > 0.0f)  /* Kontur mittig auf der Kante */
                            p = over_px(p, shape_stroke, clamp01(hw + 0.5f - fabsf(d)));
                    }
                    t->px[(y << TILE_SHIFT) | x] = p;
                }
            }
            tile_update_flag(doc, t, tx, ty);
            if (t->flag == TF_EMPTY)
                FreeVec(t);
            else
                l->tiles[ty * doc->tw + tx] = t;
        }
    u = undo_begin_struct(shape_type == SHAPE_RECT ? "Rectangle Tool" :
                          shape_type == SHAPE_ELLIPSE ? "Ellipse Tool" : "Line Tool");
    for (i = doc->nlayers; i > doc->active + 1; i--)
        doc->layers[i] = doc->layers[i - 1];
    doc->layers[doc->active + 1] = l;
    doc->nlayers++;
    doc->active++;
    doc->edit_mask = FALSE;
    doc_mark_all_dirty();
    undo_commit(u);
}

/* ---- Edit > Fill... und Edit > Stroke... ---- */

LONG fill_content, fill_opacity = 100, fill_preserve;
ULONG fill_color = 0x808080;
LONG stroke_width = 3, stroke_loc = 1, stroke_opacity = 100;
ULONG stroke_color = 0x000000;
static ULONG fs_col;

static ULONG fs_src(LONG x, LONG y)
{
    return 0xFF000000 | fs_col;
}

/* mit gesperrter Transparenz malen (Undo-Abschluss stellt Alpha wieder her) */
static void paint_preserve(const char *name, LONG x0, LONG y0, LONG x1, LONG y1, const UBYTE *cov,
                           ULONG opac, BOOL preserve)
{
    Layer *l = doc->layers[doc->active];
    UBYTE old = l->lock;
    if (preserve)
        l->lock |= LOCK_ALPHA;
    paint_area(name, x0, y0, x1, y1, cov, fs_src, opac);
    l->lock = old;
}

void edit_fill(void)
{
    static const ULONG fixed[6] = { 0, 0, 0, 0x000000, 0x808080, 0xFFFFFF };
    DlgSlider ds[] = {
        { "Contents:", &fill_content, DLG_CHOICE, DLG_CHOICE,
          "Foreground|Background|Color|Black|50% Gray|White", 1 },
        { "Color:", (LONG *)&fill_color, DLG_COLOR, DLG_COLOR, "", 1 },
        { "Opacity:", &fill_opacity, 1, 100, "%", 1 },
        { "Preserve Transparency", &fill_preserve, DLG_CHECK, DLG_CHECK, "", 1 },
    };
    if (!dialog_run("Fill", ds, 4, NULL))
        return;
    fs_col = fill_content == 0 ? fg_color : fill_content == 1 ? bg_color :
             fill_content == 2 ? fill_color : fixed[fill_content];
    paint_preserve("Fill", 0, 0, doc->w, doc->h, NULL, fill_opacity * 255 / 100, fill_preserve);
}

/* eine Stufe Dilatation (3x3 bzw. Kreuz abwechselnd ~ rund) */
static void dil1(const UBYTE *s, UBYTE *d, LONG w, LONG h, BOOL square)
{
    LONG x, y;
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
            o[x] = m;
        }
    }
}

static void dilate_n(UBYTE *b, UBYTE *tmp, LONG w, LONG h, LONG n)
{
    LONG k;
    for (k = 0; k < n; k++) {
        dil1(b, tmp, w, h, k & 1);
        CopyMem(tmp, b, w * h);
    }
}

void edit_stroke(void)
{
    DlgSlider ds[] = {
        { "Width:", &stroke_width, 1, 100, "px", 1 },
        { "Color:", (LONG *)&stroke_color, DLG_COLOR, DLG_COLOR, "", 1 },
        { "Location:", &stroke_loc, DLG_CHOICE, DLG_CHOICE, "Inside|Center|Outside", 1 },
        { "Opacity:", &stroke_opacity, 1, 100, "%", 1 },
    };
    LONG din, dout, m, x0, y0, x1, y1, bw, bh, x, y;
    UBYTE *dl, *er, *tmp, *cov;
    if (!doc->hassel) {
        status_message("Stroke needs a selection");
        return;
    }
    stroke_color = fg_color;
    if (!dialog_run("Stroke", ds, 4, NULL))
        return;
    dout = stroke_loc == 0 ? 0 : stroke_loc == 1 ? stroke_width / 2 : stroke_width;
    din = stroke_width - dout;
    m = stroke_width + 2;
    x0 = doc->selx0 - m; y0 = doc->sely0 - m; x1 = doc->selx1 + m; y1 = doc->sely1 + m;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    bw = x1 - x0;
    bh = y1 - y0;
    dl = AllocVec(bw * bh, MEMF_ANY);
    er = AllocVec(bw * bh, MEMF_ANY);
    tmp = AllocVec(bw * bh, MEMF_ANY);
    cov = AllocVec(doc->w * doc->h, MEMF_ANY | MEMF_CLEAR);
    if (dl && er && tmp && cov) {
        for (y = 0; y < bh; y++)
            for (x = 0; x < bw; x++) {
                UBYTE v = doc->sel[(y0 + y) * doc->w + x0 + x];
                dl[y * bw + x] = v;
                er[y * bw + x] = 255 - v;   /* Schrumpfen = Ausdehnen des Rests */
            }
        dilate_n(dl, tmp, bw, bh, dout);
        dilate_n(er, tmp, bw, bh, din);
        for (y = 0; y < bh; y++)
            for (x = 0; x < bw; x++) {
                LONG v = (LONG)dl[y * bw + x] - (255 - (LONG)er[y * bw + x]);
                cov[(y0 + y) * doc->w + x0 + x] = (UBYTE)(v < 0 ? 0 : v);
            }
        fs_col = stroke_color;
        pa_nosel = TRUE;
        paint_area("Stroke", x0, y0, x1, y1, cov, fs_src, stroke_opacity * 255 / 100);
        pa_nosel = FALSE;
    } else
        status_message("Not enough memory");
    if (dl) FreeVec(dl);
    if (er) FreeVec(er);
    if (tmp) FreeVec(tmp);
    if (cov) FreeVec(cov);
}

/* Auswahl mit der Vordergrundfarbe fuellen (Fill Path) */
void fill_selection_fg(const char *name)
{
    fs_col = fg_color;
    paint_area(name, 0, 0, doc->w, doc->h, NULL, fs_src, 255);
}
