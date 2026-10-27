/*
 * doc.c - Dokument, Kachel-Ebenen, Komposit (flat), Undo
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <stdio.h>
#include "apx.h"

Doc *doc;

#define UNDO_MAX 40

/* ---- Kacheln ---- */

Tile *tile_new(void)
{
    Tile *t = AllocVec(sizeof(Tile), MEMF_ANY | MEMF_CLEAR);
    if (t)
        t->flag = TF_EMPTY;
    return t;
}

Tile *tile_clone(const Tile *t)
{
    Tile *c = AllocVec(sizeof(Tile), MEMF_ANY);
    if (c) {
        /* CopyMemQuick nur fuer die Pixel: sizeof(Tile) ist nicht durch 4 teilbar */
        CopyMemQuick((APTR)t->px, c->px, sizeof(t->px));
        c->flag = t->flag;
    }
    return c;
}

/* Merker neu bestimmen, nur ueber den sichtbaren Teil (Randkacheln) */
void tile_update_flag(Doc *d, Tile *t, LONG tx, LONG ty)
{
    LONG vw = d->w - (tx << TILE_SHIFT), vh = d->h - (ty << TILE_SHIFT), x, y;
    ULONG and_a = 0xFF, or_a = 0;
    if (vw > TILE_SIZE) vw = TILE_SIZE;
    if (vh > TILE_SIZE) vh = TILE_SIZE;
    for (y = 0; y < vh; y++) {
        const ULONG *p = t->px + y * TILE_SIZE;
        for (x = 0; x < vw; x++) {
            ULONG a = p[x] >> 24;
            and_a &= a;
            or_a |= a;
        }
        if (or_a && and_a != 0xFF)
            break;              /* gemischt, Rest egal */
    }
    t->flag = !or_a ? TF_EMPTY : (and_a == 0xFF ? TF_OPAQUE : TF_MIX);
}

/* ---- Ebenen ---- */

Layer *layer_new(Doc *d, const char *name)
{
    Layer *l = AllocVec(sizeof(Layer), MEMF_ANY | MEMF_CLEAR);
    if (!l)
        return NULL;
    l->tiles = AllocVec(d->tw * d->th * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    l->fill = 255;
    if (!l->tiles) {
        FreeVec(l);
        return NULL;
    }
    l->opacity = 255;
    l->visible = 1;
    strncpy(l->name, name, sizeof(l->name) - 1);
    return l;
}

static void mask_free(Tile **m, Doc *d)
{
    LONG i;
    for (i = 0; i < d->tw * d->th; i++)
        if (m[i])
            FreeVec(m[i]);
    FreeVec(m);
}

/* ---- Ueberhang ---- */

Ovf *ovf_new(LONG x, LONG y, LONG w, LONG h)
{
    Ovf *o = AllocVec(sizeof(Ovf) + w * h * 4, MEMF_ANY | MEMF_CLEAR);
    if (o) {
        o->x = x; o->y = y; o->w = w; o->h = h;
    }
    return o;
}

Ovf *ovf_clone(const Ovf *o)
{
    Ovf *c;
    if (!o || !(c = ovf_new(o->x, o->y, o->w, o->h)))
        return NULL;
    CopyMem((APTR)o->px, c->px, o->w * o->h * 4);
    return c;
}

ULONG ovf_get(const Ovf *o, LONG x, LONG y)
{
    x -= o->x;
    y -= o->y;
    if (x < 0 || y < 0 || x >= o->w || y >= o->h)
        return 0;
    return o->px[y * o->w + x];
}

/* Pixel get(x, y) im Rechteck x0..x1/y0..y1, die ausserhalb der Flaeche
 * cw x ch liegen, als zugeschnittener Ueberhang (NULL: nichts uebrig) */
Ovf *ovf_build(LONG x0, LONG y0, LONG x1, LONG y1, LONG cw, LONG ch,
               ULONG (*get)(LONG x, LONG y))
{
    LONG x, y, bx0 = x1, by0 = y1, bx1 = x0, by1 = y0;
    Ovf *o;
    /* 1. Umriss der sichtbaren Pixel ausserhalb */
    for (y = y0; y < y1; y++) {
        BOOL in_y = y >= 0 && y < ch;
        for (x = x0; x < x1; x++) {
            if (in_y && x >= 0 && x < cw) {
                x = cw - 1;     /* Innenteil ueberspringen */
                continue;
            }
            if (get(x, y) >> 24) {
                if (x < bx0) bx0 = x;
                if (x >= bx1) bx1 = x + 1;
                if (y < by0) by0 = y;
                if (y >= by1) by1 = y + 1;
            }
        }
    }
    if (bx1 <= bx0 || by1 <= by0 || !(o = ovf_new(bx0, by0, bx1 - bx0, by1 - by0)))
        return NULL;
    /* 2. fuellen */
    for (y = by0; y < by1; y++) {
        BOOL in_y = y >= 0 && y < ch;
        for (x = bx0; x < bx1; x++) {
            if (in_y && x >= 0 && x < cw) {
                x = cw - 1;
                continue;
            }
            o->px[(y - by0) * o->w + (x - bx0)] = get(x, y);
        }
    }
    return o;
}

void layer_free(Layer *l, Doc *d)
{
    LONG i;
    if (!l)
        return;
    for (i = 0; i < d->tw * d->th; i++)
        if (l->tiles[i])
            FreeVec(l->tiles[i]);
    FreeVec(l->tiles);
    if (l->mask)
        mask_free(l->mask, d);
    if (l->text)
        FreeVec(l->text);
    fx_free_cache(l, d);
    if (l->fx)
        FreeVec(l->fx);
    if (l->ovf)
        FreeVec(l->ovf);
    if (l->movf)
        FreeVec(l->movf);
    if (l->adj)
        FreeVec(l->adj);
    FreeVec(l);
}

/* graue, deckende Kachel (Maske) */
Tile *tile_new_gray(ULONG gray)
{
    Tile *t = AllocVec(sizeof(Tile), MEMF_ANY);
    LONG k;
    ULONG v = 0xFF000000 | (gray * 0x010101);
    if (!t)
        return NULL;
    for (k = 0; k < TILE_PIX; k++)
        t->px[k] = v;
    t->flag = TF_OPAQUE;
    return t;
}

ULONG mask_gray(ULONG rgb)
{
    return ((((rgb >> 16) & 0xFF) * 77 + ((rgb >> 8) & 0xFF) * 151 + (rgb & 0xFF) * 28 + 128) >> 8);
}

BOOL doc_target_is_mask(void)
{
    return doc->edit_mask && doc->layers[doc->active]->mask != NULL;
}

/* Masken sparsam: eine fehlende Maskenkachel ist ganz weiss (zeigt alles).
 * Zum Bearbeiten wird die Maske voll belegt, danach wieder verdichtet. */
BOOL mask_dense(Layer *l)
{
    LONG i, n = doc->tw * doc->th;
    if (!l->mask)
        return TRUE;
    for (i = 0; i < n; i++)
        if (!l->mask[i] && !(l->mask[i] = tile_new_gray(255)))
            return FALSE;
    return TRUE;
}

void mask_compact(Doc *d, Layer *l)
{
    LONG i, k, n = d->tw * d->th;
    if (!l->mask)
        return;
    for (i = 0; i < n; i++) {
        Tile *t = l->mask[i];
        if (!t)
            continue;
        for (k = 0; k < TILE_PIX && (t->px[k] & 0xFF) == 0xFF; k++)
            ;
        if (k == TILE_PIX) {
            FreeVec(t);
            l->mask[i] = NULL;
        }
    }
}

/* alle Masken des Dokuments verdichten (nur ausserhalb laufender Aktionen) */
void doc_masks_compact(void)
{
    LONG i;
    for (i = 0; i < doc->nlayers; i++)
        mask_compact(doc, doc->layers[i]);
}

Tile **doc_target(void)
{
    Layer *l = doc->layers[doc->active];
    if (doc_target_is_mask()) {
        mask_dense(l);          /* Bearbeiten braucht jede Kachel */
        return l->mask;
    }
    return l->tiles;
}

/* Kachel mit Maske verrechnet; liefert t->px, wenn die Maske dort ganz weiss ist */
static const ULONG *masked_px(Layer *l, LONG idx, const Tile *t, ULONG *tmp)
{
    const Tile *m;
    LONG k;
    if (!l->mask || !l->mask_on || !(m = l->mask[idx]))
        return t->px;
    for (k = 0; k < TILE_PIX; k++)
        if ((m->px[k] & 0xFF) != 0xFF)
            break;
    if (k == TILE_PIX)
        return t->px;
    for (k = 0; k < TILE_PIX; k++) {
        ULONG p = t->px[k], a = p >> 24, g = m->px[k] & 0xFF;
        tmp[k] = (((a * g * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
    }
    return tmp;
}

static LONG add_layer(Doc *d)
{
    char name[32];
    Layer *l;
    LONG i;
    if (d->nlayers >= MAX_LAYERS)
        return -1;
    sprintf(name, "Layer %ld", (long)++d->layer_counter);
    if (!(l = layer_new(d, name)))
        return -1;
    for (i = d->nlayers; i > d->active + 1; i--)
        d->layers[i] = d->layers[i - 1];
    d->layers[d->active + 1] = l;
    d->nlayers++;
    d->active++;
    return d->active;
}

LONG doc_add_layer(void)
{
    UndoRec *u = undo_begin_struct("New Layer");
    LONG r = add_layer(doc);
    undo_commit(u);
    return r;
}

/* Ebene aus der Liste nehmen; freigegeben wird sie spaeter vom Undo */
static void remove_layer(LONG i)
{
    for (; i < doc->nlayers - 1; i++)
        doc->layers[i] = doc->layers[i + 1];
    doc->nlayers--;
    if (doc->active >= doc->nlayers)
        doc->active = doc->nlayers - 1;
    doc_mark_all_dirty();
}

void doc_delete_layer(void)
{
    UndoRec *u;
    LONG size = layer_block(doc->active), k;
    if (doc->nlayers - size < 1)
        return;
    u = undo_begin_struct(size > 1 ? "Delete Group" : "Delete Layer");
    for (k = 0; k < size; k++)
        remove_layer(doc->active - size + 1);
    if (doc->active >= doc->nlayers)
        doc->active = doc->nlayers - 1;
    undo_commit(u);
}

/* tiefe Kopie einer Ebene (Name wie angegeben) */
static void insert_layer(LONG at, Layer *l);

static Layer *clone_layer(Layer *src, const char *name)
{
    Layer *l;
    LONG i;
    if (!(l = layer_new(doc, name)))
        return NULL;
    for (i = 0; i < doc->tw * doc->th; i++)
        if (src->tiles[i] && !(l->tiles[i] = tile_clone(src->tiles[i]))) {
            layer_free(l, doc);
            return NULL;
        }
    l->opacity = src->opacity;
    l->visible = src->visible;
    l->mode = src->mode;
    l->mask_on = src->mask_on;
    if (src->mask && (l->mask = AllocVec(doc->tw * doc->th * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR)))
        for (i = 0; i < doc->tw * doc->th; i++)
            l->mask[i] = src->mask[i] ? tile_clone(src->mask[i]) : NULL;
    if (src->text && (l->text = AllocVec(sizeof(TextInfo), MEMF_ANY)))
        *l->text = *src->text;
    if (src->fx && (l->fx = AllocVec(sizeof(LayerFx), MEMF_ANY)))
        *l->fx = *src->fx;
    l->ovf = ovf_clone(src->ovf);
    l->movf = ovf_clone(src->movf);
    l->fill = src->fill;
    l->lock = src->lock;
    l->kind = src->kind;
    l->depth = src->depth;
    l->pass = src->pass;
    l->collapsed = src->collapsed;
    l->clip = src->clip;
    if (src->adj && (l->adj = AllocVec(sizeof(AdjInfo), MEMF_ANY)))
        *l->adj = *src->adj;
    return l;
}

/* Ebene oder ganze Gruppe duplizieren (Kopie direkt darueber) */
void doc_duplicate_layer(void)
{
    LONG top = doc->active, size = layer_block(top), bot = top - size + 1, k;
    Layer *cl[MAX_LAYERS];
    UndoRec *u;
    char name[40];
    if (doc->nlayers + size > MAX_LAYERS)
        return;
    for (k = 0; k < size; k++) {
        Layer *src = doc->layers[bot + k];
        strcpy(name, src->name);
        if (k == size - 1) {
            name[24] = 0;
            strcat(name, " copy");
        }
        if (!(cl[k] = clone_layer(src, name))) {
            while (k-- > 0)
                layer_free(cl[k], doc);
            return;
        }
    }
    u = undo_begin_struct(size > 1 ? "Duplicate Group" : "Duplicate Layer");
    for (k = 0; k < size; k++)
        insert_layer(top + 1 + k, cl[k]);
    doc->active = top + size;
    doc_mark_all_dirty();
    undo_commit(u);
}

void doc_move_layer(LONG from, LONG to)
{
    Layer *l;
    UndoRec *u;
    LONG i;
    if (from < 0 || to < 0 || from >= doc->nlayers || to >= doc->nlayers || from == to)
        return;
    u = undo_begin_struct("Layer Order");
    l = doc->layers[from];
    if (from < to)
        for (i = from; i < to; i++)
            doc->layers[i] = doc->layers[i + 1];
    else
        for (i = from; i > to; i--)
            doc->layers[i] = doc->layers[i - 1];
    doc->layers[to] = l;
    doc->active = to;
    doc_mark_all_dirty();
    undo_commit(u);
}

/* premultiplied -> straight fuer n Pixel */
static void unpremul(const ULONG *s, ULONG *d, LONG n)
{
    LONG x;
    for (x = 0; x < n; x++) {
        ULONG p = s[x], a = p >> 24, inv;
        if (a == 255 || !a) {
            d[x] = a ? p : 0;
            continue;
        }
        inv = (255 * 65536 + a / 2) / a;
        d[x] = (a << 24)
             | ((((p >> 16) & 0xFF) * inv >> 16) << 16)
             | ((((p >> 8) & 0xFF) * inv >> 16) << 8)
             | ((p & 0xFF) * inv >> 16);
    }
}

/* Aktive Ebene mit Fuellmethode und Deckkraft in die darunterliegende rechnen */
static void replace_range(LONG lo, LONG hi, const char *name, const char *uname);

void doc_merge_down(void)
{
    Layer *up, *lo;
    UndoRec *u;
    ULONG *tmp;
    static ULONG mtmp[TILE_PIX];
    LONG i, y;
    if (doc->active < 1)
        return;
    {
        LONG top = doc->active, bot = top - layer_block(top) + 1, k = bot - 1;
        Layer *a = doc->layers[top], *b;
        if (k < 0)
            return;
        b = doc->layers[k];
        if (b->depth != a->depth)
            return;                 /* unterste Ebene einer Gruppe: nichts darunter */
        if (a->kind != LK_PIXEL || b->kind != LK_PIXEL || bot != top) {
            /* Gruppen/Korrekturebenen: Bereich zusammensetzen */
            replace_range(k - layer_block(k) + 1, top, b->name, "Merge Down");
            return;
        }
    }
    doc_rasterize_text(doc->layers[doc->active - 1]);
    up = doc->layers[doc->active];
    lo = doc->layers[doc->active - 1];
    if (!(tmp = AllocVec(TILE_PIX * 4, MEMF_ANY)))
        return;
    u = undo_begin_struct("Merge Down");
    if (u)
        u->layer = lo;
    doc_process_dirty();        /* Effekt-Zwischenspeicher aktuell */
    for (i = 0; i < doc->tw * doc->th; i++) {
        Tile *ts = up->tiles[i], *td = lo->tiles[i];
        BOOL fxany = FALSE;
        LONG k;
        if (up->fx_valid && fx_active(up->fx) && up->fxc[0])
            for (k = 0; k < FX_COUNT; k++)
                if (up->fxc[k] && up->fxc[k][i] && up->fxc[k][i]->flag != TF_EMPTY)
                    fxany = TRUE;
        if (((!ts || ts->flag == TF_EMPTY) && !fxany) || !up->visible || !up->opacity)
            continue;
        undo_add(u, i, td ? tile_clone(td) : NULL);
        if (!td) {
            if (!(td = tile_new()))
                continue;
            lo->tiles[i] = td;
        }
        {
            static const UBYTE fxmode[FX_COUNT] = { BM_MULTIPLY, BM_SCREEN, BM_NORMAL };
            const ULONG *src = ts ? masked_px(up, i, ts, mtmp) : NULL;
            for (y = 0; y < TILE_SIZE; y++) {
                ULONG *row = tmp + y * TILE_SIZE;
                span_layer_first(row, td->px + y * TILE_SIZE, TILE_SIZE, 255);
                if (fxany)
                    for (k = 0; k < FX_COUNT; k++)
                        if (up->fxc[k] && up->fxc[k][i])
                            span_blend(row, up->fxc[k][i]->px + y * TILE_SIZE, TILE_SIZE, up->opacity, fxmode[k]);
                if (src)
                    span_blend(row, src + y * TILE_SIZE, TILE_SIZE, (up->opacity * up->fill + 127) / 255, up->mode);
                unpremul(row, td->px + y * TILE_SIZE, TILE_SIZE);
            }
        }
        tile_update_flag(doc, td, i % doc->tw, i / doc->tw);
    }
    FreeVec(tmp);
    i = doc->active;
    remove_layer(i);
    doc->active = i - 1;
    undo_commit(u);
}

/* ---- Dokument ---- */

Doc *doc_new(LONG w, LONG h, LONG mode)
{
    Doc *d = AllocVec(sizeof(Doc), MEMF_ANY | MEMF_CLEAR);
    LONG i, ntiles;
    if (!d)
        return NULL;
    d->w = w;
    d->h = h;
    d->tw = (w + TILE_MASK) >> TILE_SHIFT;
    d->th = (h + TILE_MASK) >> TILE_SHIFT;
    ntiles = d->tw * d->th;
    d->flat = AllocVec(w * h * 4, MEMF_ANY | MEMF_CLEAR);
    d->flatflag = AllocVec(ntiles, MEMF_ANY | MEMF_CLEAR);
    d->dirty = AllocVec(ntiles, MEMF_ANY | MEMF_CLEAR);
    d->sel = AllocVec(w * h, MEMF_ANY | MEMF_CLEAR);
    if (!d->flat || !d->flatflag || !d->dirty || !d->sel) {
        doc_free(d);
        return NULL;
    }

    d->layers[0] = layer_new(d, "Background");
    if (!d->layers[0]) {
        doc_free(d);
        return NULL;
    }
    d->nlayers = 1;
    if (mode == DOC_WHITE) {
        for (i = 0; i < ntiles; i++) {
            Tile *t = AllocVec(sizeof(Tile), MEMF_ANY);
            LONG k;
            if (!t) {
                doc_free(d);
                return NULL;
            }
            for (k = 0; k < TILE_PIX; k++)
                t->px[k] = 0xFFFFFFFF;
            t->flag = TF_OPAQUE;
            d->layers[0]->tiles[i] = t;
        }
        add_layer(d);
    }
    memset(d->dirty, 1, ntiles);
    d->anydirty = TRUE;
    return d;
}

void doc_free(Doc *d)
{
    LONG i;
    if (!d)
        return;
    /* ganze Kette der Groessen-Zustaende mit freigeben */
    if (d->size_prev) {
        Doc *p = d->size_prev;
        d->size_prev = NULL;
        p->size_next = NULL;
        doc_free(p);
    }
    if (d->size_next) {
        Doc *n = d->size_next;
        d->size_next = NULL;
        n->size_prev = NULL;
        doc_free(n);
    }
    if (d->uhead) {             /* Protokoll gehoert zu d: mit d als aktuellem freigeben */
        Doc *cur = doc;
        doc = d;
        undo_clear();
        doc = cur;
    }
    for (i = 0; i < d->nlayers; i++)
        layer_free(d->layers[i], d);
    if (d->flat) FreeVec(d->flat);
    if (d->flatflag) FreeVec(d->flatflag);
    if (d->dirty) FreeVec(d->dirty);
    if (d->sel) FreeVec(d->sel);
    if (d->lastsel) FreeVec(d->lastsel);
    if (d->bcache) FreeVec(d->bcache);
    if (d->bstate) FreeVec(d->bstate);
    if (d->wpath) FreeVec(d->wpath);
    if (d->qpal) FreeVec(d->qpal);
    if (d->fixpal) FreeVec(d->fixpal);
    FreeVec(d);
    if (doc == d)
        doc = NULL;
}

/* ---- Offene Dokumente (Tabs) ---- */

Doc *docs[MAX_DOCS];
LONG ndocs;
static LONG untitled_counter;

LONG doc_next_untitled(void)
{
    return untitled_counter + 1;
}

LONG doc_index(Doc *d)
{
    LONG i;
    for (i = 0; i < ndocs; i++)
        if (docs[i] == d)
            return i;
    return -1;
}

void doc_activate(Doc *d)
{
    if (d == doc)
        return;
    if (doc)
        view_save();
    doc = d;
    view_restore();
}

BOOL doc_open(Doc *d)
{
    if (ndocs >= MAX_DOCS)
        return FALSE;
    if (!d->path[0])
        d->untitled = ++untitled_counter;
    docs[ndocs++] = d;
    doc_activate(d);
    return TRUE;
}

/* Dokument schliessen; das aktive wird danach ein Nachbar-Tab */
void doc_close(Doc *d)
{
    LONG i = doc_index(d), k;
    Doc *cur = doc;
    if (i < 0)
        return;
    for (k = i; k < ndocs - 1; k++)
        docs[k] = docs[k + 1];
    ndocs--;
    doc = d;                    /* Undo-Verlauf gehoert zu d */
    undo_clear();
    doc_free(d);
    doc = cur == d ? NULL : cur;
    if (!doc && ndocs) {
        Doc *next = docs[i < ndocs ? i : ndocs - 1];
        doc = next;
        view_restore();
    }
}

const char *doc_title_of(Doc *d)
{
    const char *p = d->path, *q;
    if (!*p && d->name[0])
        return d->name;
    if (!*p) {
        sprintf(d->title, "Untitled-%ld", (long)d->untitled);
        return d->title;
    }
    for (q = p; *q; q++)
        if (*q == '/' || *q == ':')
            p = q + 1;
    return p;
}

const char *doc_title(void)
{
    return doc_title_of(doc);
}

/* ---- Dirty-Tracking ---- */

void doc_mark_dirty(LONG tx, LONG ty)
{
    doc->dirty[ty * doc->tw + tx] = 1;
    doc->anydirty = TRUE;
}

void doc_mark_all_dirty(void)
{
    memset(doc->dirty, 1, doc->tw * doc->th);
    doc->anydirty = TRUE;
}

void doc_mark_rect_dirty(LONG x0, LONG y0, LONG x1, LONG y1)
{
    LONG tx, ty;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++)
            doc_mark_dirty(tx, ty);
}

/* Eine Kachel aus allen Ebenen neu zusammensetzen (premultiplied) */
static void unpremul(const ULONG *s, ULONG *d, LONG n);

/* eine Kachel (src, Zeilenlaenge TILE_SIZE) auf das Komposit legen (Ziel-Zeilenlaenge ds) */
static void comp_tile(ULONG *dst, LONG ds, const ULONG *src, LONG tflag, LONG opacity, LONG mode,
                      LONG vw, LONG vh, LONG *state)
{
    LONG y;
    if (tflag == TF_OPAQUE && opacity == 255 && (mode == BM_NORMAL || *state == TF_EMPTY)) {
        for (y = 0; y < vh; y++)
            memcpy(dst + y * ds, src + y * TILE_SIZE, vw * 4);
        *state = TF_OPAQUE;
    } else if (*state == TF_EMPTY) {
        /* ueber Transparenz wirkt jede Fuellmethode wie Normal */
        for (y = 0; y < vh; y++)
            span_layer_first(dst + y * ds, src + y * TILE_SIZE, vw, opacity);
        *state = TF_MIX;
    } else if (mode == BM_NORMAL) {
        for (y = 0; y < vh; y++)
            span_layer_over(dst + y * ds, src + y * TILE_SIZE, vw, opacity);
    } else {
        for (y = 0; y < vh; y++)
            span_blend(dst + y * ds, src + y * TILE_SIZE, vw, opacity, mode);
    }
}

/* Basis einer Schnittmaske: naechste nicht beschnittene Pixelebene darunter
 * (gleiche Tiefe); -1 wenn keine (dann wirkt die Ebene normal) */
LONG clip_base(LONG i)
{
    Layer *l = doc->layers[i];
    LONG j;
    if (!l->clip)
        return -1;
    for (j = i - 1; j >= 0; j--) {
        Layer *b = doc->layers[j];
        if (b->depth > l->depth)
            continue;               /* Inhalt einer Gruppe darunter */
        if (b->depth < l->depth)
            return -1;
        if (b->clip)
            continue;
        return b->kind == LK_PIXEL ? j : -1;
    }
    return -1;
}

/* Alpha der Basis an einem Kachelpixel (mit Maske), 0..255 */
static UBYTE *clip_alpha(LONG base, LONG idx)
{
    static UBYTE a[TILE_PIX];
    Layer *b = doc->layers[base];
    Tile *t = b->tiles[idx];
    const Tile *m = b->mask && b->mask_on ? b->mask[idx] : NULL;
    LONG k;
    if (!t || t->flag == TF_EMPTY)
        return NULL;
    for (k = 0; k < TILE_PIX; k++) {
        ULONG v = t->px[k] >> 24;
        if (m)
            v = (v * (m->px[k] & 0xFF) * 257 + 0x8080) >> 16;
        a[k] = (UBYTE)v;
    }
    return a;
}

/* Korrekturebene: wirkt auf das bisherige Komposit (vormultipliziert) */
static void comp_adjust(ULONG *dst, LONG ds, Layer *ly, LONG idx, LONG vw, LONG vh, LONG state,
                        const UBYTE *ca)
{
    static ULONG tmp[TILE_PIX];
    ULONG op = (ly->opacity * ly->fill + 127) / 255;
    const Tile *m = ly->mask && ly->mask_on ? ly->mask[idx] : NULL;
    LONG x, y;
    if (state == TF_EMPTY || !ly->adj || !op)
        return;
    if (m) {                    /* ganz weisse Maskenkachel: wie ohne Maske */
        LONG k;
        for (k = 0; k < TILE_PIX && (m->px[k] & 0xFF) == 0xFF; k++)
            ;
        if (k == TILE_PIX)
            m = NULL;
    }
    for (y = 0; y < vh; y++)
        unpremul(dst + y * ds, tmp + y * vw, vw);
    adjust_apply(ly->adj, tmp, vw * vh);
    for (y = 0; y < vh; y++) {
        ULONG *d = dst + y * ds;
        const ULONG *c = tmp + y * vw;
        if (!m && op == 255 && !ca) {
            /* volle Wirkung ohne Maske: deckende Pixel direkt uebernehmen */
            for (x = 0; x < vw; x++) {
                ULONG p = d[x], a = p >> 24;
                if (a == 255)
                    d[x] = 0xFF000000 | (c[x] & 0xFFFFFF);
                else if (a) {
                    ULONG q = c[x], a2 = a + (a >> 7);
                    d[x] = (a << 24) | ((((q & 0xFF00FF) * a2) >> 8) & 0xFF00FF) |
                           ((((q & 0x00FF00) * a2) >> 8) & 0x00FF00);
                }
            }
            continue;
        }
        for (x = 0; x < vw; x++) {
            ULONG p = d[x], a = p >> 24, k = op, q = c[x], r = a << 24;
            LONG sh;
            if (!a)
                continue;
            if (m) {
                ULONG g = m->px[(y << TILE_SHIFT) | x] & 0xFF;
                k = (k * (g + (g >> 7))) >> 8;
            }
            if (ca) {
                ULONG g = ca[(y << TILE_SHIFT) | x];
                k = (k * (g + (g >> 7))) >> 8;
            }
            if (!k)
                continue;
            k += k >> 7;            /* 0..256 */
            for (sh = 0; sh < 24; sh += 8) {
                LONG dv = (p >> sh) & 0xFF, nv = (((q >> sh) & 0xFF) * (a + (a >> 7))) >> 8;
                r |= (ULONG)(dv + (((nv - dv) * (LONG)k) >> 8)) << sh;
            }
            d[x] = r;
        }
    }
}

/* Ebenen lo..hi der Tiefe depth einmischen; Kinder von Gruppen rekursiv */
static void comp_range(ULONG *dst, LONG ds, LONG lo, LONG hi, LONG depth, LONG idx,
                       LONG vw, LONG vh, LONG *state)
{
    static ULONG gbuf[MAX_DEPTH][TILE_PIX], gstr[TILE_PIX];
    LONG i = lo, y;
    while (i <= hi) {
        static ULONG mtmp[TILE_PIX];
        Layer *ly = doc->layers[i];
        Tile *t;
        const ULONG *src;
        if (ly->depth > depth) {
            /* Kinder einer Gruppe: Kopf ist die naechste Ebene der Tiefe depth */
            LONG h = i + 1;
            Layer *g;
            while (h <= hi && doc->layers[h]->depth > depth)
                h++;
            if (h > hi || doc->layers[h]->kind != LK_GROUP) {
                /* kaputte Struktur: wie normale Ebenen behandeln */
                comp_range(dst, ds, i, h - 1, ly->depth, idx, vw, vh, state);
                i = h;
                continue;
            }
            g = doc->layers[h];
            if (g->visible && g->opacity) {
                const Tile *gm = g->mask && g->mask_on ? g->mask[idx] : NULL;
                if ((g->pass && g->opacity == 255 && !gm) || depth + 1 >= MAX_DEPTH)
                    comp_range(dst, ds, i, h - 1, depth + 1, idx, vw, vh, state);
                else {
                    /* isoliert zusammensetzen, dann als Ganzes einmischen */
                    ULONG *b = gbuf[depth];
                    LONG st = TF_EMPTY;
                    for (y = 0; y < vh; y++)
                        memset(b + y * TILE_SIZE, 0, vw * 4);
                    comp_range(b, TILE_SIZE, i, h - 1, depth + 1, idx, vw, vh, &st);
                    if (st != TF_EMPTY && gm) {
                        /* Gruppenmaske: Ergebnis (vormultipliziert) abschwaechen */
                        LONG x;
                        for (y = 0; y < vh; y++)
                            for (x = 0; x < vw; x++) {
                                ULONG gv = gm->px[(y << TILE_SHIFT) | x] & 0xFF, p = b[y * TILE_SIZE + x];
                                if (gv == 255)
                                    continue;
                                gv += gv >> 7;
                                b[y * TILE_SIZE + x] = ((((p >> 24) * gv) >> 8) << 24) |
                                    ((((p & 0xFF00FF) * gv) >> 8) & 0xFF00FF) | ((((p & 0x00FF00) * gv) >> 8) & 0x00FF00);
                            }
                        st = TF_MIX;
                    }
                    if (st != TF_EMPTY) {
                        for (y = 0; y < vh; y++)
                            unpremul(b + y * TILE_SIZE, gstr + y * TILE_SIZE, vw);
                        comp_tile(dst, ds, gstr, TF_MIX, g->opacity, g->pass ? BM_NORMAL : g->mode,
                                  vw, vh, state);
                    }
                }
            }
            i = h + 1;
            continue;
        }
        i++;
        if (!ly->visible || !ly->opacity || ly->kind == LK_GROUP)
            continue;
        {
            /* Schnittmaske: nur wo die Basis Pixel hat */
            LONG bi = clip_base(i - 1);
            if (bi >= 0) {
                Layer *bl = doc->layers[bi];
                const UBYTE *ca;
                Tile *ct = ly->tiles[idx];
                LONG k;
                if (!bl->visible || !(ca = clip_alpha(bi, idx)))
                    continue;
                if (ly->kind == LK_ADJUST) {
                    comp_adjust(dst, ds, ly, idx, vw, vh, *state, ca);
                    continue;
                }
                if (!ct || ct->flag == TF_EMPTY || !ly->fill)
                    continue;
                src = masked_px(ly, idx, ct, mtmp);
                if (src != mtmp)
                    CopyMem((APTR)src, mtmp, TILE_PIX * 4);
                for (k = 0; k < TILE_PIX; k++) {
                    ULONG p = mtmp[k], a = ((p >> 24) * ca[k] * 257 + 0x8080) >> 16;
                    mtmp[k] = (a << 24) | (p & 0xFFFFFF);
                }
                comp_tile(dst, ds, mtmp, TF_MIX, (ly->opacity * ly->fill + 127) / 255, ly->mode, vw, vh, state);
                continue;
            }
        }
        if (ly->kind == LK_ADJUST) {
            comp_adjust(dst, ds, ly, idx, vw, vh, *state, NULL);
            continue;
        }
        t = ly->tiles[idx];
        /* Effekte unter der Ebene */
        if (ly->fx_valid && fx_active(ly->fx) && ly->fxc[0]) {
            static const UBYTE fxmode[FX_COUNT] = { BM_MULTIPLY, BM_SCREEN, BM_NORMAL };
            LONG k;
            for (k = 0; k < FX_COUNT; k++) {
                Tile *e = ly->fxc[k] ? ly->fxc[k][idx] : NULL;
                if (e && e->flag != TF_EMPTY)
                    comp_tile(dst, ds, e->px, e->flag, ly->opacity, fxmode[k], vw, vh, state);
            }
        }
        if (!t || t->flag == TF_EMPTY || !ly->fill)
            continue;
        src = masked_px(ly, idx, t, mtmp);
        comp_tile(dst, ds, src, src == t->px ? t->flag : TF_MIX, (ly->opacity * ly->fill + 127) / 255,
                  ly->mode, vw, vh, state);
    }
}

/* Unterbau zwischenspeichern: Ebenen 0..upto-1 aendern sich waehrend einer
 * Interaktion nicht (nur die aktive und darueber). Spart pro Schritt deren
 * Zusammensetzen. upto muss eine Blockgrenze sein (darunter Tiefe 0). */
void doc_cache_below(LONG upto)
{
    LONG tx, ty, y;
    doc_cache_drop();
    if (upto < 1 || upto > doc->nlayers || doc->layers[upto - 1]->depth != 0)
        return;
    doc_process_dirty();
    doc->bcache = AllocVec(doc->w * doc->h * 4, MEMF_ANY);
    doc->bstate = AllocVec(doc->tw * doc->th, MEMF_ANY);
    if (!doc->bcache || !doc->bstate) {
        doc_cache_drop();
        return;
    }
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT, vw = doc->w - x0, vh = doc->h - y0;
            LONG st = TF_EMPTY;
            ULONG *dst = doc->bcache + y0 * doc->w + x0;
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            comp_range(dst, doc->w, 0, upto - 1, 0, ty * doc->tw + tx, vw, vh, &st);
            if (st == TF_EMPTY)
                for (y = 0; y < vh; y++)
                    memset(dst + y * doc->w, 0, vw * 4);
            doc->bstate[ty * doc->tw + tx] = (UBYTE)st;
        }
    doc->bcache_upto = upto;
}

void doc_cache_drop(void)
{
    if (doc->bcache) FreeVec(doc->bcache);
    if (doc->bstate) FreeVec(doc->bstate);
    doc->bcache = NULL;
    doc->bstate = NULL;
    doc->bcache_upto = 0;
}

static void flatten_tile(LONG tx, LONG ty)
{
    LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
    LONG vw = doc->w - x0, vh = doc->h - y0;
    LONG idx = ty * doc->tw + tx, y;
    ULONG *dst = doc->flat + y0 * doc->w + x0;
    LONG state = TF_EMPTY;

    if (vw > TILE_SIZE) vw = TILE_SIZE;
    if (vh > TILE_SIZE) vh = TILE_SIZE;

    if (doc->bcache && doc->bcache_upto <= doc->nlayers) {
        /* Unterbau aus dem Zwischenspeicher, nur der Rest wird gemischt */
        const ULONG *src = doc->bcache + y0 * doc->w + x0;
        state = doc->bstate[idx];
        if (state != TF_EMPTY)
            for (y = 0; y < vh; y++)
                memcpy(dst + y * doc->w, src + y * doc->w, vw * 4);
        comp_range(dst, doc->w, doc->bcache_upto, doc->nlayers - 1, 0, idx, vw, vh, &state);
    } else
        comp_range(dst, doc->w, 0, doc->nlayers - 1, 0, idx, vw, vh, &state);

    if (state == TF_EMPTY) {
        for (y = 0; y < vh; y++)
            memset(dst + y * doc->w, 0, vw * 4);
    } else if (state == TF_MIX) {
        /* koennte trotzdem ganz deckend oder leer sein */
        ULONG and_a = 0xFF, or_a = 0;
        LONG x;
        for (y = 0; y < vh; y++) {
            const ULONG *p = dst + y * doc->w;
            for (x = 0; x < vw; x++) {
                ULONG a = p[x] >> 24;
                and_a &= a;
                or_a |= a;
            }
        }
        state = !or_a ? TF_EMPTY : (and_a == 0xFF ? TF_OPAQUE : TF_MIX);
    }
    doc->flatflag[idx] = (UBYTE)state;
}

void doc_process_dirty(void)
{
    LONG tx, ty;
    if (!doc->anydirty)
        return;
    fx_process();
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            UBYTE *dp = &doc->dirty[ty * doc->tw + tx];
            if (!*dp)
                continue;
            *dp = 0;
            flatten_tile(tx, ty);
            view_inval_doc(tx << TILE_SHIFT, ty << TILE_SHIFT,
                           (tx + 1) << TILE_SHIFT, (ty + 1) << TILE_SHIFT);
        }
    doc->anydirty = FALSE;
}

ULONG layer_get(Layer *l, LONG x, LONG y)
{
    Tile *t;
    if (x < 0 || y < 0 || x >= doc->w || y >= doc->h)
        return 0;
    t = l->tiles[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
    return t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
}

/* Farbe im Komposit (straight RGB) */
ULONG doc_pick(LONG x, LONG y)
{
    ULONG p, a, r, g, b;
    if (x < 0 || y < 0 || x >= doc->w || y >= doc->h)
        return 0;
    p = doc->flat[y * doc->w + x];
    a = p >> 24;
    if (!a)
        return 0xFFFFFF;
    r = ((p >> 16) & 0xFF) * 255 / a;
    g = ((p >> 8) & 0xFF) * 255 / a;
    b = (p & 0xFF) * 255 / a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

/* ---- Undo ----
 * Ein Eintrag haelt Kachel-Originale einer Ebene und/oder den Ebenen-Aufbau
 * vorher/nachher. Entfernte Ebenen leben weiter, solange ein Eintrag auf sie
 * zeigt; freigegeben wird erst, was weder im Dokument noch in einem
 * verbliebenen Eintrag vorkommt.
 */

/* Verlauf liegt im Dokument: doc->uhead, doc->ucur (zuletzt angewendet), doc->ucount */

static void state_capture(LayerState *st)
{
    LONG i;
    st->n = doc->nlayers;
    st->active = doc->active;
    for (i = 0; i < st->n; i++) {
        Layer *l = doc->layers[i];
        st->l[i] = l;
        st->opacity[i] = l->opacity;
        st->mode[i] = l->mode;
        st->mask[i] = l->mask;
        st->mask_on[i] = l->mask_on;
        st->has_text[i] = l->text != NULL;
        if (l->text)
            st->text[i] = *l->text;
        st->has_fx[i] = l->fx != NULL;
        st->fill[i] = l->fill;
        st->lock[i] = l->lock;
        st->kind[i] = l->kind;
        st->depth[i] = l->depth;
        st->pass[i] = l->pass;
        st->collapsed[i] = l->collapsed;
        st->has_adj[i] = l->adj != NULL;
        st->clip[i] = l->clip;
        if (l->adj)
            st->adj[i] = *l->adj;
        if (l->fx)
            st->fx[i] = *l->fx;
        memcpy(st->name[i], l->name, 32);
    }
}

static void state_restore(const LayerState *st)
{
    LONG i;
    doc->nlayers = st->n;
    doc->active = st->active;
    for (i = 0; i < st->n; i++) {
        Layer *l = st->l[i];
        doc->layers[i] = l;
        l->opacity = st->opacity[i];
        l->mode = st->mode[i];
        l->mask = st->mask[i];
        l->mask_on = st->mask_on[i];
        if (st->has_text[i]) {
            if (!l->text)
                l->text = AllocVec(sizeof(TextInfo), MEMF_ANY);
            if (l->text)
                *l->text = st->text[i];
        } else if (l->text) {
            FreeVec(l->text);
            l->text = NULL;
        }
        if (st->has_fx[i]) {
            if (!l->fx)
                l->fx = AllocVec(sizeof(LayerFx), MEMF_ANY);
            if (l->fx)
                *l->fx = st->fx[i];
        } else if (l->fx) {
            FreeVec(l->fx);
            l->fx = NULL;
        }
        l->fx_valid = FALSE;
        l->fill = st->fill[i];
        l->lock = st->lock[i];
        l->kind = st->kind[i];
        l->depth = st->depth[i];
        l->pass = st->pass[i];
        l->collapsed = st->collapsed[i];
        l->clip = st->clip[i];
        if (st->has_adj[i]) {
            if (!l->adj)
                l->adj = AllocVec(sizeof(AdjInfo), MEMF_ANY);
            if (l->adj)
                *l->adj = st->adj[i];
        } else if (l->adj) {
            FreeVec(l->adj);
            l->adj = NULL;
        }
        memcpy(l->name, st->name[i], 32);
    }
    doc_mark_all_dirty();
}

static BOOL state_equal(const LayerState *a, const LayerState *b)
{
    LONG i;
    if (a->n != b->n || a->active != b->active)
        return FALSE;
    for (i = 0; i < a->n; i++)
        if (a->l[i] != b->l[i] || a->opacity[i] != b->opacity[i] || a->mode[i] != b->mode[i] ||
            a->mask[i] != b->mask[i] || a->mask_on[i] != b->mask_on[i] ||
            a->has_text[i] != b->has_text[i] || memcmp(a->name[i], b->name[i], 32) ||
            (a->has_text[i] && memcmp(&a->text[i], &b->text[i], sizeof(TextInfo))) ||
            a->has_fx[i] != b->has_fx[i] || a->fill[i] != b->fill[i] || a->lock[i] != b->lock[i] ||
            a->kind[i] != b->kind[i] || a->depth[i] != b->depth[i] || a->pass[i] != b->pass[i] ||
            a->collapsed[i] != b->collapsed[i] || a->has_adj[i] != b->has_adj[i] || a->clip[i] != b->clip[i] ||
            (a->has_adj[i] && memcmp(&a->adj[i], &b->adj[i], sizeof(AdjInfo))) ||
            (a->has_fx[i] && memcmp(&a->fx[i], &b->fx[i], sizeof(LayerFx))))
            return FALSE;
    return TRUE;
}

static BOOL layer_in_doc(Layer *l)
{
    LONG i;
    for (i = 0; i < doc->nlayers; i++)
        if (doc->layers[i] == l)
            return TRUE;
    return FALSE;
}

static BOOL layer_in_rec(Layer *l, UndoRec *u)
{
    LONG i;
    if (u->layer == l)
        return TRUE;
    if (u->before)
        for (i = 0; i < u->before->n; i++)
            if (u->before->l[i] == l)
                return TRUE;
    if (u->after)
        for (i = 0; i < u->after->n; i++)
            if (u->after->l[i] == l)
                return TRUE;
    return FALSE;
}

static BOOL mask_in_rec(Tile **m, UndoRec *u)
{
    LONG i;
    if (u->before)
        for (i = 0; i < u->before->n; i++)
            if (u->before->mask[i] == m)
                return TRUE;
    if (u->after)
        for (i = 0; i < u->after->n; i++)
            if (u->after->mask[i] == m)
                return TRUE;
    return FALSE;
}

/* Kette (bereits aus der Liste geloest) freigeben, samt verwaister Ebenen und
 * Masken (die leben, solange das Dokument oder ein Eintrag sie braucht) */
static void free_chain(UndoRec *chain)
{
    static Layer *cand[MAX_LAYERS * 2 * (UNDO_MAX + 1)];
    static Tile **mcand[MAX_LAYERS * 2 * (UNDO_MAX + 1)];
    static UBYTE unused[MAX_LAYERS * 2 * (UNDO_MAX + 1)];
    LONG nc = 0, nm = 0, i, k, j;
    UndoRec *r, *nx;

    for (r = chain; r; r = r->next) {
        LayerState *sts[2];
        sts[0] = r->before;
        sts[1] = r->after;
        for (k = 0; k < 2; k++)
            if (sts[k])
                for (i = 0; i < sts[k]->n; i++) {
                    Layer *l = sts[k]->l[i];
                    Tile **m = sts[k]->mask[i];
                    for (j = 0; j < nc && cand[j] != l; j++)
                        ;
                    if (j == nc && nc < (LONG)(sizeof(cand) / sizeof(cand[0])))
                        cand[nc++] = l;
                    if (m) {
                        for (j = 0; j < nm && mcand[j] != m; j++)
                            ;
                        if (j == nm && nm < (LONG)(sizeof(mcand) / sizeof(mcand[0])))
                            mcand[nm++] = m;
                    }
                }
    }
    for (r = chain; r; r = nx) {
        nx = r->next;
        for (i = 0; i < r->n; i++)
            if (r->tiles[i])
                FreeVec(r->tiles[i]);
        if (r->idx) FreeVec(r->idx);
        if (r->tiles) FreeVec(r->tiles);
        if (r->before) FreeVec(r->before);
        if (r->after) FreeVec(r->after);
        if (r->ovf) FreeVec(r->ovf);
        if (r->path) FreeVec(r->path);
        FreeVec(r);
    }
    /* welche Ebenen werden nicht mehr gebraucht? */
    for (i = 0; i < nc; i++) {
        BOOL used = layer_in_doc(cand[i]);
        for (r = doc->uhead; r && !used; r = r->next)
            used = layer_in_rec(cand[i], r);
        unused[i] = !used;
    }
    /* Masken: frei, wenn keine Ebene (auch keine gleich freigegebene) und
     * kein Eintrag sie mehr benutzt */
    for (j = 0; j < nm; j++) {
        BOOL used = FALSE;
        for (i = 0; i < doc->nlayers && !used; i++)
            used = doc->layers[i]->mask == mcand[j];
        for (i = 0; i < nc && !used; i++)
            used = cand[i]->mask == mcand[j];
        for (r = doc->uhead; r && !used; r = r->next)
            used = mask_in_rec(mcand[j], r);
        if (!used)
            mask_free(mcand[j], doc);
    }
    for (i = 0; i < nc; i++)
        if (unused[i])
            layer_free(cand[i], doc);
}

static UndoRec *undo_alloc(const char *name)
{
    UndoRec *u = AllocVec(sizeof(UndoRec), MEMF_ANY | MEMF_CLEAR);
    LONG max = doc->tw * doc->th;
    if (!u)
        return NULL;
    u->idx = AllocVec(max * sizeof(LONG), MEMF_ANY);
    u->tiles = AllocVec(max * sizeof(Tile *), MEMF_ANY);
    if (!u->idx || !u->tiles) {
        u->next = NULL;
        free_chain(u);
        return NULL;
    }
    strncpy(u->name, name, sizeof(u->name) - 1);
    return u;
}

UndoRec *undo_begin(Layer *l, const char *name)
{
    UndoRec *u = undo_alloc(name);
    if (u)
        u->layer = l;
    return u;
}

/* Eintrag fuer den Arbeitspfad: merkt sich den Pfad vor der Aenderung */
UndoRec *undo_begin_path(const char *name)
{
    UndoRec *u = undo_alloc(name);
    if (!u)
        return NULL;
    u->is_path = TRUE;
    if (doc->wpath && (u->path = AllocVec(sizeof(Path), MEMF_ANY)))
        CopyMem(doc->wpath, u->path, sizeof(Path));
    else if (doc->wpath) {
        free_chain(u);
        return NULL;
    }
    return u;
}

UndoRec *undo_begin_target(const char *name)
{
    UndoRec *u = undo_begin(doc->layers[doc->active], name);
    if (u)
        u->on_mask = doc_target_is_mask();
    return u;
}

void undo_discard(UndoRec *u)
{
    if (u) {
        u->next = NULL;
        free_chain(u);
    }
}

UndoRec *undo_begin_struct(const char *name)
{
    UndoRec *u = undo_alloc(name);
    if (!u)
        return NULL;
    if (!(u->before = AllocVec(sizeof(LayerState), MEMF_ANY))) {
        free_chain(u);
        return NULL;
    }
    state_capture(u->before);
    return u;
}

void undo_add(UndoRec *u, LONG idx, Tile *orig)
{
    if (!u) {
        if (orig)
            FreeVec(orig);
        return;
    }
    u->idx[u->n] = idx;
    u->tiles[u->n] = orig;
    u->n++;
}

void undo_commit(UndoRec *u)
{
    UndoRec *r;
    if (!u)
        return;
    if (!u->before && u->layer && (u->layer->lock & LOCK_ALPHA) && !u->on_mask) {
        LONG i, k;
        for (i = 0; i < u->n; i++) {
            LONG idx = u->idx[i];
            Tile *t = u->layer->tiles[idx], *o = u->tiles[i];
            if (!t)
                continue;
            for (k = 0; k < TILE_PIX; k++)
                t->px[k] = (o ? o->px[k] & 0xFF000000 : 0) | (t->px[k] & 0xFFFFFF);
            tile_update_flag(doc, t, idx % doc->tw, idx / doc->tw);
            doc_mark_dirty(idx % doc->tw, idx / doc->tw);
        }
    }
    if (u->is_path) {
        Path *a = u->path, *b = doc->wpath;
        if ((!a && !b) || (a && b && !memcmp(a, b, sizeof(Path)))) {
            free_chain(u);      /* Pfad unveraendert */
            return;
        }
    } else if (u->before) {
        if ((u->after = AllocVec(sizeof(LayerState), MEMF_ANY)))
            state_capture(u->after);
        if (!u->after || (!u->n && state_equal(u->before, u->after))) {
            free_chain(u);      /* nichts geaendert */
            return;
        }
    } else if (!u->n) {
        free_chain(u);
        return;
    }
    if (doc->size_next) {
        Doc *n = doc->size_next;
        doc->size_next = NULL;
        n->size_prev = NULL;
        doc_free(n);
    }
    /* Redo-Zweig verwerfen */
    r = doc->ucur ? doc->ucur->next : doc->uhead;
    if (doc->ucur)
        doc->ucur->next = NULL;
    else
        doc->uhead = NULL;
    u->prev = doc->ucur;
    u->next = NULL;
    if (doc->ucur)
        doc->ucur->next = u;
    else
        doc->uhead = u;
    doc->ucur = u;
    if (r) {
        LONG n = 0;
        UndoRec *x;
        for (x = r; x; x = x->next)
            n++;
        doc->ucount -= n;
        free_chain(r);
    }
    doc->ucount++;
    doc->modified = TRUE;
    /* aeltesten Eintrag verwerfen */
    while (doc->ucount > UNDO_MAX && doc->uhead && doc->uhead != doc->ucur) {
        r = doc->uhead;
        doc->uhead = r->next;
        doc->uhead->prev = NULL;
        r->next = NULL;
        doc->ucount--;
        free_chain(r);
    }
}

static void undo_swap(UndoRec *u)
{
    LONG i;
    if (u->is_path) {           /* Arbeitspfad tauschen */
        Path *p = doc->wpath;
        doc->wpath = u->path;
        u->path = p;
        view_inval_all();
        return;
    }
    Tile **arr = u->on_mask ? u->layer->mask : u->layer->tiles;
    if (u->has_ovf) {
        /* Ueberhang der Pixel bzw. der Maske tauschen */
        Ovf **po = u->on_mask ? &u->layer->movf : &u->layer->ovf, *o = *po;
        *po = u->ovf;
        u->ovf = o;
    }
    if (!arr)
        return;
    for (i = 0; i < u->n; i++) {
        LONG idx = u->idx[i];
        Tile *t = arr[idx];
        arr[idx] = u->tiles[i];
        u->tiles[i] = t;
        doc_mark_dirty(idx % doc->tw, idx / doc->tw);
    }
}

BOOL undo_step(void)
{
    UndoRec *r;
    if (!doc->ucur)
        return FALSE;
    do {                        /* verkettete Eintraege gemeinsam */
        r = doc->ucur;
        if (r->before)
            state_restore(r->before);
        undo_swap(r);
        doc->ucur = r->prev;
    } while (r->chain && doc->ucur);
    doc->modified = TRUE;
    return TRUE;
}

BOOL redo_step(void)
{
    UndoRec *n = doc->ucur ? doc->ucur->next : doc->uhead;
    if (!n)
        return FALSE;
    for (;;) {
        if (n->after)
            state_restore(n->after);
        undo_swap(n);
        doc->ucur = n;
        n = n->next;
        if (!n || !n->chain)
            break;
    }
    doc->modified = TRUE;
    return TRUE;
}

void undo_clear(void)
{
    UndoRec *r = doc->uhead;
    doc->uhead = doc->ucur = NULL;
    doc->ucount = 0;
    if (r)
        free_chain(r);
}

/* Protokoll: Eintrag 0 = Ausgangszustand, danach die Schritte */
LONG undo_history(const char **names, LONG max, LONG *cur)
{
    UndoRec *r;
    LONG n = 0;
    if (max > 0)
        names[n++] = doc->base_name[0] ? doc->base_name : doc->path[0] ? "Open" : "New";
    *cur = 0;
    for (r = doc->uhead; r && n < max; r = r->next) {
        if (!r->chain)
            names[n++] = r->name;
        if (r == doc->ucur)
            *cur = n - 1;
    }
    return n;
}

void undo_goto(LONG pos)
{
    LONG cur = 0, n = 0;
    UndoRec *r;
    for (r = doc->uhead; r; r = r->next) {
        if (!r->chain)
            n++;
        if (r == doc->ucur)
            cur = n;
    }
    while (cur > pos && undo_step())
        cur--;
    while (cur < pos && redo_step())
        cur++;
}

/* ---- Auf Hintergrundebene reduzieren ---- */

/* Komposit (doc->flat, nur sichtbare Ebenen) als neue Ebene */
static Layer *layer_from_flat(const char *name)
{
    Layer *l;
    LONG tx, ty, x, y;
    doc_process_dirty();
    if (!(l = layer_new(doc, name)))
        return NULL;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
            LONG vw = doc->w - x0, vh = doc->h - y0;
            Tile *t;
            if (doc->flatflag[ty * doc->tw + tx] == TF_EMPTY)
                continue;
            if (!(t = tile_new()))
                continue;
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            for (y = 0; y < vh; y++) {
                const ULONG *s = doc->flat + (y0 + y) * doc->w + x0;
                ULONG *d = t->px + y * TILE_SIZE;
                for (x = 0; x < vw; x++) {
                    ULONG p = s[x], a = p >> 24, inv;
                    if (a == 255 || !a) {
                        d[x] = p;
                        continue;
                    }
                    /* premultiplied -> straight */
                    inv = (255 * 65536 + a / 2) / a;
                    d[x] = (a << 24)
                         | ((((p >> 16) & 0xFF) * inv >> 16) << 16)
                         | ((((p >> 8) & 0xFF) * inv >> 16) << 8)
                         | ((p & 0xFF) * inv >> 16);
                }
            }
            tile_update_flag(doc, t, tx, ty);
            l->tiles[ty * doc->tw + tx] = t;
        }
    return l;
}

void doc_flatten(void)
{
    Layer *l;
    LONG i;
    if (!(l = layer_from_flat("Background")))
        return;
    {
        UndoRec *u = undo_begin_struct("Flatten Image");
        Layer *old[MAX_LAYERS];
        LONG nold = doc->nlayers;
        for (i = 0; i < nold; i++)
            old[i] = doc->layers[i];
        if (!u)
            undo_clear();       /* kein Speicher fuer Undo: Verlauf aufgeben */
        doc->layers[0] = l;
        doc->nlayers = 1;
        doc->active = 0;
        doc_mark_all_dirty();
        if (u)
            undo_commit(u);     /* alte Ebenen gehoeren jetzt dem Undo */
        else
            for (i = 0; i < nold; i++)
                layer_free(old[i], doc);
    }
}

/* ---- Ebenenmasken ---- */

void doc_mask_add(LONG how)
{
    Layer *l = doc->layers[doc->active];
    LONG n = doc->tw * doc->th, i;
    Tile **m;
    UndoRec *u;
    if (l->mask)
        return;
    if (how == MASK_REVEAL_SEL && !doc->hassel)
        how = MASK_REVEAL_ALL;
    if (!(m = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR)))
        return;
    for (i = 0; i < n && how != MASK_REVEAL_ALL; i++) {
        if (!(m[i] = tile_new_gray(how == MASK_HIDE_ALL ? 0 : 255))) {
            mask_free(m, doc);
            status_message("Not enough memory");
            return;
        }
        if (how == MASK_REVEAL_SEL) {
            LONG x0 = (i % doc->tw) << TILE_SHIFT, y0 = (i / doc->tw) << TILE_SHIFT, x, y;
            for (y = 0; y < TILE_SIZE && y0 + y < doc->h; y++)
                for (x = 0; x < TILE_SIZE && x0 + x < doc->w; x++)
                    m[i]->px[(y << TILE_SHIFT) | x] = 0xFF000000 | (doc->sel[(y0 + y) * doc->w + x0 + x] * 0x010101);
        }
    }
    u = undo_begin_struct("Add Layer Mask");
    if (l->movf) {              /* Ueberhang einer frueheren Maske gilt nicht mehr */
        FreeVec(l->movf);
        l->movf = NULL;
    }
    l->mask = m;
    l->mask_on = 1;
    mask_compact(doc, l);
    doc->edit_mask = TRUE;
    doc_mark_all_dirty();
    undo_commit(u);
    if (how == MASK_REVEAL_SEL)
        sel_none();
}

void doc_mask_delete(void)
{
    Layer *l = doc->layers[doc->active];
    UndoRec *u;
    if (!l->mask)
        return;
    u = undo_begin_struct("Delete Layer Mask");
    if (!u)
        undo_clear();
    l->mask = NULL;             /* die Maske gehoert jetzt dem Undo */
    doc->edit_mask = FALSE;
    doc_mark_all_dirty();
    undo_commit(u);
}

void doc_mask_apply(void)
{
    Layer *l = doc->layers[doc->active];
    UndoRec *u;
    LONG i, n = doc->tw * doc->th, k;
    if (!l->mask)
        return;
    u = undo_begin_struct("Apply Layer Mask");
    if (u)
        u->layer = l;
    for (i = 0; i < n; i++) {
        Tile *t = l->tiles[i], *m = l->mask[i];
        if (!t || !m)
            continue;
        undo_add(u, i, tile_clone(t));
        if (l->mask_on)
            for (k = 0; k < TILE_PIX; k++) {
                ULONG p = t->px[k], g = m->px[k] & 0xFF;
                t->px[k] = ((((p >> 24) * g * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
            }
        tile_update_flag(doc, t, i % doc->tw, i / doc->tw);
    }
    l->mask = NULL;
    doc->edit_mask = FALSE;
    doc_mark_all_dirty();
    undo_commit(u);
}

void doc_mask_toggle(void)
{
    Layer *l = doc->layers[doc->active];
    UndoRec *u;
    if (!l->mask)
        return;
    u = undo_begin_struct(l->mask_on ? "Disable Layer Mask" : "Enable Layer Mask");
    l->mask_on ^= 1;
    doc_mark_all_dirty();
    undo_commit(u);
}

void doc_rasterize_text(Layer *l)
{
    UndoRec *u;
    if (!l || !l->text)
        return;
    u = undo_begin_struct("Rasterize Type");
    FreeVec(l->text);
    l->text = NULL;
    undo_commit(u);
}

/* ---- Auswahl fuellen / leeren (Alt+Backspace / Entf) ---- */

void doc_fill_selection(ULONG color, BOOL clear)
{
    Tile **arr;
    UndoRec *u;
    LONG tx, ty, x, y;
    ULONG cr, cg, cb;

    if (!doc->hassel)
        return;
    if (doc_target_is_mask()) {
        /* Maske: Grauwert der Farbe; Loeschen fuellt mit der Hintergrundfarbe */
        ULONG g = mask_gray(clear ? bg_color : color);
        color = g * 0x010101;
        clear = FALSE;
    } else
        doc_rasterize_text(doc->layers[doc->active]);
    arr = doc_target();
    cr = (color >> 16) & 0xFF; cg = (color >> 8) & 0xFF; cb = color & 0xFF;
    u = undo_begin_target(clear ? "Clear" : "Fill");
    for (ty = doc->sely0 >> TILE_SHIFT; ty <= (doc->sely1 - 1) >> TILE_SHIFT; ty++)
        for (tx = doc->selx0 >> TILE_SHIFT; tx <= (doc->selx1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx;
            LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
            LONG vw = doc->w - x0, vh = doc->h - y0;
            Tile *t = arr[idx];
            BOOL any = FALSE;
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            /* betrifft die Auswahl diese Kachel ueberhaupt? */
            for (y = 0; y < vh && !any; y++) {
                const UBYTE *s = doc->sel + (y0 + y) * doc->w + x0;
                for (x = 0; x < vw; x++)
                    if (s[x]) { any = TRUE; break; }
            }
            if (!any || (clear && (!t || t->flag == TF_EMPTY)))
                continue;
            undo_add(u, idx, t ? tile_clone(t) : NULL);
            if (!t) {
                if (!(t = tile_new()))
                    continue;
                arr[idx] = t;
            }
            for (y = 0; y < vh; y++) {
                const UBYTE *s = doc->sel + (y0 + y) * doc->w + x0;
                ULONG *p = t->px + y * TILE_SIZE;
                for (x = 0; x < vw; x++) {
                    ULONG sa = s[x], o = p[x], da, oa, inv;
                    if (!sa)
                        continue;
                    da = ((o >> 24) * (255 - sa) * 257 + 0x8080) >> 16;
                    if (clear) {
                        p[x] = (da << 24) | (o & 0xFFFFFF);
                        continue;
                    }
                    oa = sa + da;
                    inv = inv_tab[oa];
                    p[x] = (oa << 24)
                         | (((cr * sa + ((o >> 16) & 0xFF) * da) * inv >> 16) << 16)
                         | (((cg * sa + ((o >> 8) & 0xFF) * da) * inv >> 16) << 8)
                         | ((cb * sa + (o & 0xFF) * da) * inv >> 16);
                }
            }
            tile_update_flag(doc, t, tx, ty);
            doc_mark_dirty(tx, ty);
        }
    undo_commit(u);
}

/* sichtbare Ebenen zu einer verschmelzen, ausgeblendete bleiben */
void doc_merge_visible(void)
{
    Layer *l, *top = NULL;
    UndoRec *u;
    LONG i, n = 0, pos = 0;
    for (i = 0; i < doc->nlayers; i++)
        if (doc->layers[i]->visible) {
            top = doc->layers[i];
            n++;
        }
    if (n < 2)
        return;
    if (!(l = layer_from_flat(doc->layers[doc->active]->visible ? doc->layers[doc->active]->name : top->name)))
        return;
    u = undo_begin_struct("Merge Visible");
    /* sichtbare entfernen (gehoeren dem Undo), neue an die Stelle der obersten */
    for (i = 0; i < doc->nlayers; ) {
        if (doc->layers[i]->visible) {
            LONG k;
            for (k = i; k < doc->nlayers - 1; k++)
                doc->layers[k] = doc->layers[k + 1];
            doc->nlayers--;
            pos = i;
        } else
            i++;
    }
    for (i = doc->nlayers; i > pos; i--)
        doc->layers[i] = doc->layers[i - 1];
    doc->layers[pos] = l;
    doc->nlayers++;
    doc->active = pos;
    doc_mark_all_dirty();
    undo_commit(u);
}

/* Auswahl der aktiven Ebene als neue Ebene darueber (cut: im Original loeschen) */
void doc_layer_via(BOOL cut)
{
    Layer *src = doc->layers[doc->active], *l;
    UndoRec *u;
    char name[32];
    LONG tx, ty, x, y;
    if (!doc->hassel) {
        if (!cut)
            doc_duplicate_layer();
        return;
    }
    if (doc->nlayers >= MAX_LAYERS)
        return;
    doc_rasterize_text(src);
    sprintf(name, "Layer %ld", (long)++doc->layer_counter);
    if (!(l = layer_new(doc, name)))
        return;
    u = undo_begin_struct(cut ? "Layer via Cut" : "Layer via Copy");
    if (u)
        u->layer = src;
    for (ty = doc->sely0 >> TILE_SHIFT; ty <= (doc->sely1 - 1) >> TILE_SHIFT; ty++)
        for (tx = doc->selx0 >> TILE_SHIFT; tx <= (doc->selx1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx, x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
            LONG vw = doc->w - x0, vh = doc->h - y0;
            Tile *s = src->tiles[idx], *t;
            if (!s || s->flag == TF_EMPTY || !(t = tile_new()))
                continue;
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            if (cut)
                undo_add(u, idx, tile_clone(s));
            for (y = 0; y < vh; y++) {
                const UBYTE *m = doc->sel + (y0 + y) * doc->w + x0;
                ULONG *sp = s->px + (y << TILE_SHIFT), *dp = t->px + (y << TILE_SHIFT);
                for (x = 0; x < vw; x++) {
                    ULONG p = sp[x], a = p >> 24, k = m[x];
                    if (!k || !a)
                        continue;
                    dp[x] = (((a * k * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
                    if (cut)
                        sp[x] = (((a * (255 - k) * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
                }
            }
            tile_update_flag(doc, t, tx, ty);
            if (t->flag == TF_EMPTY)
                FreeVec(t);
            else
                l->tiles[idx] = t;
            if (cut)
                tile_update_flag(doc, s, tx, ty);
        }
    for (x = doc->nlayers; x > doc->active + 1; x--)
        doc->layers[x] = doc->layers[x - 1];
    doc->layers[doc->active + 1] = l;
    doc->nlayers++;
    doc->active++;
    doc_mark_all_dirty();
    undo_commit(u);
}

/* ---- Gruppen ---- */

LONG layer_block(LONG i)
{
    LONG n = 1, k;
    if (i < 0 || i >= doc->nlayers || doc->layers[i]->kind != LK_GROUP)
        return 1;
    for (k = i - 1; k >= 0 && doc->layers[k]->depth > doc->layers[i]->depth; k--)
        n++;
    return n;
}

BOOL layer_shown(LONG i)
{
    LONG d = doc->layers[i]->depth, j;
    if (!doc->layers[i]->visible)
        return FALSE;
    for (j = i + 1; j < doc->nlayers && d > 0; j++)
        if (doc->layers[j]->depth < d) {
            if (!doc->layers[j]->visible)
                return FALSE;
            d = doc->layers[j]->depth;
        }
    return TRUE;
}

/* Block (Ebene oder Gruppe samt Kindern) mit oberstem Index top verschieben:
 * dest = Index des untersten Blockelements nach dem Herausnehmen */
void doc_move_block(LONG top, LONG dest, LONG depth)
{
    Layer *blk[MAX_LAYERS];
    LONG size = layer_block(top), bot = top - size + 1, i, dd;
    if (depth < 0) depth = 0;
    if (depth > MAX_DEPTH - 1) depth = MAX_DEPTH - 1;
    dd = depth - doc->layers[top]->depth;
    for (i = 0; i < size; i++)
        blk[i] = doc->layers[bot + i];
    for (i = bot; i + size < doc->nlayers; i++)
        doc->layers[i] = doc->layers[i + size];
    if (dest < 0) dest = 0;
    if (dest > doc->nlayers - size) dest = doc->nlayers - size;
    for (i = doc->nlayers - 1; i >= dest + size; i--)
        doc->layers[i] = doc->layers[i - size];
    for (i = 0; i < size; i++) {
        LONG nd = blk[i]->depth + dd;
        blk[i]->depth = (UBYTE)(nd < 0 ? 0 : nd > MAX_DEPTH - 1 ? MAX_DEPTH - 1 : nd);
        doc->layers[dest + i] = blk[i];
    }
    doc->active = dest + size - 1;
    doc_mark_all_dirty();
}

/* Strg+] / Strg+[: eine Stufe nach oben/unten, auch in Gruppen hinein und heraus */
void doc_step_layer(BOOL up)
{
    LONG n = doc->nlayers, top = doc->active, size, bot, d, dest, nd;
    UndoRec *u;
    if (top < 0 || top >= n)
        return;
    size = layer_block(top);
    bot = top - size + 1;
    d = nd = doc->layers[top]->depth;
    if (up) {
        LONG j = top + 1;
        if (j >= n)
            return;
        if (doc->layers[j]->depth < d) {            /* aus der Gruppe nach oben */
            dest = j - size + 1;
            nd = d - 1;
        } else if (doc->layers[j]->depth > d) {     /* in die Gruppe darueber (unten hinein) */
            LONG h = j;
            while (h < n - 1 && doc->layers[h]->depth > d)
                h++;
            if (doc->layers[h]->collapsed) {        /* zugeklappt: ueberspringen */
                dest = h - size + 1;
            } else {
                dest = bot;
                nd = d + 1;
            }
        } else
            dest = j - size + 1;
    } else {
        LONG k = bot - 1;
        if (k < 0)
            return;
        if (doc->layers[k]->depth < d) {            /* aus der Gruppe nach unten */
            dest = bot;
            nd = d - 1;
        } else if (doc->layers[k]->kind == LK_GROUP && !doc->layers[k]->collapsed) {
            dest = k;                               /* oben in die Gruppe darunter */
            nd = d + 1;
        } else
            dest = k - layer_block(k) + 1;
    }
    if (d == 0 && nd < 0)
        return;
    u = undo_begin_struct("Layer Order");
    doc_move_block(top, dest, nd);
    undo_commit(u);
}

static Layer *new_group_layer(void)
{
    char name[32];
    Layer *g;
    sprintf(name, "Group %ld", (long)++doc->group_counter);
    if (!(g = layer_new(doc, name)))
        return NULL;
    g->kind = LK_GROUP;
    g->pass = 1;
    return g;
}

static void insert_layer(LONG at, Layer *l)
{
    LONG i;
    for (i = doc->nlayers; i > at; i--)
        doc->layers[i] = doc->layers[i - 1];
    doc->layers[at] = l;
    doc->nlayers++;
}

void doc_new_group(void)
{
    Layer *g;
    UndoRec *u;
    if (doc->nlayers >= MAX_LAYERS || !(g = new_group_layer()))
        return;
    u = undo_begin_struct("New Group");
    g->depth = doc->layers[doc->active]->depth;
    insert_layer(doc->active + 1, g);
    doc->active++;
    doc->edit_mask = FALSE;
    doc_mark_all_dirty();
    undo_commit(u);
}

/* aktive Ebene (bzw. Gruppe) in eine neue Gruppe packen */
void doc_group_layers(void)
{
    LONG top = doc->active, size = layer_block(top), k;
    Layer *g;
    UndoRec *u;
    if (doc->nlayers >= MAX_LAYERS || doc->layers[top]->depth >= MAX_DEPTH - 1 || !(g = new_group_layer()))
        return;
    u = undo_begin_struct("Group Layers");
    g->depth = doc->layers[top]->depth;
    for (k = top - size + 1; k <= top; k++)
        doc->layers[k]->depth++;
    insert_layer(top + 1, g);
    doc->active = top + 1;
    doc->edit_mask = FALSE;
    doc_mark_all_dirty();
    undo_commit(u);
}

void doc_ungroup(void)
{
    LONG top = doc->active, size = layer_block(top), k;
    UndoRec *u;
    if (doc->layers[top]->kind != LK_GROUP)
        return;
    u = undo_begin_struct("Ungroup Layers");
    for (k = top - size + 1; k < top; k++)
        doc->layers[k]->depth--;
    remove_layer(top);
    doc->active = size > 1 ? top - 1 : (top > 0 ? top - 1 : 0);
    doc_mark_all_dirty();
    undo_commit(u);
}

/* Ebenen lo..hi allein zusammensetzen (Rest ausgeblendet) und als neue Ebene liefern */
static Layer *layer_from_range(LONG lo, LONG hi, const char *name)
{
    UBYTE vis[MAX_LAYERS], pass[MAX_LAYERS], op[MAX_LAYERS];
    LONG i, d = doc->layers[hi]->depth;
    Layer *l;
    for (i = 0; i < doc->nlayers; i++) {
        Layer *x = doc->layers[i];
        vis[i] = x->visible; pass[i] = x->pass; op[i] = x->opacity;
        if (i < lo || i > hi)
            x->visible = FALSE;
    }
    /* Gruppen ueber dem Bereich: durchlassen */
    for (i = hi + 1; i < doc->nlayers && d > 0; i++)
        if (doc->layers[i]->depth < d) {
            Layer *x = doc->layers[i];
            x->visible = TRUE; x->pass = 1; x->opacity = 255;
            d = x->depth;
        }
    doc_mark_all_dirty();
    l = layer_from_flat(name);
    for (i = 0; i < doc->nlayers; i++) {
        Layer *x = doc->layers[i];
        x->visible = vis[i]; x->pass = pass[i]; x->opacity = op[i];
    }
    doc_mark_all_dirty();
    return l;
}

/* Bloecke lo..hi durch eine verschmolzene Ebene ersetzen */
static void replace_range(LONG lo, LONG hi, const char *name, const char *uname)
{
    Layer *l;
    UndoRec *u;
    LONG k, d = doc->layers[hi]->depth;
    char nm[32];
    strncpy(nm, name, 31);
    nm[31] = 0;
    if (!(l = layer_from_range(lo, hi, nm)))
        return;
    l->depth = (UBYTE)d;
    u = undo_begin_struct(uname);
    for (k = lo; k <= hi; k++)
        remove_layer(lo);
    insert_layer(lo, l);
    doc->active = lo;
    doc->edit_mask = FALSE;
    doc_mark_all_dirty();
    undo_commit(u);
}

void doc_merge_group(void)
{
    LONG top = doc->active, size = layer_block(top);
    if (doc->layers[top]->kind != LK_GROUP)
        return;
    replace_range(top - size + 1, top, doc->layers[top]->name, "Merge Group");
}

/* ---- Korrekturebenen ---- */

static Layer *new_adjust_layer(LONG cmd)
{
    LONG nt = doc->tw * doc->th, i;
    Layer *l = layer_new(doc, adjust_name(cmd));
    if (!l)
        return NULL;
    l->kind = LK_ADJUST;
    if (!(l->adj = AllocVec(sizeof(AdjInfo), MEMF_ANY)) ||
        !(l->mask = AllocVec(nt * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR))) {
        layer_free(l, doc);
        return NULL;
    }
    adjust_defaults(l->adj, cmd);
    /* Maske: alles zeigen (keine Kacheln) bzw. die Auswahl */
    for (i = 0; i < nt && doc->hassel; i++) {
        Tile *t = tile_new_gray(255);
        if (!t) {
            layer_free(l, doc);
            return NULL;
        }
        if (doc->hassel) {
            LONG x0 = (i % doc->tw) << TILE_SHIFT, y0 = (i / doc->tw) << TILE_SHIFT, x, y;
            for (y = 0; y < TILE_SIZE; y++)
                for (x = 0; x < TILE_SIZE; x++) {
                    LONG gx = x0 + x, gy = y0 + y;
                    ULONG g = gx < doc->w && gy < doc->h ? doc->sel[gy * doc->w + gx] : 0;
                    t->px[(y << TILE_SHIFT) | x] = 0xFF000000 | (g * 0x010101);
                }
        }
        l->mask[i] = t;
    }
    l->mask_on = 1;
    mask_compact(doc, l);
    return l;
}

BOOL doc_new_adjust(LONG cmd, void (*preview)(void))
{
    Layer *l;
    UndoRec *u;
    if (doc->nlayers >= MAX_LAYERS || !(l = new_adjust_layer(cmd)))
        return FALSE;
    u = undo_begin_struct("New Adjustment Layer");
    l->depth = doc->layers[doc->active]->depth;
    insert_layer(doc->active + 1, l);
    doc->active++;
    doc->edit_mask = TRUE;
    doc_mark_all_dirty();
    doc_cache_below(doc->active);
    /* Einstellungen im Properties-Panel; nur Curves braucht den Dialog */
    if (preview && !adjust_dialog(l->adj, preview)) {
        doc_cache_drop();
        remove_layer(doc->active);
        doc->active--;
        undo_discard(u);
        layer_free(l, doc);
        doc_mark_all_dirty();
        return FALSE;
    }
    doc_cache_drop();
    doc_mark_all_dirty();
    undo_commit(u);
    return TRUE;
}

BOOL doc_edit_adjust(void (*preview)(void))
{
    Layer *l = doc->layers[doc->active];
    AdjInfo orig, now;
    UndoRec *u;
    if (l->kind != LK_ADJUST || !l->adj)
        return FALSE;
    orig = *l->adj;
    doc_cache_below(doc->active);
    if (!adjust_dialog(l->adj, preview)) {
        doc_cache_drop();
        *l->adj = orig;
        doc_mark_all_dirty();
        return FALSE;
    }
    doc_cache_drop();
    now = *l->adj;
    *l->adj = orig;
    u = undo_begin_struct(adjust_name(l->adj->cmd));
    *l->adj = now;
    doc_mark_all_dirty();
    undo_commit(u);
    return TRUE;
}

/* Layer > Create/Release Clipping Mask */
void doc_clip_toggle(void)
{
    Layer *l = doc->layers[doc->active];
    UndoRec *u;
    if (l->kind == LK_GROUP || doc->active == 0)
        return;
    u = undo_begin_struct(l->clip ? "Release Clipping Mask" : "Create Clipping Mask");
    l->clip ^= 1;
    doc_mark_all_dirty();
    undo_commit(u);
}

/* ---- Mehrfachauswahl im Ebenen-Panel ---- */

/* Gruppe, in der Ebene i liegt (-1: oberste Ebene) */
static LONG parent_of(LONG i)
{
    LONG d = doc->layers[i]->depth, j;
    for (j = i + 1; j < doc->nlayers; j++)
        if (doc->layers[j]->depth < d)
            return j;
    return -1;
}

/* ausgewaehlte Ebenen (immer mit der aktiven), ohne solche in einer
 * ebenfalls ausgewaehlten Gruppe; aufsteigend. Rueckgabe: Anzahl */
LONG doc_picked(LONG *out)
{
    LONG i, n = 0;
    for (i = 0; i < doc->nlayers; i++) {
        LONG p;
        if (!doc->layers[i]->picked && i != doc->active)
            continue;
        for (p = parent_of(i); p >= 0; p = parent_of(p))
            if (doc->layers[p]->picked || p == doc->active)
                break;
        if (p < 0)
            out[n++] = i;
    }
    return n;
}

void doc_pick_clear(void)
{
    LONG i;
    for (i = 0; i < doc->nlayers; i++)
        doc->layers[i]->picked = 0;
}

void doc_delete_picked(void)
{
    LONG p[MAX_LAYERS], n = doc_picked(p), k, removed = 0;
    UndoRec *u;
    for (k = 0; k < n; k++)
        removed += layer_block(p[k]);
    if (doc->nlayers - removed < 1)
        return;
    u = undo_begin_struct("Delete Layers");
    for (k = n - 1; k >= 0; k--) {          /* von oben: Indizes darunter bleiben gueltig */
        LONG size = layer_block(p[k]), j;
        for (j = 0; j < size; j++)
            remove_layer(p[k] - size + 1);
    }
    doc->active = p[0] - layer_block(p[0]) + 1 - 1;
    if (doc->active < 0)
        doc->active = 0;
    if (doc->active >= doc->nlayers)
        doc->active = doc->nlayers - 1;
    doc_pick_clear();
    undo_commit(u);
}

/* ausgewaehlte Ebenen in eine neue Gruppe an der Stelle der obersten */
void doc_group_picked(const char *name)
{
    LONG p[MAX_LAYERS], n = doc_picked(p), k, i, nm = 0, nr = 0, ins = 0;
    LONG top = p[n - 1], tstart = top - layer_block(top) + 1, D = doc->layers[top]->depth;
    Layer *moved[MAX_LAYERS], *rest[MAX_LAYERS], *g;
    UBYTE inblk[MAX_LAYERS];
    UndoRec *u;
    if (doc->nlayers >= MAX_LAYERS || D >= MAX_DEPTH - 1)
        return;
    memset(inblk, 0, sizeof(inblk));
    for (k = 0; k < n; k++) {
        LONG size = layer_block(p[k]), r = doc->layers[p[k]]->depth;
        for (i = p[k] - size + 1; i <= p[k]; i++) {
            if (doc->layers[i]->depth - r + D + 1 >= MAX_DEPTH)
                return;                     /* zu tief verschachtelt */
            inblk[i] = 1;
        }
    }
    if (!(g = new_group_layer()))
        return;
    u = undo_begin_struct(name);
    for (k = 0; k < n; k++) {
        LONG size = layer_block(p[k]), delta = D + 1 - doc->layers[p[k]]->depth;
        for (i = p[k] - size + 1; i <= p[k]; i++) {
            doc->layers[i]->depth += delta;
            moved[nm++] = doc->layers[i];
        }
    }
    for (i = 0; i < doc->nlayers; i++)
        if (!inblk[i]) {
            if (i < tstart)
                ins++;
            rest[nr++] = doc->layers[i];
        }
    g->depth = D;
    k = 0;
    for (i = 0; i < ins; i++)
        doc->layers[k++] = rest[i];
    for (i = 0; i < nm; i++)
        doc->layers[k++] = moved[i];
    doc->layers[k] = g;
    doc->active = k++;
    for (i = ins; i < nr; i++)
        doc->layers[k++] = rest[i];
    doc->nlayers = k;
    doc->edit_mask = FALSE;
    doc_pick_clear();
    doc_mark_all_dirty();
    undo_commit(u);
}

/* Merge Layers: gruppieren und die Gruppe zusammenfuegen (ein Schritt) */
void doc_merge_picked(void)
{
    UndoRec *g;
    LONG p[MAX_LAYERS], n = doc_picked(p);
    char name[32];
    strncpy(name, doc->layers[p[n - 1]]->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    doc_group_picked("Merge Layers");
    g = doc->ucur;
    if (!g || doc->layers[doc->active]->kind != LK_GROUP)
        return;
    strcpy(doc->layers[doc->active]->name, name);   /* Ergebnis heisst wie die oberste */
    doc_merge_group();
    if (doc->ucur && doc->ucur != g)
        doc->ucur->chain = TRUE;
}

/* genau eine Ebene ausgewaehlt (keine Mehrfachauswahl) */
BOOL doc_picked_one(void)
{
    LONG p[MAX_LAYERS];
    return doc_picked(p) == 1;
}

/* Move Tool, Auto-Select: oberste sichtbare Pixelebene mit Deckung am Punkt
 * (Maske beruecksichtigt); group: deren oberste Gruppe. -1: keine */
LONG doc_layer_at(LONG x, LONG y, BOOL group)
{
    LONG i, idx;
    if (x < 0 || y < 0 || x >= doc->w || y >= doc->h)
        return -1;
    idx = (y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT);
    for (i = doc->nlayers - 1; i >= 0; i--) {
        Layer *l = doc->layers[i];
        Tile *t = l->tiles[idx];
        LONG k = ((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK);
        ULONG a;
        if (l->kind != LK_PIXEL || !t || !layer_shown(i))
            continue;
        a = t->px[k] >> 24;
        if (l->mask && l->mask_on && l->mask[idx])
            a = a * (l->mask[idx]->px[k] & 0xFF) / 255;
        a = a * l->opacity / 255;
        if (a < 13)             /* fast durchsichtig: durchklicken */
            continue;
        if (group) {
            LONG p;
            while ((p = parent_of(i)) >= 0)
                i = p;
        }
        return i;
    }
    return -1;
}
