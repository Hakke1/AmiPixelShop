/*
 * clip.c - Ausschneiden, Kopieren, Einfuegen
 *
 * Intern wird mit Alpha kopiert (ARGB straight). Zusaetzlich landet das Bild
 * als 24-Bit-IFF-ILBM im System-Clipboard (Unit 0), Transparenz auf Weiss.
 * Beim Einfuegen gilt: Steht im Clipboard noch unser eigener Eintrag (gleiche
 * Clip-ID), wird die interne Kopie mit Alpha genommen; hat ein anderes
 * Programm inzwischen etwas abgelegt, wird es ueber die Datatypes gelesen.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/clipboard.h>
#include <libraries/iffparse.h>
#include <datatypes/datatypes.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/iffparse.h>
#include <string.h>
#include <stdio.h>
#include "apx.h"

struct Library *IFFParseBase;

static ULONG *clip_px;          /* ARGB straight */
static LONG clip_w, clip_h, clip_x, clip_y;
static Doc *clip_src;           /* Herkunft (fuer gleiche Position beim Einfuegen) */
static ULONG sys_clip_id;       /* ID unseres letzten Clipboard-Eintrags */

void clip_free(void)
{
    if (clip_px)
        FreeVec(clip_px);
    clip_px = NULL;
    if (IFFParseBase) {
        CloseLibrary(IFFParseBase);
        IFFParseBase = NULL;
    }
}

static BOOL open_iffparse(void)
{
    return IFFParseBase || (IFFParseBase = OpenLibrary("iffparse.library", 39));
}

/* ---- System-Clipboard ---- */

static ULONG sys_clip_current_id(void);

/* 24-Bit-ILBM (planar, unkomprimiert), Transparenz auf Weiss */
static void sys_clip_write(const ULONG *px, LONG w, LONG h)
{
    struct IFFHandle *iff;
    struct ClipboardHandle *ch;
    LONG wb = ((w + 15) >> 4) * 2, x, y, p;
    UBYTE *row;

    if (!open_iffparse() || !(iff = AllocIFF()))
        return;
    if (!(ch = OpenClipboard(PRIMARY_CLIP))) {
        FreeIFF(iff);
        return;
    }
    row = AllocVec(wb * 24, MEMF_ANY);
    iff->iff_Stream = (ULONG)ch;
    InitIFFasClip(iff);
    if (row && !OpenIFF(iff, IFFF_WRITE)) {
        if (!PushChunk(iff, ID_ILBM, ID_FORM, IFFSIZE_UNKNOWN)) {
            struct BitMapHeader bmhd;
            memset(&bmhd, 0, sizeof(bmhd));
            bmhd.bmh_Width = w;
            bmhd.bmh_Height = h;
            bmhd.bmh_Depth = 24;
            bmhd.bmh_Masking = mskNone;
            bmhd.bmh_Compression = 0;
            bmhd.bmh_XAspect = 1;
            bmhd.bmh_YAspect = 1;
            bmhd.bmh_PageWidth = w;
            bmhd.bmh_PageHeight = h;
            if (!PushChunk(iff, 0, ID_BMHD, sizeof(bmhd))) {
                WriteChunkBytes(iff, &bmhd, sizeof(bmhd));
                PopChunk(iff);
            }
            if (!PushChunk(iff, 0, ID_BODY, IFFSIZE_UNKNOWN)) {
                for (y = 0; y < h; y++) {
                    memset(row, 0, wb * 24);
                    for (x = 0; x < w; x++) {
                        ULONG v = px[y * w + x], a = v >> 24, c[3];
                        UBYTE bit = 0x80 >> (x & 7);
                        LONG byte = x >> 3, k;
                        c[0] = (((v >> 16) & 0xFF) * a + 255 * (255 - a)) / 255;
                        c[1] = (((v >> 8) & 0xFF) * a + 255 * (255 - a)) / 255;
                        c[2] = ((v & 0xFF) * a + 255 * (255 - a)) / 255;
                        /* Ebenen 0-7 Rot (LSB zuerst), 8-15 Gruen, 16-23 Blau */
                        for (k = 0; k < 3; k++)
                            for (p = 0; p < 8; p++)
                                if ((c[k] >> p) & 1)
                                    row[(k * 8 + p) * wb + byte] |= bit;
                    }
                    WriteChunkBytes(iff, row, wb * 24);
                }
                PopChunk(iff);
            }
            PopChunk(iff);
        }
        CloseIFF(iff);
    }
    if (row)
        FreeVec(row);
    CloseClipboard(ch);
    FreeIFF(iff);
    /* Kennung so merken, wie sie beim Lesen gemeldet wird (die ID im
     * Schreib-Request passt nicht dazu -> eigener Eintrag galt als fremd) */
    sys_clip_id = sys_clip_current_id();
}

static ULONG sys_clip_current_id(void)
{
    struct ClipboardHandle *ch;
    ULONG id = 0;
    if (!open_iffparse() || !(ch = OpenClipboard(PRIMARY_CLIP)))
        return 0;
    ch->cbh_Req.io_Command = CBD_CURRENTREADID;
    if (!DoIO((struct IORequest *)&ch->cbh_Req))
        id = ch->cbh_Req.io_ClipID;
    CloseClipboard(ch);
    return id;
}

/* Fremdes Bild aus dem System-Clipboard uebernehmen, falls vorhanden */
static void sys_clip_import(void)
{
    ULONG id = sys_clip_current_id();
    LONG w, h;
    char err[80];
    ULONG *buf;
    if (!id || (id == sys_clip_id && clip_px))
        return;                 /* leer oder noch unser eigener Eintrag */
    if (!(buf = dt_read_argb((APTR)PRIMARY_CLIP, DTST_CLIPBOARD, &w, &h, err)))
        return;                 /* kein Bild (z. B. Text): interne Kopie behalten */
    if (clip_px)
        FreeVec(clip_px);
    clip_px = buf;
    clip_w = w;
    clip_h = h;
    clip_x = clip_y = 0;
    clip_src = NULL;
    sys_clip_id = id;
}

/* ---- Kopieren ---- */

static ULONG unpremul_px(ULONG p)
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

BOOL clip_copy(BOOL merged, BOOL cut)
{
    Layer *l = doc->layers[doc->active];
    LONG x0, y0, x1, y1, w, h, x, y;
    ULONG *buf, any = 0;

    if (doc->hassel) {
        x0 = doc->selx0; y0 = doc->sely0;
        x1 = doc->selx1; y1 = doc->sely1;
    } else {
        x0 = y0 = 0;
        x1 = doc->w; y1 = doc->h;
    }
    w = x1 - x0;
    h = y1 - y0;
    if (merged)
        doc_process_dirty();
    if (!(buf = AllocVec(w * h * 4, MEMF_ANY))) {
        status_message("Not enough memory");
        return FALSE;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            LONG gx = x0 + x, gy = y0 + y;
            ULONG p = merged ? unpremul_px(doc->flat[gy * doc->w + gx]) : layer_get(l, gx, gy);
            if (!merged && l->mask) {
                LONG idx = (gy >> TILE_SHIFT) * doc->tw + (gx >> TILE_SHIFT);
                ULONG g = l->mask[idx] ? l->mask[idx]->px[((gy & TILE_MASK) << TILE_SHIFT) | (gx & TILE_MASK)] & 0xFF : 255;
                if (doc_target_is_mask())
                    p = 0xFF000000 | (g * 0x010101);    /* Maske bearbeitet: ihre Grauwerte */
                else if (l->mask_on)
                    p = ((((p >> 24) * g * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
            }
            if (doc->hassel) {
                ULONG s = doc->sel[gy * doc->w + gx];
                p = ((((p >> 24) * s * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
            }
            if (!(p >> 24))
                p = 0;
            buf[y * w + x] = p;
            any |= p;
        }
    if (!any) {
        FreeVec(buf);
        status_message("The selected area is empty");
        return FALSE;
    }
    if (clip_px)
        FreeVec(clip_px);
    clip_px = buf;
    clip_w = w;
    clip_h = h;
    clip_x = x0;
    clip_y = y0;
    clip_src = doc;
    sys_clip_write(buf, w, h);

    if (cut && !merged) {
        if (doc->hassel)
            doc_fill_selection(0, TRUE);
        else {
            /* ohne Auswahl: ganze Ebene leeren */
            sel_all();
            doc_fill_selection(0, TRUE);
            sel_none();
        }
    }
    return TRUE;
}

/* ---- Einfuegen ---- */

BOOL clip_paste(void)
{
    Layer *l;
    UndoRec *u;
    char name[32];
    LONG px0, py0, tx, ty, i;

    sys_clip_import();
    if (!clip_px) {
        status_message("The clipboard contains no image");
        return FALSE;
    }
    if (doc->nlayers >= MAX_LAYERS) {
        status_message("Too many layers");
        return FALSE;
    }
    /* gleiche Stelle im selben Bild, sonst mittig */
    if (clip_src == doc && clip_x + clip_w <= doc->w && clip_y + clip_h <= doc->h) {
        px0 = clip_x;
        py0 = clip_y;
    } else {
        px0 = (doc->w - clip_w) / 2;
        py0 = (doc->h - clip_h) / 2;
    }
    sprintf(name, "Layer %ld", (long)++doc->layer_counter);
    if (!(l = layer_new(doc, name)))
        return FALSE;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            LONG ox = tx << TILE_SHIFT, oy = ty << TILE_SHIFT, y;
            LONG ax0 = px0 > ox ? px0 : ox, ay0 = py0 > oy ? py0 : oy;
            LONG ax1 = px0 + clip_w < ox + TILE_SIZE ? px0 + clip_w : ox + TILE_SIZE;
            LONG ay1 = py0 + clip_h < oy + TILE_SIZE ? py0 + clip_h : oy + TILE_SIZE;
            Tile *t;
            if (ax1 > doc->w) ax1 = doc->w;
            if (ay1 > doc->h) ay1 = doc->h;
            if (ax0 >= ax1 || ay0 >= ay1 || !(t = tile_new()))
                continue;
            for (y = ay0; y < ay1; y++)
                memcpy(t->px + ((y - oy) << TILE_SHIFT) + (ax0 - ox),
                       clip_px + (y - py0) * clip_w + (ax0 - px0), (ax1 - ax0) * 4);
            tile_update_flag(doc, t, tx, ty);
            if (t->flag == TF_EMPTY)
                FreeVec(t);
            else
                l->tiles[ty * doc->tw + tx] = t;
        }
    u = undo_begin_struct("Paste");
    for (i = doc->nlayers; i > doc->active + 1; i--)
        doc->layers[i] = doc->layers[i - 1];
    doc->layers[doc->active + 1] = l;
    doc->nlayers++;
    doc->active++;
    doc_mark_all_dirty();
    undo_commit(u);
    return TRUE;
}

Doc *clip_new_doc(void)
{
    Doc *d;
    sys_clip_import();
    if (!clip_px) {
        status_message("The clipboard contains no image");
        return NULL;
    }
    d = doc_from_argb_pub(clip_w, clip_h, clip_px);
    if (!d)
        status_message("Not enough memory");
    return d;
}
