/*
 * crop.c - Freistellungswerkzeug
 *
 * Rahmen in Dokumentkoordinaten (darf ueber die Flaeche hinausgehen: dann
 * wird sie erweitert). Ausserhalb wird abgedunkelt, Drittel-Raster im
 * Rahmen. Angewendet wird ueber doc_canvas_rect: Abgeschnittenes bleibt als
 * Ueberhang der Ebenen erhalten (wie "Delete Cropped Pixels" aus).
 */
#include <exec/types.h>
#include <proto/exec.h>
#include "apx.h"

static BOOL cr_on;
static float cr_x0, cr_y0, cr_x1, cr_y1;
static LONG cr_drag;            /* 0 nichts, 1 neu, 2 verschieben, 3.. Griff */
static float cr_ax, cr_ay;      /* Anker beim Ziehen (Dokument) */
static float cr_sx0, cr_sy0, cr_sx1, cr_sy1;
static BOOL cr_fresh, cr_sfresh;  /* Rahmen noch der Start-Rahmen ueber dem ganzen Bild */

/* Griffe: 0..7 = oben links, oben, oben rechts, rechts, unten rechts, unten, unten links, links */
static void handle_pos(LONG i, LONG *x, LONG *y)
{
    LONG l = doc2scrx(cr_x0), r = doc2scrx(cr_x1), t = doc2scry(cr_y0), b = doc2scry(cr_y1);
    static const BYTE hx[8] = { 0, 1, 2, 2, 2, 1, 0, 0 }, hy[8] = { 0, 0, 0, 1, 2, 2, 2, 1 };
    *x = hx[i] == 0 ? l : hx[i] == 1 ? (l + r) / 2 : r;
    *y = hy[i] == 0 ? t : hy[i] == 1 ? (t + b) / 2 : b;
}

static void crop_overlay(void)
{
    LONG l = doc2scrx(cr_x0), r = doc2scrx(cr_x1), t = doc2scry(cr_y0), b = doc2scry(cr_y1);
    LONG x, y, i, ix0, iy0, ix1, iy1;
    /* ausserhalb abdunkeln (nur das Bild) */
    ix0 = doc2scrx(0); iy0 = doc2scry(0); ix1 = doc2scrx(doc->w); iy1 = doc2scry(doc->h);
    if (ix0 < cvx) ix0 = cvx;
    if (iy0 < cvy) iy0 = cvy;
    if (ix1 > cvx + cvw) ix1 = cvx + cvw;
    if (iy1 > cvy + cvh) iy1 = cvy + cvh;
    for (y = iy0; y < iy1; y++) {
        ULONG *p = bb + y * SW;
        BOOL in_y = y >= t && y < b;
        for (x = ix0; x < ix1; x++)
            if (!in_y || x < l || x >= r)
                p[x] = 0xFF000000 | ((p[x] >> 1) & 0x7F7F7F);
    }
    /* Drittel-Raster */
    for (i = 1; i < 3; i++) {
        LONG gx = l + (r - l) * i / 3, gy = t + (b - t) * i / 3;
        line(gx, t, gx, b - 1, 0xFF9A9A9A);
        line(l, gy, r - 1, gy, 0xFF9A9A9A);
    }
    frame(l - 1, t - 1, r - l + 2, b - t + 2, 0xFF000000);
    frame(l, t, r - l, b - t, 0xFFFFFFFF);
    for (i = 0; i < 8; i++) {
        LONG hx, hy;
        handle_pos(i, &hx, &hy);
        fill(hx - 3, hy - 3, 7, 7, 0xFFFFFFFF);
        frame(hx - 4, hy - 4, 9, 9, 0xFF000000);
    }
}

BOOL crop_active(void)
{
    return cr_on;
}

void crop_begin(void)
{
    cr_x0 = cr_y0 = 0.0f;
    cr_x1 = doc->w;
    cr_y1 = doc->h;
    cr_on = TRUE;
    cr_fresh = TRUE;
    cr_drag = 0;
    view_transform_overlay = crop_overlay;
    view_inval_all();
}

void crop_end(void)
{
    if (!cr_on)
        return;
    cr_on = FALSE;
    view_transform_overlay = NULL;
    view_inval_all();
}

void crop_get(LONG *w, LONG *h)
{
    *w = (LONG)floorf(cr_x1 + 0.5f) - (LONG)floorf(cr_x0 + 0.5f);
    *h = (LONG)floorf(cr_y1 + 0.5f) - (LONG)floorf(cr_y0 + 0.5f);
}

void crop_mouse_down(LONG sx, LONG sy)
{
    LONG i;
    float dx = scr2docx(sx), dy = scr2docy(sy);
    if (!cr_on)
        crop_begin();
    cr_sx0 = cr_x0; cr_sy0 = cr_y0; cr_sx1 = cr_x1; cr_sy1 = cr_y1;
    cr_sfresh = cr_fresh;
    cr_ax = dx;
    cr_ay = dy;
    for (i = 0; i < 8; i++) {
        LONG hx, hy;
        handle_pos(i, &hx, &hy);
        if (sx >= hx - 7 && sx <= hx + 7 && sy >= hy - 7 && sy <= hy + 7) {
            cr_drag = 3 + i;
            return;
        }
    }
    /* im unveraenderten Start-Rahmen zieht man wie in PS einen neuen auf */
    if (!cr_fresh && dx >= cr_x0 && dx < cr_x1 && dy >= cr_y0 && dy < cr_y1)
        cr_drag = 2;
    else {
        cr_drag = 1;
        cr_x0 = cr_x1 = floorf(dx + 0.5f);
        cr_y0 = cr_y1 = floorf(dy + 0.5f);
    }
}

void crop_mouse_move(LONG sx, LONG sy)
{
    float dx = floorf(scr2docx(sx) + 0.5f), dy = floorf(scr2docy(sy) + 0.5f), t;
    float mx = floorf(scr2docx(sx) - cr_ax + 0.5f), my = floorf(scr2docy(sy) - cr_ay + 0.5f);
    switch (cr_drag) {
    case 0:
        return;
    case 1:                     /* neuer Rahmen von Anker zur Maus */
        cr_x0 = floorf(cr_ax + 0.5f); cr_y0 = floorf(cr_ay + 0.5f);
        cr_x1 = dx; cr_y1 = dy;
        break;
    case 2:
        cr_x0 = cr_sx0 + mx; cr_x1 = cr_sx1 + mx;
        cr_y0 = cr_sy0 + my; cr_y1 = cr_sy1 + my;
        break;
    default: {
        LONG h = cr_drag - 3;
        if (h == 0 || h == 6 || h == 7) cr_x0 = cr_sx0 + mx;
        if (h == 2 || h == 3 || h == 4) cr_x1 = cr_sx1 + mx;
        if (h == 0 || h == 1 || h == 2) cr_y0 = cr_sy0 + my;
        if (h == 4 || h == 5 || h == 6) cr_y1 = cr_sy1 + my;
        break; }
    }
    if (cr_drag == 1 || cr_drag >= 3) {
        /* Ecken getauscht? ordnen (ganzzahlig vergleichen: 68881 rechnet 80 Bit) */
        if ((LONG)cr_x1 < (LONG)cr_x0) { t = cr_x0; cr_x0 = cr_x1; cr_x1 = t; }
        if ((LONG)cr_y1 < (LONG)cr_y0) { t = cr_y0; cr_y0 = cr_y1; cr_y1 = t; }
    }
    view_inval_all();
}

void crop_mouse_up(LONG sx, LONG sy)
{
    crop_mouse_move(sx, sy);
    if ((LONG)(cr_x1 - cr_x0) < 1 || (LONG)(cr_y1 - cr_y0) < 1) {
        cr_x0 = cr_sx0; cr_y0 = cr_sy0; cr_x1 = cr_sx1; cr_y1 = cr_sy1;
        cr_fresh = cr_sfresh;
        view_inval_all();
    } else if (cr_drag)
        cr_fresh = FALSE;
    cr_drag = 0;
}

/* anwenden; danach bleibt das Werkzeug mit neuem Rahmen aktiv */
BOOL crop_commit(void)
{
    LONG x0 = (LONG)floorf(cr_x0 + 0.5f), y0 = (LONG)floorf(cr_y0 + 0.5f);
    LONG x1 = (LONG)floorf(cr_x1 + 0.5f), y1 = (LONG)floorf(cr_y1 + 0.5f);
    BOOL ok;
    if (!cr_on)
        return FALSE;
    crop_end();
    ok = doc_canvas_rect(x0, y0, x1, y1);
    crop_begin();
    return ok;
}
