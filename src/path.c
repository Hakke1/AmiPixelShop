/*
 * path.c - Pen Tool und Arbeitspfad
 *
 * Ein Arbeitspfad je Dokument aus Teilpfaden kubischer Bezier-Segmente.
 * Ankerpunkt mit Griffen (absolut, Dokumentkoordinaten). Klick setzt eine
 * Ecke, Ziehen einen Kurvenpunkt (Griffe symmetrisch), Klick auf den ersten
 * Punkt schliesst. Vorhandene Punkte und Griffe lassen sich ziehen.
 * Aus dem Pfad: Auswahl, Fuellen (Vordergrundfarbe), Konturieren (Pinsel).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "apx.h"

static LONG pd_mode;            /* 0 nichts, 1 neuer Punkt (Griff ziehen), 2 Punkt, 3 Griff aus, 4 Griff ein */
static LONG pd_idx;             /* gezogener Punkt */
static float pd_dx, pd_dy;      /* Abstand Maus - Punkt beim Greifen */
static LONG sel_pt = -1;        /* zuletzt bearbeiteter Punkt (Griffe sichtbar) */
static UndoRec *pd_undo;        /* laufende Pfad-Aenderung */

static Path *get_path(BOOL create)
{
    if (!doc->wpath && create)
        doc->wpath = AllocVec(sizeof(Path), MEMF_ANY | MEMF_CLEAR);
    return doc->wpath;
}

BOOL path_exists(void)
{
    return doc->wpath && doc->wpath->npts > 0;
}

/* Teilpfad, zu dem Punkt i gehoert */
static LONG sub_of(Path *p, LONG i)
{
    LONG s;
    for (s = p->nsub - 1; s > 0 && p->sub[s] > i; s--)
        ;
    return s;
}

static LONG sub_end(Path *p, LONG s)
{
    return s + 1 < p->nsub ? p->sub[s + 1] : p->npts;
}

/* offener Teilpfad am Ende, an den angehaengt wird (-1: keiner) */
static LONG open_sub(Path *p)
{
    if (!p || !p->nsub || p->closed[p->nsub - 1] || !p->drawing)
        return -1;
    return p->nsub - 1;
}

/* ---- Abflachen ---- */

/* Teilpfad s als Polygon; Rueckgabe Anzahl Punkte (max. max) */
static LONG flatten(Path *p, LONG s, float *xs, float *ys, LONG max, BOOL close)
{
    LONG a = p->sub[s], e = sub_end(p, s), i, n = 0, segs = e - a - 1;
    if (e <= a)
        return 0;
    if (close && p->closed[s])
        segs++;
    if (n < max) { xs[n] = p->pt[a].x; ys[n] = p->pt[a].y; n++; }
    for (i = 0; i < segs; i++) {
        PathPt *q0 = &p->pt[a + i], *q1 = &p->pt[a + (i + 1) % (e - a)];
        float lx = q1->x - q0->x, ly = q1->y - q0->y, len = sqrtf(lx * lx + ly * ly);
        LONG k, steps;
        BOOL straight = q0->ox == q0->x && q0->oy == q0->y && q1->ix == q1->x && q1->iy == q1->y;
        steps = straight ? 1 : (LONG)(len / 3.0f) + 4;
        if (steps > 96)
            steps = 96;
        for (k = 1; k <= steps && n < max; k++) {
            float t = (float)k / steps, u = 1.0f - t;
            float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
            xs[n] = b0 * q0->x + b1 * q0->ox + b2 * q1->ix + b3 * q1->x;
            ys[n] = b0 * q0->y + b1 * q0->oy + b2 * q1->iy + b3 * q1->y;
            n++;
        }
    }
    return n;
}

#define MAXFLAT 16384
static float *fx_buf, *fy_buf;

static BOOL flat_bufs(void)
{
    if (!fx_buf)
        fx_buf = AllocVec(MAXFLAT * sizeof(float), MEMF_ANY);
    if (!fy_buf)
        fy_buf = AllocVec(MAXFLAT * sizeof(float), MEMF_ANY);
    return fx_buf && fy_buf;
}

void path_free_bufs(void)
{
    if (fx_buf) FreeVec(fx_buf);
    if (fy_buf) FreeVec(fy_buf);
    fx_buf = fy_buf = NULL;
}

/* ---- Anzeige ---- */

static void path_overlay(void)
{
    Path *p = doc->wpath;
    LONG s, i;
    if (!p || !flat_bufs())
        return;
    for (s = 0; s < p->nsub; s++) {
        LONG n = flatten(p, s, fx_buf, fy_buf, MAXFLAT, TRUE), a = p->sub[s], e = sub_end(p, s);
        for (i = 1; i < n; i++) {
            LONG x0 = doc2scrx(fx_buf[i - 1]), y0 = doc2scry(fy_buf[i - 1]);
            LONG x1 = doc2scrx(fx_buf[i]), y1 = doc2scry(fy_buf[i]);
            line(x0 + 1, y0 + 1, x1 + 1, y1 + 1, 0xFF000000);
            line(x0, y0, x1, y1, 0xFF3A8EF0);
        }
        for (i = a; i < e; i++) {
            PathPt *q = &p->pt[i];
            LONG x = doc2scrx(q->x), y = doc2scry(q->y);
            if (i == sel_pt || (i == e - 1 && open_sub(p) == s)) {
                /* Griffe des aktuellen Punkts */
                LONG hx, hy;
                if (q->ox != q->x || q->oy != q->y) {
                    hx = doc2scrx(q->ox); hy = doc2scry(q->oy);
                    line(x, y, hx, hy, 0xFF3A8EF0);
                    fill(hx - 2, hy - 2, 5, 5, 0xFF3A8EF0);
                }
                if (q->ix != q->x || q->iy != q->y) {
                    hx = doc2scrx(q->ix); hy = doc2scry(q->iy);
                    line(x, y, hx, hy, 0xFF3A8EF0);
                    fill(hx - 2, hy - 2, 5, 5, 0xFF3A8EF0);
                }
                fill(x - 3, y - 3, 7, 7, 0xFF3A8EF0);
            } else {
                fill(x - 3, y - 3, 7, 7, 0xFFFFFFFF);
                frame(x - 3, y - 3, 7, 7, 0xFF3A8EF0);
            }
        }
    }
}

void path_show(BOOL on)
{
    view_pen_overlay = on ? path_overlay : NULL;
    view_inval_all();
}

/* ---- Bearbeiten mit der Maus (Bildschirmkoordinaten) ---- */

static BOOL near(float dx, float dy, LONG sx, LONG sy)
{
    LONG x = doc2scrx(dx), y = doc2scry(dy);
    return sx >= x - 6 && sx <= x + 6 && sy >= y - 6 && sy <= y + 6;
}

void path_mouse_down(LONG sx, LONG sy)
{
    Path *p;
    float dx = scr2docx(sx), dy = scr2docy(sy);
    LONG i, s;
    if (pd_undo)
        undo_commit(pd_undo);
    pd_undo = undo_begin_path("Pen");     /* Zustand vor dem Klick */
    if (!(p = get_path(TRUE)))
        return;
    if (sel_pt >= p->npts)
        sel_pt = -1;
    /* Griffe des aktuellen Punkts */
    if (sel_pt >= 0 && sel_pt < p->npts) {
        PathPt *q = &p->pt[sel_pt];
        if ((q->ox != q->x || q->oy != q->y) && near(q->ox, q->oy, sx, sy)) {
            pd_mode = 3; pd_idx = sel_pt;
            if (pd_undo)
                strcpy(pd_undo->name, "Drag Direction Point");
            return;
        }
        if ((q->ix != q->x || q->iy != q->y) && near(q->ix, q->iy, sx, sy)) {
            pd_mode = 4; pd_idx = sel_pt;
            if (pd_undo)
                strcpy(pd_undo->name, "Drag Direction Point");
            return;
        }
    }
    /* Ankerpunkte */
    for (i = 0; i < p->npts; i++)
        if (near(p->pt[i].x, p->pt[i].y, sx, sy)) {
            s = sub_of(p, i);
            if (open_sub(p) == s && i == p->sub[s] && p->npts - p->sub[s] >= 2) {
                p->closed[s] = 1;           /* schliessen */
                p->drawing = FALSE;
                sel_pt = i;
                pd_mode = 0;
                if (pd_undo)
                    strcpy(pd_undo->name, "Close Path");
            } else {
                pd_mode = 2;
                if (pd_undo)
                    strcpy(pd_undo->name, "Drag Anchor Point");
                pd_idx = sel_pt = i;
                pd_dx = dx - p->pt[i].x;
                pd_dy = dy - p->pt[i].y;
            }
            view_inval_all();
            return;
        }
    /* neuer Punkt */
    if (p->npts >= PATH_MAXPTS)
        return;
    if (open_sub(p) < 0) {
        if (p->nsub >= PATH_MAXSUB)
            return;
        p->sub[p->nsub] = p->npts;
        p->closed[p->nsub] = 0;
        p->nsub++;
        p->drawing = TRUE;
    }
    i = p->npts++;
    if (pd_undo)
        strcpy(pd_undo->name, "Add Anchor Point");
    p->pt[i].x = p->pt[i].ix = p->pt[i].ox = dx;
    p->pt[i].y = p->pt[i].iy = p->pt[i].oy = dy;
    pd_mode = 1;
    pd_idx = sel_pt = i;
    doc->modified = TRUE;
    view_inval_all();
}

void path_mouse_move(LONG sx, LONG sy)
{
    Path *p = doc->wpath;
    float dx = scr2docx(sx), dy = scr2docy(sy);
    PathPt *q;
    if (!p || !pd_mode || pd_idx >= p->npts)
        return;
    q = &p->pt[pd_idx];
    switch (pd_mode) {
    case 1:                     /* Kurvenpunkt aufziehen: Griffe symmetrisch */
    case 3:
        q->ox = dx; q->oy = dy;
        q->ix = 2 * q->x - dx; q->iy = 2 * q->y - dy;
        break;
    case 4:
        q->ix = dx; q->iy = dy;
        q->ox = 2 * q->x - dx; q->oy = 2 * q->y - dy;
        break;
    case 2: {
        float nx = dx - pd_dx, ny = dy - pd_dy, mx = nx - q->x, my = ny - q->y;
        q->x = nx; q->y = ny;
        q->ix += mx; q->iy += my; q->ox += mx; q->oy += my;
        break; }
    }
    view_inval_all();
}

void path_mouse_up(LONG sx, LONG sy)
{
    Path *p = doc->wpath;
    path_mouse_move(sx, sy);
    if (p && pd_mode == 1 && pd_idx < p->npts) {
        /* kaum gezogen: Ecke */
        PathPt *q = &p->pt[pd_idx];
        if (near(q->ox, q->oy, doc2scrx(q->x), doc2scry(q->y))) {
            q->ox = q->ix = q->x;
            q->oy = q->iy = q->y;
        }
    }
    pd_mode = 0;
    if (pd_undo) {
        undo_commit(pd_undo);   /* verwirft sich selbst, wenn nichts geaendert */
        pd_undo = NULL;
    }
    view_inval_all();
}

/* Enter/Esc: Pfad beenden (naechster Klick beginnt einen neuen Teilpfad) */
void path_end(void)
{
    if (doc->wpath)
        doc->wpath->drawing = FALSE;
    sel_pt = -1;
    view_inval_all();
}

/* Backspace: letzten Punkt des offenen Teilpfads entfernen */
void path_delete_last(void)
{
    Path *p = doc->wpath;
    LONG s = open_sub(p);
    UndoRec *u;
    if (s < 0)
        return;
    u = undo_begin_path("Delete Anchor Point");
    p->npts--;
    if (p->npts <= p->sub[s]) {
        p->nsub--;
        p->drawing = FALSE;
    }
    sel_pt = p->npts - 1;
    undo_commit(u);
    view_inval_all();
}

void path_delete(void)
{
    if (doc->wpath) {
        UndoRec *u = undo_begin_path("Delete Path");
        if (u) {
            if (u->path)
                FreeVec(u->path);   /* Kopie unnoetig: */
            u->path = doc->wpath;   /* der Pfad selbst gehoert jetzt dem Undo */
            doc->wpath = NULL;
            undo_commit(u);
        } else {
            FreeVec(doc->wpath);
            doc->wpath = NULL;
        }
    }
    sel_pt = -1;
    view_inval_all();
}

/* ---- Pfad verwenden ---- */

/* Auswahl aus allen Teilpfaden (geschlossen gedacht) */
BOOL path_to_selection(void)
{
    Path *p = doc->wpath;
    LONG s, first = TRUE;
    if (!p || !flat_bufs())
        return FALSE;
    for (s = 0; s < p->nsub; s++) {
        LONG n;
        BOOL c = p->closed[s];
        p->closed[s] = 1;
        n = flatten(p, s, fx_buf, fy_buf, MAXFLAT, TRUE);
        p->closed[s] = c;
        if (n < 3)
            continue;
        sel_polygon(fx_buf, fy_buf, n, first ? SEL_NEW : SEL_ADD);
        first = FALSE;
    }
    return !first;
}

/* Fuellen mit der Vordergrundfarbe (ueber eine voruebergehende Auswahl) */
void path_fill(void)
{
    LONG n = doc->w * doc->h;
    UBYTE *save = AllocVec(n, MEMF_ANY);
    BOOL had = doc->hassel;
    LONG b[4];
    if (!save)
        return;
    CopyMem(doc->sel, save, n);
    b[0] = doc->selx0; b[1] = doc->sely0; b[2] = doc->selx1; b[3] = doc->sely1;
    if (path_to_selection())
        fill_selection_fg("Fill Path");
    CopyMem(save, doc->sel, n);
    doc->hassel = had;
    doc->selx0 = b[0]; doc->sely0 = b[1]; doc->selx1 = b[2]; doc->sely1 = b[3];
    FreeVec(save);
}

/* Kontur mit dem Pinsel entlang des Pfads */
void path_stroke(void)
{
    Path *p = doc->wpath;
    LONG s, i;
    if (!p || !flat_bufs())
        return;
    for (s = 0; s < p->nsub; s++) {
        LONG n = flatten(p, s, fx_buf, fy_buf, MAXFLAT, TRUE);
        if (n < 1)
            continue;
        stroke_begin(fx_buf[0], fy_buf[0], FALSE);
        for (i = 1; i < n; i++)
            stroke_to(fx_buf[i], fy_buf[i]);
        stroke_end();
        if (doc->ucur)
            strcpy(doc->ucur->name, "Stroke Path");
        if (s > 0 && doc->ucur)
            doc->ucur->chain = TRUE;    /* alle Teilpfade ein Schritt */
    }
}

/* Miniatur des Pfads (Paths-Panel), Flaeche w x h am Schirm */
void path_draw_thumb(LONG x, LONG y, LONG w, LONG h, ULONG c)
{
    Path *p = doc->wpath;
    LONG s, i;
    float k;
    if (!p || !flat_bufs())
        return;
    k = (float)w / doc->w;
    if ((float)h / doc->h < k)
        k = (float)h / doc->h;
    x += (LONG)((w - doc->w * k) * 0.5f);
    y += (LONG)((h - doc->h * k) * 0.5f);
    for (s = 0; s < p->nsub; s++) {
        LONG n = flatten(p, s, fx_buf, fy_buf, MAXFLAT, TRUE);
        for (i = 1; i < n; i++)
            line(x + (LONG)(fx_buf[i - 1] * k), y + (LONG)(fy_buf[i - 1] * k),
                 x + (LONG)(fx_buf[i] * k), y + (LONG)(fy_buf[i] * k), c);
    }
}
