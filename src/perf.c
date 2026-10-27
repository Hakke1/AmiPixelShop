/*
 * perf.c - "AmiPixelShop PERF": typische Ablaeufe auf 1920x1080 messen
 *
 * Laeuft mit echtem Bildschirm, schreibt die Zeiten nach RAM:perf.txt.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include "apx.h"

ULONG filter_perf(LONG cmd);

static BPTR pf;

static void out(const char *name, ULONG us, LONG n)
{
    char buf[100];
    if (n > 1)
        sprintf(buf, "%-34s %6ld.%ld ms  (je Schritt, %ld Schritte)\n", name,
                (long)(us / n / 1000), (long)(us / n / 100 % 10), (long)n);
    else
        sprintf(buf, "%-34s %6ld.%ld ms\n", name, (long)(us / 1000), (long)(us / 100 % 10));
    if (pf)
        Write(pf, buf, strlen(buf));
}

/* Ebene mit Inhalt fuellen: fn(x, y) liefert ARGB straight */
static void fill_layer(Layer *l, ULONG (*fn)(LONG x, LONG y))
{
    LONG tx, ty, x, y;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            Tile *t = l->tiles[ty * doc->tw + tx];
            if (!t && !(t = l->tiles[ty * doc->tw + tx] = tile_new()))
                continue;
            for (y = 0; y < TILE_SIZE; y++)
                for (x = 0; x < TILE_SIZE; x++) {
                    LONG gx = (tx << TILE_SHIFT) + x, gy = (ty << TILE_SHIFT) + y;
                    t->px[(y << TILE_SHIFT) | x] = gx < doc->w && gy < doc->h ? fn(gx, gy) : 0;
                }
            tile_update_flag(doc, t, tx, ty);
            if (t->flag == TF_EMPTY) {
                FreeVec(t);
                l->tiles[ty * doc->tw + tx] = NULL;
            }
        }
}

static ULONG seed = 1;
static ULONG photo(LONG x, LONG y)
{
    ULONG n;
    seed = seed * 1103515245 + 12345;
    n = (seed >> 16) & 15;
    return 0xFF000000 | (((x * 255 / 1920 + n) & 0xFF) << 16) | (((y * 255 / 1080 + n) & 0xFF) << 8) |
           (((x + y) / 12 + n) & 0xFF);
}
static ULONG blobs(LONG x, LONG y)
{
    LONG dx = (x % 400) - 200, dy = (y % 300) - 150;
    return dx * dx + dy * dy < 120 * 120 ? 0xB0E04020 : 0;
}
static ULONG band(LONG x, LONG y)
{
    return y > 400 && y < 700 ? 0x80305080 : 0;
}

void perf_run(void)
{
    Doc *d = doc_new(1920, 1080, DOC_WHITE);
    Layer *l1, *l2;
    ULONG t;
    LONG i;
    if (!d || !doc_open(d))
        return;
    pf = Open("RAM:perf.txt", MODE_NEWFILE);
    ui_draw_all();
    /* Hintergrund wie ein Foto, zwei Ebenen darueber (eine Multiplizieren) */
    fill_layer(doc->layers[0], photo);
    l1 = doc->layers[1];
    fill_layer(l1, blobs);
    doc_add_layer();
    l2 = doc->layers[doc->active];
    fill_layer(l2, band);
    l2->mode = BM_MULTIPLY;

    l1->visible = l2->visible = FALSE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit nur Hintergrund (deckend)", now_us() - t, 1);
    l1->visible = TRUE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit + Normal-Ebene (Kreise)", now_us() - t, 1);
    l1->visible = FALSE;
    l2->visible = TRUE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit + Multiply-Ebene (Band)", now_us() - t, 1);
    l1->visible = TRUE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit komplett (3 Ebenen)", now_us() - t, 1);

    view_set_zoom(1.0f, cvx + cvw / 2, cvy + cvh / 2);
    view_inval_all();
    t = now_us();
    view_flush();
    out("Anzeige 100% komplett", now_us() - t, 1);

    view_fit();
    view_inval_all();
    t = now_us();
    view_flush();
    out("Anzeige Fit komplett", now_us() - t, 1);

    t = now_us();
    for (i = 0; i < 10; i++) {
        view_pan(20, 10);
        view_flush();
    }
    out("Pan (Fit)", now_us() - t, 10);

    /* Pinselstrich interaktiv: je Schritt Tupfer + Komposit + Anzeige */
    doc->active = 1;
    brush_size = 60;
    brush_rebuild();
    t = now_us();
    stroke_begin(200.0f, 200.0f, FALSE);
    for (i = 1; i <= 40; i++) {
        stroke_to(200.0f + i * 30.0f, 200.0f + i * 12.0f);
        doc_process_dirty();
        view_flush();
    }
    out("Pinsel 60px (Tupfer+Komposit+Anzeige)", now_us() - t, 40);
    t = now_us();
    stroke_end();
    out("Pinsel Strichende (Undo)", now_us() - t, 1);

    /* Ebene ziehen: Teile einzeln */
    doc->active = 2;
    t = now_us();
    move_begin();
    out("  Ziehen Start (Kopien+Unterbau)", now_us() - t, 1);
    {
        ULONG tu = 0, tc = 0, td = 0, t0;
        for (i = 1; i <= 10; i++) {
            t0 = now_us();
            move_update(i * 7, i * 4);
            tu += now_us() - t0;
            t0 = now_us();
            doc_process_dirty();
            tc += now_us() - t0;
            t0 = now_us();
            view_flush();
            td += now_us() - t0;
        }
        out("  Ziehen: Ebene neu aufbauen", tu, 10);
        out("  Ziehen: Komposit", tc, 10);
        out("  Ziehen: Anzeige", td, 10);
        out("Ebene ziehen (Neuaufbau+Komposit+Anz.)", tu + tc + td, 10);
    }
    t = now_us();
    move_end();
    out("Ebene ziehen Ende", now_us() - t, 1);

    doc->active = 0;
    out("Filter Invert (Hintergrund)", filter_perf(CMD_ADJ_INVERT), 1);
    out("Filter Gaussian Blur 2px", filter_perf(CMD_FLT_GAUSS), 1);
    out("Filter Hue/Saturation (0)", filter_perf(CMD_ADJ_HUESAT), 1);
    t = now_us();
    doc_process_dirty();
    out("  danach Komposit", now_us() - t, 1);

    /* Korrekturebene Farbton */
    doc->active = doc->nlayers - 1;
    if (doc_new_adjust(CMD_ADJ_INVERT, NULL)) {
        Layer *a = doc->layers[doc->active];
        a->adj->cmd = CMD_ADJ_HUESAT;
        a->adj->v[0] = 40;
        doc_mark_all_dirty();
        t = now_us();
        doc_process_dirty();
        out("Komposit mit Hue/Sat-Korrekturebene", now_us() - t, 1);
        a->adj->cmd = CMD_ADJ_CURVES;
        doc_mark_all_dirty();
        t = now_us();
        doc_process_dirty();
        out("Komposit mit Curves-Korrekturebene", now_us() - t, 1);
    }

    /* Auswahl und Ameisen */
    t = now_us();
    sel_ellipse(100.0f, 100.0f, 1800.0f, 1000.0f, SEL_NEW);
    out("Auswahl Ellipse", now_us() - t, 1);
    t = now_us();
    view_ants_rebuild();
    out("Ameisen neu aufbauen", now_us() - t, 1);

    t = now_us();
    undo_step();
    doc_process_dirty();
    out("Undo + Komposit", now_us() - t, 1);

    if (pf)
        Close(pf);
    pf = 0;
}
