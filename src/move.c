/*
 * move.c - Verschieben-Werkzeug
 *
 * Ohne Auswahl: die ganze aktive Ebene wird verschoben; bei einer Gruppe alle
 *               Pixelebenen darin (je Ebene ein Zustand in slot[], Undo als
 *               verkettete Eintraege, Textpositionen ueber einen Struktur-Eintrag).
 * Mit Auswahl:  die ausgewaehlten Pixel werden herausgeloest ("schwebend"),
 *               es bleibt ein Loch, und die Auswahl wandert mit.
 * Waehrend des Ziehens wird die Ebene immer aus den Originalkacheln neu
 * aufgebaut; am Ende wandern die Originale ins Undo.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "apx.h"

static BOOL mv_active, mv_float;
static Layer *mv_layer;
static Tile **mv_orig;          /* Kopien aller Kacheln vor dem Verschieben */
static Tile **mv_base;          /* Ebene mit Loch (nur bei Auswahl) */
static UBYTE *mv_base_own;      /* mv_base[i] gehoert uns (sonst = mv_orig[i]) */
static UBYTE *mv_touched;       /* Kachel wurde veraendert */
static ULONG *fpx;              /* schwebende Pixel (straight ARGB) */
static UBYTE *fsel;             /* Auswahlmaske dazu */
static LONG fx0, fy0, fw, fh;   /* Lage beim Start */
static LONG cur_dx, cur_dy;
static UndoRec *mv_text_undo;   /* Textebene: Position im Undo mitfuehren */
static Ovf *mv_ovf;             /* Ueberhang vor dem Verschieben (gehoert weiter der Ebene) */

/* Gruppe: Zustand je Ebene; mv_layer/mv_orig/... zeigen auf den gerade bearbeiteten */
static struct { Layer *l; Tile **orig; UBYTE *touched; Ovf *ovf; } slot[MAX_LAYERS];
static LONG nslot;
static BOOL mv_group;
static BOOL mv_first;            /* erster Schritt: uebernommene Kacheln neu zeichnen */
static BOOL mv_stolen, mv_shifted;  /* Originale uebernommen statt kopiert / schon verschoben */

static void use_slot(LONG k)
{
    mv_layer = slot[k].l;
    mv_orig = slot[k].orig;
    mv_touched = slot[k].touched;
    mv_ovf = slot[k].ovf;
}

BOOL move_active(void)
{
    return mv_active;
}

static LONG ntiles(void)
{
    return doc->tw * doc->th;
}

/* n Pixel ab (sx,sy) aus einer Kachelmenge lesen, ausserhalb = 0 */
static void fetch_row(Tile **src, LONG sx, LONG sy, LONG n, ULONG *dst)
{
    if (sy < 0 || sy >= doc->h) {
        memset(dst, 0, n * 4);
        return;
    }
    while (n > 0) {
        LONG k;
        if (sx < 0) {
            k = -sx < n ? -sx : n;
            memset(dst, 0, k * 4);
        } else if (sx >= doc->w) {
            memset(dst, 0, n * 4);
            return;
        } else {
            Tile *t = src[(sy >> TILE_SHIFT) * doc->tw + (sx >> TILE_SHIFT)];
            LONG off = sx & TILE_MASK;
            k = TILE_SIZE - off;
            if (k > n) k = n;
            if (sx + k > doc->w) k = doc->w - sx;
            if (t)
                memcpy(dst, t->px + ((sy & TILE_MASK) << TILE_SHIFT) + off, k * 4);
            else
                memset(dst, 0, k * 4);
        }
        dst += k;
        sx += k;
        n -= k;
    }
}

/* Teile der Zeile ausserhalb der Arbeitsflaeche aus dem alten Ueberhang */
static void fetch_ovf(LONG sx, LONG sy, LONG n, ULONG *dst)
{
    LONG i, a, b;
    if (!mv_ovf)
        return;
    if (sy < 0 || sy >= doc->h) {
        a = n;                  /* ganze Zeile draussen */
        b = 0;
    } else {
        a = -sx < n ? -sx : n;  /* links draussen: 0..a */
        b = doc->w - sx > 0 ? doc->w - sx : 0;  /* rechts draussen: b..n */
    }
    for (i = 0; i < a; i++)
        dst[i] = ovf_get(mv_ovf, sx + i, sy);
    for (i = b > a ? b : a; i < n; i++)
        dst[i] = ovf_get(mv_ovf, sx + i, sy);
}

/* Inhalt vor dem Verschieben (Flaeche + Ueberhang) */
static ULONG src_get(LONG x, LONG y)
{
    if (x >= 0 && y >= 0 && x < doc->w && y < doc->h) {
        Tile *t = mv_orig[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
        return t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
    }
    return mv_ovf ? ovf_get(mv_ovf, x, y) : 0;
}

static ULONG shifted_get(LONG x, LONG y)
{
    return src_get(x - cur_dx, y - cur_dy);
}

/* straight-alpha src-over */
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

/* Ziel-Kachel bereitstellen (oder NULL bei Speichermangel) */
static Tile *dest_tile(LONG idx)
{
    Tile *t = mv_layer->tiles[idx];
    if (!t && (t = tile_new()))
        mv_layer->tiles[idx] = t;
    return t;
}

static void finish_tile(LONG idx)
{
    Tile *t = mv_layer->tiles[idx];
    LONG tx = idx % doc->tw, ty = idx / doc->tw;
    if (t) {
        tile_update_flag(doc, t, tx, ty);
        if (t->flag == TF_EMPTY) {
            FreeVec(t);
            mv_layer->tiles[idx] = NULL;
        }
    }
    mv_touched[idx] = 1;
    doc_mark_dirty(tx, ty);
}

/* ---- Maske mitverschieben (verknuepft) oder allein (Kette aus, Maske aktiv) ---- */

static Layer *mm_layer;         /* Ebene, deren Maske bewegt wird (NULL: keine) */
static UBYTE *mm_snap;          /* Maske vor dem Verschieben als Grau, w*h */
static Tile **mm_orig;          /* Original-Kacheln (gehoeren ans Undo, falls verschoben) */
static UBYTE mm_def;            /* Wert fuer neu aufgedeckte Bereiche */
static BOOL mm_moved, mm_only;

static void mm_begin(Layer *l)
{
    LONG w = doc->w, h = doc->h, n = ntiles(), x, y, white = 0, cnt = 0;
    mm_layer = NULL;
    mm_moved = FALSE;
    if (!l || !l->mask)
        return;
    mm_snap = AllocVec(w * h, MEMF_ANY);
    mm_orig = AllocVec(n * sizeof(Tile *), MEMF_ANY);
    if (!mm_snap || !mm_orig) {
        if (mm_snap) FreeVec(mm_snap);
        if (mm_orig) FreeVec(mm_orig);
        mm_snap = NULL;
        mm_orig = NULL;
        return;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            Tile *t = l->mask[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
            UBYTE v = t ? (UBYTE)t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 255;
            mm_snap[y * w + x] = v;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                cnt++;
                white += v >= 128;
            }
        }
    /* aussen gilt, was am Rand ueberwiegt (Photoshop: Maske laeuft weiter) */
    mm_def = white * 2 >= cnt ? 255 : 0;
    CopyMem(l->mask, mm_orig, n * sizeof(Tile *));
    mm_layer = l;
}

static void mm_shift(LONG dx, LONG dy)
{
    LONG w = doc->w, h = doc->h, tx, ty, x, y;
    Layer *l = mm_layer;
    if (!l)
        return;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            LONG idx = ty * doc->tw + tx;
            Tile *t = l->mask[idx];
            if (!mm_moved || t == mm_orig[idx]) {
                if (!(t = tile_new()))
                    continue;
                l->mask[idx] = t;
            }
            for (y = 0; y < TILE_SIZE; y++) {
                LONG sy = (ty << TILE_SHIFT) + y - dy;
                ULONG *o = t->px + (y << TILE_SHIFT);
                for (x = 0; x < TILE_SIZE; x++) {
                    LONG sx = (tx << TILE_SHIFT) + x - dx;
                    ULONG v = sx >= 0 && sy >= 0 && sx < w && sy < h ? mm_snap[sy * w + sx] : mm_def;
                    o[x] = 0xFF000000 | (v * 0x010101);
                }
            }
            doc_mark_dirty(tx, ty);
        }
    mm_moved = TRUE;
}

/* abschliessen: Undo-Eintrag fuer die Maske (chain: mit dem vorigen zusammen) */
static void mm_end(BOOL chain)
{
    LONG i, n = ntiles();
    if (mm_layer && mm_moved) {
        UndoRec *u = undo_begin(mm_layer, "Move");
        if (u) {
            u->on_mask = TRUE;
            u->chain = chain;
        }
        for (i = 0; i < n; i++) {
            tile_update_flag(doc, mm_layer->mask[i], i % doc->tw, i / doc->tw);
            if (u)
                undo_add(u, i, mm_orig[i]);
            else if (mm_orig[i])
                FreeVec(mm_orig[i]);
        }
        undo_commit(u);
    }
    if (mm_snap) FreeVec(mm_snap);
    if (mm_orig) FreeVec(mm_orig);
    mm_snap = NULL;
    mm_orig = NULL;
    mm_layer = NULL;
    mm_moved = FALSE;
}

static void free_state(void)
{
    LONG i, n = ntiles();
    for (i = 0; i < n; i++) {
        if (mv_base && mv_base_own[i] && mv_base[i])
            FreeVec(mv_base[i]);
        if (mv_orig[i])
            FreeVec(mv_orig[i]);
    }
    FreeVec(mv_orig);
    if (mv_base) FreeVec(mv_base);
    if (mv_base_own) FreeVec(mv_base_own);
    if (mv_touched) FreeVec(mv_touched);
    if (fpx) FreeVec(fpx);
    if (fsel) FreeVec(fsel);
    mv_orig = mv_base = NULL;
    mv_base_own = mv_touched = NULL;
    fpx = NULL;
    fsel = NULL;
    mv_active = FALSE;
}

void move_begin(void)
{
    LONG i, n = ntiles(), x, y;
    if (mv_active)
        return;
    mv_group = FALSE;
    mm_only = FALSE;
    {
        Layer *al = doc->layers[doc->active];
        if (al->mask && al->mask_unlink && doc->edit_mask && !doc->hassel && doc_picked_one()) {
            /* Kette aus, Maske aktiv: nur die Maske */
            mm_begin(al);
            if (mm_layer) {
                mm_only = TRUE;
                mv_active = TRUE;
                cur_dx = cur_dy = 0;
            }
            return;
        }
    }
    {
    LONG pk[MAX_LAYERS], np = doc_picked(pk), low = doc->nlayers, q;
    if (np > 1 || doc->layers[doc->active]->kind == LK_GROUP) {
        /* Gruppe oder mehrere ausgewaehlte Ebenen: alle Pixelebenen darin */
        LONG k;
        if (doc->hassel) {
            status_message("Select a layer to move selected pixels");
            return;
        }
        nslot = 0;
        for (q = 0; q < np; q++)
        for (k = pk[q] - layer_block(pk[q]) + 1; k <= pk[q]; k++) {
            Layer *l = doc->layers[k];
            if (k < low)
                low = k;
            if (l->kind != LK_PIXEL || (l->lock & LOCK_POS))
                continue;
            slot[nslot].l = l;
            slot[nslot].ovf = l->ovf;
            slot[nslot].orig = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
            slot[nslot].touched = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
            nslot++;
            if (!slot[nslot - 1].orig || !slot[nslot - 1].touched)
                goto gfail;
            /* Originale uebernehmen (nur gelesen), Ebene wird beim Verschieben neu gefuellt */
            for (i = 0; i < n; i++)
                if (l->tiles[i]) {
                    slot[nslot - 1].orig[i] = l->tiles[i];
                    slot[nslot - 1].touched[i] = 1;
                    l->tiles[i] = NULL;
                }
        }
        if (!nslot)
            return;
        mv_group = TRUE;
        mv_stolen = TRUE;
        mv_shifted = FALSE;
        mv_float = FALSE;
        mv_text_undo = NULL;
        cur_dx = cur_dy = 0;
        mv_active = TRUE;
        doc_cache_below(low);
        if (np == 1 && !doc->layers[doc->active]->mask_unlink)
            mm_begin(doc->layers[doc->active]);     /* Gruppenmaske */
        return;
gfail:
        while (nslot-- > 0) {
            use_slot(nslot);
            if (mv_orig) {
                for (i = 0; i < n; i++)
                    if (mv_orig[i])
                        mv_layer->tiles[i] = mv_orig[i];    /* uebernommene zurueck */
                FreeVec(mv_orig);
            }
            if (mv_touched)
                FreeVec(mv_touched);
        }
        nslot = 0;
        mv_orig = NULL;
        mv_touched = NULL;
        status_message("Not enough memory");
        return;
    }
    }
    mv_layer = doc->layers[doc->active];
    mv_float = doc->hassel;
    mv_text_undo = NULL;
    if (mv_layer->text) {
        if (mv_float)
            doc_rasterize_text(mv_layer);   /* Teil eines Textes: wird zu Pixeln */
        else
            mv_text_undo = undo_begin_struct("Move");
    }
    cur_dx = cur_dy = 0;
    mv_ovf = mv_layer->ovf;
    mv_orig = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    mv_touched = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    if (!mv_orig || !mv_touched)
        goto fail;
    mv_stolen = !mv_float;
    mv_shifted = FALSE;
    for (i = 0; i < n; i++)
        if (mv_layer->tiles[i]) {
            if (mv_stolen) {
                /* nur gelesen: uebernehmen statt kopieren (spart 8 MB bei 1080p) */
                mv_orig[i] = mv_layer->tiles[i];
                mv_touched[i] = 1;
                mv_layer->tiles[i] = NULL;
            } else if (!(mv_orig[i] = tile_clone(mv_layer->tiles[i])))
                goto fail;
        }
    mv_active = TRUE;
    doc_cache_below(doc->active);
    if (!mv_float && !mv_layer->mask_unlink)
        mm_begin(mv_layer);

    if (mv_float) {
        fx0 = doc->selx0; fy0 = doc->sely0;
        fw = doc->selx1 - fx0; fh = doc->sely1 - fy0;
        mv_base = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
        mv_base_own = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
        fpx = AllocVec(fw * fh * 4, MEMF_ANY);
        fsel = AllocVec(fw * fh, MEMF_ANY);
        if (!mv_base || !mv_base_own || !fpx || !fsel)
            goto fail;
        for (i = 0; i < n; i++)
            mv_base[i] = mv_orig[i];
        /* schwebende Pixel herausloesen, Loch in mv_base stanzen */
        for (y = 0; y < fh; y++) {
            const UBYTE *s = doc->sel + (fy0 + y) * doc->w + fx0;
            memcpy(fsel + y * fw, s, fw);
            for (x = 0; x < fw; x++) {
                LONG gx = fx0 + x, gy = fy0 + y;
                LONG idx = (gy >> TILE_SHIFT) * doc->tw + (gx >> TILE_SHIFT);
                ULONG p = layer_get(mv_layer, gx, gy), a = p >> 24, sa = s[x];
                fpx[y * fw + x] = (((a * sa * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
                if (!sa || !a)
                    continue;
                if (!mv_base_own[idx]) {
                    if (!(mv_base[idx] = tile_clone(mv_orig[idx]))) {
                        mv_base[idx] = mv_orig[idx];
                        continue;
                    }
                    mv_base_own[idx] = 1;
                }
                {
                    ULONG *q = &mv_base[idx]->px[((gy & TILE_MASK) << TILE_SHIFT) | (gx & TILE_MASK)];
                    *q = ((((*q >> 24) * (255 - sa) * 257 + 0x8080) >> 16) << 24) | (*q & 0xFFFFFF);
                }
            }
        }
    }
    return;
fail:
    doc_cache_drop();
    if (mv_orig)
        free_state();
    else if (mv_touched)
        FreeVec(mv_touched);
    mv_orig = NULL;
    mv_touched = NULL;
    mv_active = FALSE;
}

/* Kacheln im Rechteck (Dokumentkoordinaten) aus mv_base + Schwebe neu bauen */
static void rebuild_float_rect(LONG x0, LONG y0, LONG x1, LONG y1, LONG dx, LONG dy)
{
    LONG tx, ty, x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx;
            LONG ox0 = tx << TILE_SHIFT, oy0 = ty << TILE_SHIFT;
            Tile *t = dest_tile(idx);
            if (!t)
                continue;
            if (mv_base[idx])
                memcpy(t->px, mv_base[idx]->px, sizeof(t->px));
            else
                memset(t->px, 0, sizeof(t->px));
            /* Schwebe an neuer Stelle darueberlegen */
            for (y = 0; y < TILE_SIZE; y++) {
                LONG fy = oy0 + y - fy0 - dy;
                if (fy < 0 || fy >= fh || oy0 + y >= doc->h)
                    continue;
                for (x = 0; x < TILE_SIZE; x++) {
                    LONG fx = ox0 + x - fx0 - dx;
                    if (fx < 0 || fx >= fw || ox0 + x >= doc->w)
                        continue;
                    t->px[(y << TILE_SHIFT) | x] = over(t->px[(y << TILE_SHIFT) | x], fpx[fy * fw + fx]);
                }
            }
            finish_tile(idx);
        }
}

static void move_selection_mask(LONG odx, LONG ody, LONG dx, LONG dy)
{
    LONG x, y;
    /* alte Lage loeschen, neue schreiben */
    for (y = 0; y < fh; y++) {
        LONG gy = fy0 + ody + y;
        if (gy < 0 || gy >= doc->h)
            continue;
        for (x = 0; x < fw; x++) {
            LONG gx = fx0 + odx + x;
            if (gx >= 0 && gx < doc->w)
                doc->sel[gy * doc->w + gx] = 0;
        }
    }
    for (y = 0; y < fh; y++) {
        LONG gy = fy0 + dy + y;
        if (gy < 0 || gy >= doc->h)
            continue;
        for (x = 0; x < fw; x++) {
            LONG gx = fx0 + dx + x;
            if (gx >= 0 && gx < doc->w)
                doc->sel[gy * doc->w + gx] = fsel[y * fw + x];
        }
    }
    sel_update_bounds();
}

static void layer_shift(LONG dx, LONG dy);

void move_update(LONG dx, LONG dy)
{
    if (!mv_active || (dx == cur_dx && dy == cur_dy))
        return;
    if (mm_only) {
        cur_dx = dx;
        cur_dy = dy;
        mm_shift(dx, dy);
        return;
    }
    if (mm_layer)
        mm_shift(dx, dy);
    if (mv_float) {
        LONG odx = cur_dx, ody = cur_dy;
        /* alte und neue Lage der Schwebe neu aufbauen */
        rebuild_float_rect(fx0 + odx, fy0 + ody, fx0 + odx + fw, fy0 + ody + fh, dx, dy);
        rebuild_float_rect(fx0 + dx, fy0 + dy, fx0 + dx + fw, fy0 + dy + fh, dx, dy);
        cur_dx = dx;
        cur_dy = dy;
        move_selection_mask(odx, ody, dx, dy);
        view_ants_rebuild();
        return;
    }
    cur_dx = dx;
    cur_dy = dy;
    mv_first = !mv_shifted;
    mv_shifted = TRUE;
    if (mv_group) {
        LONG k;
        for (k = 0; k < nslot; k++) {
            use_slot(k);
            layer_shift(dx, dy);
        }
        return;
    }
    layer_shift(dx, dy);
}

/* aktuelle Ebene (mv_layer) aus den Originalen um dx/dy versetzt neu aufbauen */
/* Quellbereich (Originale + Ueberhang) leer? Dann bleibt die Kachel leer */
static BOOL src_empty(LONG sx, LONG sy, LONG w, LONG h)
{
    LONG tx0, ty0, tx1, ty1, tx, ty;
    if (mv_ovf && sx < mv_ovf->x + mv_ovf->w && sx + w > mv_ovf->x &&
        sy < mv_ovf->y + mv_ovf->h && sy + h > mv_ovf->y)
        return FALSE;
    if (sx >= doc->w || sy >= doc->h || sx + w <= 0 || sy + h <= 0)
        return TRUE;
    tx0 = sx < 0 ? 0 : sx >> TILE_SHIFT;
    ty0 = sy < 0 ? 0 : sy >> TILE_SHIFT;
    tx1 = (sx + w - 1 >= doc->w ? doc->w - 1 : sx + w - 1) >> TILE_SHIFT;
    ty1 = (sy + h - 1 >= doc->h ? doc->h - 1 : sy + h - 1) >> TILE_SHIFT;
    for (ty = ty0; ty <= ty1; ty++)
        for (tx = tx0; tx <= tx1; tx++) {
            Tile *t = mv_orig[ty * doc->tw + tx];
            if (t && t->flag != TF_EMPTY)
                return FALSE;
        }
    return TRUE;
}

static void layer_shift(LONG dx, LONG dy)
{
    LONG i, n, y;
    n = ntiles();
    for (i = 0; i < n; i++) {
        LONG tx = i % doc->tw, ty = i / doc->tw;
        LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
        LONG vw = doc->w - x0, vh = doc->h - y0;
        Tile *t;
        if (vw > TILE_SIZE) vw = TILE_SIZE;
        if (vh > TILE_SIZE) vh = TILE_SIZE;
        if (src_empty(x0 - dx, y0 - dy, vw, vh)) {
            /* bleibt leer: nur aufraeumen, wenn vorher etwas da war */
            if ((t = mv_layer->tiles[i])) {
                FreeVec(t);
                mv_layer->tiles[i] = NULL;
                mv_touched[i] = 1;
                doc_mark_dirty(tx, ty);
            } else if (mv_first && mv_orig[i])
                doc_mark_dirty(tx, ty);     /* Original stand hier, jetzt leer */
            continue;
        }
        if (!(t = dest_tile(i)))
            continue;
        for (y = 0; y < vh; y++) {
            fetch_row(mv_orig, x0 - dx, y0 + y - dy, vw, t->px + (y << TILE_SHIFT));
            fetch_ovf(x0 - dx, y0 + y - dy, vw, t->px + (y << TILE_SHIFT));
        }
        finish_tile(i);
    }
}

static void end_layer(UndoRec *u);

/* Originale unveraendert zuruecklegen (Klick ohne Ziehen) */
static void restore_stolen(void)
{
    LONG i, n = ntiles();
    for (i = 0; i < n; i++) {
        if (mv_layer->tiles[i])
            FreeVec(mv_layer->tiles[i]);
        mv_layer->tiles[i] = mv_orig[i];
        mv_orig[i] = NULL;
        mv_touched[i] = 0;
    }
}

void move_end(void)
{
    UndoRec *u;
    if (!mv_active)
        return;
    doc_cache_drop();
    if (mm_only) {
        mm_end(FALSE);
        mm_only = FALSE;
        mv_active = FALSE;
        return;
    }
    if (mv_stolen && !mv_shifted) {
        mm_end(FALSE);          /* nicht bewegt: nichts zu tun */
        LONG k;
        if (mv_group)
            for (k = 0; k < nslot; k++) {
                use_slot(k);
                restore_stolen();
                free_state();
            }
        else {
            if (mv_text_undo) {
                undo_discard(mv_text_undo);
                mv_text_undo = NULL;
            }
            restore_stolen();
            free_state();
        }
        nslot = 0;
        mv_group = FALSE;
        mv_stolen = FALSE;
        return;
    }
    mv_stolen = FALSE;
    if (mv_group) {
        /* Textpositionen (Struktur-Eintrag), dann je Ebene ein verketteter Eintrag */
        UndoRec *st = undo_begin_struct("Move");
        BOOL any;
        LONG k;
        for (k = 0; k < nslot; k++) {
            TextInfo *t = slot[k].l->text;
            if (t && (cur_dx || cur_dy)) {
                t->x += cur_dx; t->y += cur_dy;
                t->bx0 += cur_dx; t->bx1 += cur_dx;
                t->by0 += cur_dy; t->by1 += cur_dy;
            }
        }
        undo_commit(st);
        any = st && doc->ucur == st;
        for (k = 0; k < nslot; k++) {
            use_slot(k);
            u = undo_begin(mv_layer, "Move");
            if (u)
                u->chain = any;
            end_layer(u);
            if (u && doc->ucur == u)
                any = TRUE;
            free_state();
        }
        mm_end(any);
        nslot = 0;
        mv_group = FALSE;
        return;
    }
    if (mv_text_undo) {
        /* Textebene: Grundlinie mitverschieben, Kacheln und Text in einem Eintrag */
        u = mv_text_undo;
        u->layer = mv_layer;
        mv_layer->text->x += cur_dx;
        mv_layer->text->y += cur_dy;
        mv_layer->text->bx0 += cur_dx; mv_layer->text->bx1 += cur_dx;
        mv_layer->text->by0 += cur_dy; mv_layer->text->by1 += cur_dy;
        mv_text_undo = NULL;
    } else
        u = undo_begin(mv_layer, "Move");
    {
        UndoRec *before = doc->ucur;
        end_layer(u);
        free_state();
        mm_end(doc->ucur != before);    /* Maske im selben Schritt wie die Pixel */
    }
}

/* Ueberhang und Original-Kacheln der aktuellen Ebene ins Undo u, dann abschliessen */
static void end_layer(UndoRec *u)
{
    LONG i, n = ntiles();
    /* ganze Ebene verschoben: was ueber den Rand ragt, bleibt im Ueberhang */
    if (!mv_float && (cur_dx || cur_dy)) {
        LONG x0 = 0, y0 = 0, x1 = doc->w, y1 = doc->h;
        Ovf *no;
        if (mv_ovf) {
            if (mv_ovf->x < x0) x0 = mv_ovf->x;
            if (mv_ovf->y < y0) y0 = mv_ovf->y;
            if (mv_ovf->x + mv_ovf->w > x1) x1 = mv_ovf->x + mv_ovf->w;
            if (mv_ovf->y + mv_ovf->h > y1) y1 = mv_ovf->y + mv_ovf->h;
        }
        no = ovf_build(x0 + cur_dx, y0 + cur_dy, x1 + cur_dx, y1 + cur_dy, doc->w, doc->h, shifted_get);
        if (u && (no || mv_ovf)) {
            u->has_ovf = TRUE;
            u->ovf = mv_ovf;    /* alter Ueberhang gehoert jetzt dem Undo */
            mv_layer->ovf = no;
        } else if (no)
            FreeVec(no);
    }
    mv_ovf = NULL;
    for (i = 0; i < n; i++)
        if (mv_touched[i] && (mv_orig[i] || mv_layer->tiles[i])) {
            undo_add(u, i, mv_orig[i]);
            mv_orig[i] = NULL;  /* gehoert jetzt dem Undo */
        }
    undo_commit(u);
}

void move_nudge(LONG dx, LONG dy)
{
    move_begin();
    move_update(dx, dy);
    move_end();
}
