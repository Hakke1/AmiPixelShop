/*
 * transform.c - Frei transformieren (Strg+T), Spiegeln, Drehen
 *
 * Wie beim Verschieben werden die Pixel herausgeloest (mit Auswahl) bzw. die
 * ganze Ebene genommen und als "schwebender" Puffer (premultiplied) gehalten.
 * Abbildung: Ziel = C + R(winkel) * S * (Quelle - Quellmitte).
 * Gerendert wird rueckwaerts (je Zielpixel die Quellposition), die innere
 * Schleife laeuft affin mit konstanten Schrittweiten.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "apx.h"

static BOOL tf_on;
static Layer *tf_layer;
static Tile **tf_orig;          /* Kopien aller Kacheln vorher */
static Tile **tf_base;          /* Ebene ohne die schwebenden Pixel */
static UBYTE *tf_base_own;
static UBYTE *tf_touched;
static ULONG *fpx;              /* schwebende Pixel, premultiplied */
static UBYTE *fsel;             /* zugehoerige Auswahl (oder NULL) */
static LONG fx0, fy0, fw, fh;   /* Lage der Quelle */
static BOOL tf_hadsel;

/* aktuelle Abbildung */
static float tcx, tcy, tsx = 1.0f, tsy = 1.0f, tang;
static LONG bx0, by0, bx1, by1; /* zuletzt gerenderter Zielbereich */
static BOOL tf_rendered;

/* Ziehen */
enum { DR_NONE, DR_MOVE, DR_ROTATE, DR_SCALE };
static LONG drag, drag_handle;
static float d_mx, d_my, d_cx, d_cy, d_sx, d_sy, d_ang;
static LONG ov_x0, ov_y0, ov_x1, ov_y1;    /* Overlay-Box am Schirm */

BOOL transform_active(void)
{
    return tf_on;
}

/* ---- Hilfen ---- */

static ULONG premul(ULONG p)
{
    ULONG a = p >> 24, a2 = a + (a >> 7);
    if (a == 255)
        return p;
    return (a << 24) | ((((p & 0xFF00FF) * a2) >> 8) & 0xFF00FF) | ((((p & 0x00FF00) * a2) >> 8) & 0x00FF00);
}

static ULONG unpremul(ULONG p)
{
    ULONG a = p >> 24, inv, r, g, b;
    if (a == 255 || !a)
        return a ? p : 0;
    inv = (255 * 65536 + a / 2) / a;
    r = (((p >> 16) & 0xFF) * inv) >> 16;
    g = (((p >> 8) & 0xFF) * inv) >> 16;
    b = ((p & 0xFF) * inv) >> 16;
    return (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

/* straight src-over */
static ULONG over(ULONG d, ULONG s)
{
    ULONG sa = s >> 24, da, oa, inv;
    if (!sa)
        return d;
    if (sa == 255)
        return s;
    da = ((d >> 24) * (255 - sa) * 257 + 0x8080) >> 16;
    oa = sa + da;
    inv = inv_tab[oa];
    return (oa << 24)
         | (((((s >> 16) & 0xFF) * sa + ((d >> 16) & 0xFF) * da) * inv >> 16) << 16)
         | (((((s >> 8) & 0xFF) * sa + ((d >> 8) & 0xFF) * da) * inv >> 16) << 8)
         | (((s & 0xFF) * sa + (d & 0xFF) * da) * inv >> 16);
}

/* Quellpunkt (relativ zur Quellmitte, unskaliert) -> Dokument */
static void map_pt(float u, float v, float *x, float *y)
{
    float c = cosf(tang), s = sinf(tang);
    u *= tsx;
    v *= tsy;
    *x = tcx + c * u - s * v;
    *y = tcy + s * u + c * v;
}

/* Ecke i (0 oben links, im Uhrzeigersinn) bzw. Kantenmitte i (4..7) */
static void handle_pt(LONG i, float *x, float *y)
{
    static const float hu[8] = { -1, 1, 1, -1, 0, 1, 0, -1 };
    static const float hv[8] = { -1, -1, 1, 1, -1, 0, 1, 0 };
    map_pt(hu[i] * fw * 0.5f, hv[i] * fh * 0.5f, x, y);
}

static void dest_bounds(LONG *x0, LONG *y0, LONG *x1, LONG *y1)
{
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f, x, y;
    LONG i;
    for (i = 0; i < 4; i++) {
        handle_pt(i, &x, &y);
        if (x < mnx) mnx = x;
        if (y < mny) mny = y;
        if (x > mxx) mxx = x;
        if (y > mxy) mxy = y;
    }
    *x0 = (LONG)floorf(mnx) - 1;
    *y0 = (LONG)floorf(mny) - 1;
    *x1 = (LONG)ceilf(mxx) + 1;
    *y1 = (LONG)ceilf(mxy) + 1;
}

/* ---- Overlay: Rahmen und Anfasser ---- */

static void tf_overlay(void)
{
    float px[8], py[8];
    LONG i, sx[8], sy[8];
    if (!tf_on)
        return;
    for (i = 0; i < 8; i++) {
        handle_pt(i, &px[i], &py[i]);
        sx[i] = ox + (LONG)floorf(px[i] * zoom + 0.5f);
        sy[i] = oy + (LONG)floorf(py[i] * zoom + 0.5f);
    }
    for (i = 0; i < 4; i++)
        ant_line(sx[i], sy[i], sx[(i + 1) & 3], sy[(i + 1) & 3], 0);
    for (i = 0; i < 8; i++) {
        fill(sx[i] - 3, sy[i] - 3, 7, 7, 0xFFFFFFFF);
        frame(sx[i] - 4, sy[i] - 4, 9, 9, 0xFF000000);
    }
    {
        LONG cx = ox + (LONG)floorf(tcx * zoom + 0.5f), cy = oy + (LONG)floorf(tcy * zoom + 0.5f);
        line(cx - 4, cy, cx + 4, cy, 0xFF000000);
        line(cx, cy - 4, cx, cy + 4, 0xFF000000);
    }
}

static void overlay_changed(void)
{
    LONG i, x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y1 = -0x7FFFFFFF;
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
    for (i = 0; i < 8; i++) {
        float x, y;
        LONG s, t;
        handle_pt(i, &x, &y);
        s = ox + (LONG)floorf(x * zoom + 0.5f);
        t = oy + (LONG)floorf(y * zoom + 0.5f);
        if (s < x0) x0 = s;
        if (t < y0) y0 = t;
        if (s > x1) x1 = s;
        if (t > y1) y1 = t;
    }
    ov_x0 = x0 - 6; ov_y0 = y0 - 6;
    ov_x1 = x1 + 7; ov_y1 = y1 + 7;
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
}

/* ---- Rendern ---- */

/* Kacheln im Rechteck aus tf_base + transformierter Quelle neu bauen */
static void render_rect(LONG x0, LONG y0, LONG x1, LONG y1, BOOL smooth)
{
    float c = cosf(tang), s = sinf(tang);
    /* inverse Abbildung: Quelle = M * (Ziel - C) + Quellmitte (in Pufferkoord.) */
    float m00 = c / tsx, m01 = s / tsx, m10 = -s / tsy, m11 = c / tsy;
    LONG tx, ty;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx, gx0 = tx << TILE_SHIFT, gy0 = ty << TILE_SHIFT, x, y;
            Tile *t = tf_layer->tiles[idx];
            if (!t && !(t = tile_new()))
                continue;
            tf_layer->tiles[idx] = t;
            if (tf_base[idx])
                memcpy(t->px, tf_base[idx]->px, sizeof(t->px));
            else
                memset(t->px, 0, sizeof(t->px));
            for (y = 0; y < TILE_SIZE; y++) {
                LONG gy = gy0 + y;
                float dy = gy + 0.5f - tcy;
                /* Quellkoordinaten am Zeilenanfang, Schritt je Pixel */
                float dx = gx0 + 0.5f - tcx;
                float u = m00 * dx + m01 * dy + fw * 0.5f;
                float v = m10 * dx + m11 * dy + fh * 0.5f;
                ULONG *row = t->px + (y << TILE_SHIFT);
                if (gy >= doc->h)
                    break;
                for (x = 0; x < TILE_SIZE; x++, u += m00, v += m10) {
                    ULONG p;
                    if (gx0 + x >= doc->w)
                        break;
                    if (!smooth) {
                        LONG iu = (LONG)floorf(u), iv = (LONG)floorf(v);
                        if (iu < 0 || iv < 0 || iu >= fw || iv >= fh)
                            continue;
                        p = fpx[iv * fw + iu];
                    } else {
                        /* bilinear, ausserhalb = transparent (weiche Kanten) */
                        float uu = u - 0.5f, vv = v - 0.5f;
                        LONG iu = (LONG)floorf(uu), iv = (LONG)floorf(vv), k;
                        ULONG q[4], wu, wv, out = 0;
                        if (iu < -1 || iv < -1 || iu >= fw || iv >= fh)
                            continue;
                        wu = (ULONG)((uu - iu) * 256.0f);
                        wv = (ULONG)((vv - iv) * 256.0f);
                        q[0] = (iu >= 0 && iv >= 0) ? fpx[iv * fw + iu] : 0;
                        q[1] = (iu + 1 < fw && iv >= 0) ? fpx[iv * fw + iu + 1] : 0;
                        q[2] = (iu >= 0 && iv + 1 < fh) ? fpx[(iv + 1) * fw + iu] : 0;
                        q[3] = (iu + 1 < fw && iv + 1 < fh) ? fpx[(iv + 1) * fw + iu + 1] : 0;
                        for (k = 0; k < 32; k += 8) {
                            ULONG a = (q[0] >> k) & 0xFF, b = (q[1] >> k) & 0xFF;
                            ULONG cc = (q[2] >> k) & 0xFF, d = (q[3] >> k) & 0xFF;
                            ULONG top = a * (256 - wu) + b * wu, bot = cc * (256 - wu) + d * wu;
                            out |= (((top * (256 - wv) + bot * wv) + 32768) >> 16) << k;
                        }
                        p = out;
                    }
                    if (p)
                        row[x] = over(row[x], unpremul(p));
                }
            }
            tile_update_flag(doc, t, tx, ty);
            if (t->flag == TF_EMPTY) {
                FreeVec(t);
                tf_layer->tiles[idx] = NULL;
            }
            tf_touched[idx] = 1;
            doc_mark_dirty(tx, ty);
        }
}

static void render(BOOL smooth)
{
    LONG x0, y0, x1, y1;
    dest_bounds(&x0, &y0, &x1, &y1);
    if (tf_rendered) {
        /* alte Lage freiraeumen (falls ausserhalb der neuen) */
        if (bx0 < x0 || by0 < y0 || bx1 > x1 || by1 > y1) {
            LONG ux0 = bx0 < x0 ? bx0 : x0, uy0 = by0 < y0 ? by0 : y0;
            LONG ux1 = bx1 > x1 ? bx1 : x1, uy1 = by1 > y1 ? by1 : y1;
            render_rect(ux0, uy0, ux1, uy1, smooth);
        } else
            render_rect(x0, y0, x1, y1, smooth);
    } else {
        /* erstes Mal: auch die Ausgangslage (Loch) neu bauen */
        LONG ux0 = fx0 < x0 ? fx0 : x0, uy0 = fy0 < y0 ? fy0 : y0;
        LONG ux1 = fx0 + fw > x1 ? fx0 + fw : x1, uy1 = fy0 + fh > y1 ? fy0 + fh : y1;
        render_rect(ux0, uy0, ux1, uy1, smooth);
    }
    bx0 = x0; by0 = y0; bx1 = x1; by1 = y1;
    tf_rendered = TRUE;
    overlay_changed();
}

/* ---- Beginn / Ende ---- */

static void free_state(void)
{
    LONG i, n = doc->tw * doc->th;
    for (i = 0; i < n; i++) {
        if (tf_base_own && tf_base_own[i] && tf_base[i])
            FreeVec(tf_base[i]);
        if (tf_orig && tf_orig[i])
            FreeVec(tf_orig[i]);
    }
    if (tf_orig) FreeVec(tf_orig);
    if (tf_base) FreeVec(tf_base);
    if (tf_base_own) FreeVec(tf_base_own);
    if (tf_touched) FreeVec(tf_touched);
    if (fpx) FreeVec(fpx);
    if (fsel) FreeVec(fsel);
    tf_orig = tf_base = NULL;
    tf_base_own = tf_touched = NULL;
    fpx = NULL;
    fsel = NULL;
    tf_on = FALSE;
    view_transform_overlay = NULL;
}

BOOL transform_begin(void)
{
    LONG n = doc->tw * doc->th, i, x, y;
    if (tf_on)
        return TRUE;
    doc_rasterize_text(doc->layers[doc->active]);
    doc_cache_below(doc->active);   /* Unterbau aendert sich beim Ziehen nicht */
    tf_layer = doc->layers[doc->active];
    tf_hadsel = doc->hassel;
    tf_orig = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    tf_base = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    tf_base_own = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    tf_touched = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    if (!tf_orig || !tf_base || !tf_base_own || !tf_touched)
        goto fail;
    for (i = 0; i < n; i++)
        if (tf_layer->tiles[i] && !(tf_orig[i] = tile_clone(tf_layer->tiles[i])))
            goto fail;

    if (tf_hadsel) {
        fx0 = doc->selx0; fy0 = doc->sely0;
        fw = doc->selx1 - fx0; fh = doc->sely1 - fy0;
        for (i = 0; i < n; i++)
            tf_base[i] = tf_orig[i];
    } else {
        /* Inhalt der Ebene eingrenzen */
        LONG x0 = doc->w, y0 = doc->h, x1 = 0, y1 = 0;
        for (y = 0; y < doc->h; y++)
            for (x = 0; x < doc->w; x++)
                if (layer_get(tf_layer, x, y) >> 24) {
                    if (x < x0) x0 = x;
                    if (x >= x1) x1 = x + 1;
                    if (y < y0) y0 = y;
                    y1 = y + 1;
                }
        if (x1 <= x0) {
            status_message("The layer is empty");
            free_state();
            doc_cache_drop();
            return FALSE;
        }
        fx0 = x0; fy0 = y0; fw = x1 - x0; fh = y1 - y0;
        /* Basis: leer (die ganze Ebene schwebt) */
    }
    fpx = AllocVec(fw * fh * 4, MEMF_ANY);
    if (tf_hadsel)
        fsel = AllocVec(fw * fh, MEMF_ANY);
    if (!fpx || (tf_hadsel && !fsel))
        goto fail;
    for (y = 0; y < fh; y++)
        for (x = 0; x < fw; x++) {
            LONG gx = fx0 + x, gy = fy0 + y;
            ULONG p = layer_get(tf_layer, gx, gy);
            if (tf_hadsel) {
                ULONG s = doc->sel[gy * doc->w + gx], a = p >> 24;
                LONG idx = (gy >> TILE_SHIFT) * doc->tw + (gx >> TILE_SHIFT);
                fsel[y * fw + x] = (UBYTE)s;
                p = (((a * s * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
                /* Loch in die Basis stanzen */
                if (s && a && tf_orig[idx]) {
                    ULONG *q;
                    if (!tf_base_own[idx]) {
                        Tile *c = tile_clone(tf_orig[idx]);
                        if (c) {
                            tf_base[idx] = c;
                            tf_base_own[idx] = 1;
                        }
                    }
                    if (tf_base_own[idx]) {
                        q = &tf_base[idx]->px[((gy & TILE_MASK) << TILE_SHIFT) | (gx & TILE_MASK)];
                        *q = ((((*q >> 24) * (255 - s) * 257 + 0x8080) >> 16) << 24) | (*q & 0xFFFFFF);
                    }
                }
            }
            fpx[y * fw + x] = premul(p);
        }
    tcx = fx0 + fw * 0.5f;
    tcy = fy0 + fh * 0.5f;
    tsx = tsy = 1.0f;
    tang = 0.0f;
    tf_rendered = FALSE;
    drag = DR_NONE;
    tf_on = TRUE;
    if (tf_hadsel) {
        doc->hassel = FALSE;    /* waehrend des Transformierens keine Ameisen */
        view_ants_rebuild();
    }
    view_transform_overlay = tf_overlay;
    ov_x0 = ov_x1 = 0;
    overlay_changed();
    return TRUE;
fail:
    doc_cache_drop();
    free_state();
    status_message("Not enough memory");
    return FALSE;
}

/* Auswahl mittransformieren (naechster Nachbar) */
static void transform_selection(void)
{
    float c = cosf(tang), s = sinf(tang);
    float m00 = c / tsx, m01 = s / tsx, m10 = -s / tsy, m11 = c / tsy;
    LONG x, y, x0, y0, x1, y1;
    memset(doc->sel, 0, doc->w * doc->h);
    dest_bounds(&x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = x + 0.5f - tcx, dy = y + 0.5f - tcy;
            LONG iu = (LONG)floorf(m00 * dx + m01 * dy + fw * 0.5f);
            LONG iv = (LONG)floorf(m10 * dx + m11 * dy + fh * 0.5f);
            if (iu >= 0 && iv >= 0 && iu < fw && iv < fh)
                doc->sel[y * doc->w + x] = fsel[iv * fw + iu];
        }
    sel_update_bounds();
}

void transform_apply(void)
{
    UndoRec *u;
    LONG i, n;
    if (!tf_on)
        return;
    if (!tf_rendered && tsx == 1.0f && tsy == 1.0f && tang == 0.0f) {
        transform_cancel();     /* nichts veraendert */
        return;
    }
    render(TRUE);               /* endgueltig in voller Qualitaet */
    doc_cache_drop();
    n = doc->tw * doc->th;
    u = undo_begin(tf_layer, "Free Transform");
    for (i = 0; i < n; i++)
        if (tf_touched[i]) {
            undo_add(u, i, tf_orig[i]);
            tf_orig[i] = NULL;
        }
    undo_commit(u);
    if (tf_hadsel)
        transform_selection();
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
    free_state();
    view_ants_rebuild();
}

void transform_cancel(void)
{
    LONG i, n;
    if (!tf_on)
        return;
    doc_cache_drop();
    n = doc->tw * doc->th;
    for (i = 0; i < n; i++)
        if (tf_touched[i]) {
            if (tf_layer->tiles[i])
                FreeVec(tf_layer->tiles[i]);
            tf_layer->tiles[i] = tf_orig[i];
            tf_orig[i] = NULL;
            doc_mark_dirty(i % doc->tw, i / doc->tw);
        }
    if (tf_hadsel) {
        doc->hassel = TRUE;
        sel_update_bounds();
    }
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
    free_state();
    view_ants_rebuild();
}

/* Spiegeln / Drehen in einem Schritt (naechster Nachbar = pixelgenau) */
void transform_quick(float sx, float sy, float ang)
{
    if (!transform_begin())
        return;
    tsx = sx;
    tsy = sy;
    tang = ang;
    render(FALSE);
    {
        /* render(TRUE) in apply wuerde bei 90-Grad-Schritten weichzeichnen */
        UndoRec *u;
        LONG i, n = doc->tw * doc->th;
        u = undo_begin(tf_layer, "Transform");
        for (i = 0; i < n; i++)
            if (tf_touched[i]) {
                undo_add(u, i, tf_orig[i]);
                tf_orig[i] = NULL;
            }
        undo_commit(u);
        if (tf_hadsel)
            transform_selection();
        view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
        free_state();
        view_ants_rebuild();
    }
}

/* ---- Maus ---- */

static LONG hit_handle(LONG mx, LONG my)
{
    LONG i;
    for (i = 0; i < 8; i++) {
        float x, y;
        LONG sx, sy;
        handle_pt(i, &x, &y);
        sx = ox + (LONG)floorf(x * zoom + 0.5f);
        sy = oy + (LONG)floorf(y * zoom + 0.5f);
        if (mx >= sx - 6 && mx <= sx + 6 && my >= sy - 6 && my <= sy + 6)
            return i;
    }
    return -1;
}

/* Punkt im (gedrehten) Rahmen? */
static BOOL inside(float x, float y)
{
    float c = cosf(-tang), s = sinf(-tang), dx = x - tcx, dy = y - tcy;
    float u = c * dx - s * dy, v = s * dx + c * dy;
    return fabsf(u) <= fabsf(tsx) * fw * 0.5f && fabsf(v) <= fabsf(tsy) * fh * 0.5f;
}

void transform_mouse_down(LONG mx, LONG my, BOOL shift)
{
    float x = scr2docx(mx), y = scr2docy(my);
    LONG h = hit_handle(mx, my);
    d_mx = x; d_my = y;
    d_cx = tcx; d_cy = tcy;
    d_sx = tsx; d_sy = tsy;
    d_ang = tang;
    if (h >= 0) {
        drag = DR_SCALE;
        drag_handle = h;
    } else if (inside(x, y))
        drag = DR_MOVE;
    else
        drag = DR_ROTATE;
    (void)shift;
}

/* liefert TRUE, wenn sich etwas geaendert hat (dann rendern) */
BOOL transform_mouse_move(LONG mx, LONG my, BOOL shift)
{
    float x = scr2docx(mx), y = scr2docy(my);
    if (drag == DR_NONE)
        return FALSE;
    if (drag == DR_MOVE) {
        tcx = d_cx + (x - d_mx);
        tcy = d_cy + (y - d_my);
    } else if (drag == DR_ROTATE) {
        float a0 = atan2f(d_my - d_cy, d_mx - d_cx), a1 = atan2f(y - d_cy, x - d_cx);
        tang = d_ang + (a1 - a0);
        if (shift) {
            float st = 3.14159265f / 12.0f;     /* 15 Grad */
            tang = floorf(tang / st + 0.5f) * st;
        }
    } else {
        /* Skalieren im lokalen (ungedrehten) System, Gegenseite bleibt fest */
        static const float hu[8] = { -1, 1, 1, -1, 0, 1, 0, -1 };
        static const float hv[8] = { -1, -1, 1, 1, -1, 0, 1, 0 };
        float c = cosf(d_ang), s = sinf(d_ang);
        float dx = x - d_cx, dy = y - d_cy;
        float lu = c * dx + s * dy, lv = -s * dx + c * dy;   /* Maus lokal */
        float hw = d_sx * fw * 0.5f, hh = d_sy * fh * 0.5f;  /* halbe Ausdehnung */
        float u0 = -hw, u1 = hw, v0 = -hh, v1 = hh, nsx = d_sx, nsy = d_sy, mu, mv;
        LONG h = drag_handle;
        if (hu[h] < 0) u0 = lu;
        if (hu[h] > 0) u1 = lu;
        if (hv[h] < 0) v0 = lv;
        if (hv[h] > 0) v1 = lv;
        if (hu[h] != 0)
            nsx = (u1 - u0) / fw;
        if (hv[h] != 0)
            nsy = (v1 - v0) / fh;
        if (shift && hu[h] != 0 && hv[h] != 0) {
            /* proportional: groessere relative Aenderung gewinnt */
            float rx = nsx / d_sx, ry = nsy / d_sy, r = fabsf(rx) > fabsf(ry) ? rx : ry;
            nsx = d_sx * r;
            nsy = d_sy * r;
            if (hu[h] < 0) u0 = u1 - nsx * fw; else u1 = u0 + nsx * fw;
            if (hv[h] < 0) v0 = v1 - nsy * fh; else v1 = v0 + nsy * fh;
        }
        if (fabsf(nsx * fw) < 1.0f) nsx = nsx < 0 ? -1.0f / fw : 1.0f / fw;
        if (fabsf(nsy * fh) < 1.0f) nsy = nsy < 0 ? -1.0f / fh : 1.0f / fh;
        tsx = nsx;
        tsy = nsy;
        /* neue Mitte: Mitte der lokalen Box, zurueckgedreht */
        mu = (u0 + u1) * 0.5f;
        mv = (v0 + v1) * 0.5f;
        tcx = d_cx + c * mu - s * mv;
        tcy = d_cy + s * mu + c * mv;
    }
    return TRUE;
}

void transform_mouse_up(void)
{
    if (drag == DR_NONE)
        return;
    drag = DR_NONE;
    render(TRUE);               /* nach dem Loslassen geglaettet */
}

void transform_update(BOOL smooth)
{
    if (tf_on)
        render(smooth);
}

void transform_get(float *sx, float *sy, float *ang)
{
    *sx = tsx;
    *sy = tsy;
    *ang = tang;
}
