/*
 * fileio.c - Bilder laden und speichern
 *
 * Laden:     .aps (eigenes Format), sonst stb_image (PNG, JPEG, PSD, GIF,
 *            BMP, TGA), sonst Datatypes (ILBM und alles Installierte)
 * Speichern: .aps (Ebenen), .png (mit Alpha), .jpg/.jpeg (auf Weiss)
 *
 * .aps-Format (big-endian):
 *   "APX1", ULONG version=1, w, h, nlayers, active
 *   je Ebene: char name[32], UBYTE opacity, UBYTE visible, UBYTE mode, UBYTE 0,
 *             UBYTE vorhanden[tw*th], ULONG zlib-Laenge, zlib(Kacheln)
 *   Kachel: 4 Ebenen A, R, G, B zu je 64x64 Bytes, jedes Byte als Differenz
 *           zum linken Nachbarn (erste Spalte: zum oberen), wie PNG "Sub".
 *   ab Version 3 je Ebene danach Abschnitte: ULONG tag, ULONG laenge, Daten;
 *   tag 0 beendet. "MASK": ULONG an, ULONG zlib-Laenge, zlib(Grauwerte aller
 *   Kacheln, je 64x64 Bytes); "TEXT": struct TextInfo.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <datatypes/datatypes.h>
#include <datatypes/pictureclass.h>
#include <intuition/classusr.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/datatypes.h>
#include <clib/alib_protos.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "apx.h"
#include "stb_image.h"
#include "stb_image_write.h"

extern unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

struct Library *DataTypesBase;

#define APS_MAGIC   0x41505831  /* "APX1" */
#define APS_VERSION 3     /* 3: Abschnitte je Ebene (Maske, Text) */
#define TAG_MASK 0x4D41534B /* "MASK" */
#define TAG_TEXT 0x54455854 /* "TEXT" */
#define TAG_LFX  0x4C465820 /* "LFX " */
#define TAG_FLLK 0x464C4C4B /* "FLLK": ULONG fill, ULONG lock */
#define TAG_KIND 0x4B494E44 /* "KIND": ULONG kind, depth, pass, collapsed */
#define TAG_ADJ  0x41444A20 /* "ADJ ": AdjInfo */
#define TAG_CLIP 0x434C4950 /* "CLIP": Schnittmaske (ohne Daten) */
#define TAG_OVF  0x4F564620 /* "OVF ": x, y, w, h, zlib-Laenge, zlib(ARGB) */
#define TAG_MOVF 0x4D4F5646 /* "MOVF": wie OVF, Ueberhang der Maske */
#define TAG_PATH 0x50415448 /* "PATH": nach der letzten Ebene, Arbeitspfad */
#define TAG_AMIG 0x414D4947 /* "AMIG": Farbtiefe und Aufloesung des Dokuments */

/* Kachel -> 4 Kanalebenen mit Differenzfilter */
static void tile_encode(const ULONG *px, UBYTE *out)
{
    LONG c, x, y;
    for (c = 0; c < 4; c++) {
        LONG sh = 24 - c * 8;
        for (y = 0; y < TILE_SIZE; y++) {
            const ULONG *row = px + y * TILE_SIZE;
            UBYTE prev = y ? (UBYTE)(px[(y - 1) * TILE_SIZE] >> sh) : 0;
            for (x = 0; x < TILE_SIZE; x++) {
                UBYTE v = (UBYTE)(row[x] >> sh);
                *out++ = (UBYTE)(v - prev);
                prev = v;
            }
        }
    }
}

static void tile_decode(const UBYTE *in, ULONG *px)
{
    LONG c, x, y;
    memset(px, 0, TILE_PIX * 4);
    for (c = 0; c < 4; c++) {
        LONG sh = 24 - c * 8;
        for (y = 0; y < TILE_SIZE; y++) {
            ULONG *row = px + y * TILE_SIZE;
            UBYTE prev = y ? (UBYTE)(px[(y - 1) * TILE_SIZE] >> sh) : 0;
            for (x = 0; x < TILE_SIZE; x++) {
                prev = (UBYTE)(prev + *in++);
                row[x] |= (ULONG)prev << sh;
            }
        }
    }
}

static BOOL ext_is(const char *path, const char *e)
{
    LONG lp = strlen(path), le = strlen(e), i;
    if (lp < le)
        return FALSE;
    for (i = 0; i < le; i++)
        if (tolower((unsigned char)path[lp - le + i]) != e[i])
            return FALSE;
    return TRUE;
}

/* ---- Dokument aus ARGB-Puffer (straight) ---- */

static Doc *doc_from_argb(LONG w, LONG h, const ULONG *src)
{
    Doc *d = doc_new(w, h, DOC_EMPTY);
    Layer *l;
    LONG tx, ty, y;
    if (!d)
        return NULL;
    l = d->layers[0];
    for (ty = 0; ty < d->th; ty++)
        for (tx = 0; tx < d->tw; tx++) {
            LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
            LONG vw = w - x0, vh = h - y0;
            Tile *t = tile_new();
            if (!t) {
                doc_free(d);
                return NULL;
            }
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            for (y = 0; y < vh; y++)
                CopyMem((APTR)(src + (y0 + y) * w + x0), t->px + y * TILE_SIZE, vw * 4);
            tile_update_flag(d, t, tx, ty);
            if (t->flag == TF_EMPTY)
                FreeVec(t);
            else
                l->tiles[ty * d->tw + tx] = t;
        }
    return d;
}

/* ---- Laden ueber stb_image ---- */

static Doc *load_stb(const char *path)
{
    int w, h, n;
    unsigned char *data = stbi_load(path, &w, &h, &n, 4);
    ULONG *argb;
    Doc *d;
    LONG i;
    if (!data)
        return NULL;
    /* RGBA-Bytes -> ARGB, in place */
    argb = (ULONG *)data;
    for (i = 0; i < (LONG)w * h; i++) {
        ULONG v = argb[i];      /* R G B A (big-endian gelesen) */
        argb[i] = (v >> 8) | (v << 24);
    }
    d = doc_from_argb(w, h, argb);
    stbi_image_free(data);
    return d;
}

/* ---- Laden ueber Datatypes ---- */

/* Bild ueber Datatypes als ARGB lesen (Datei oder Clipboard).
 * name: Dateiname bzw. Clipboard-Unit, srctype: DTST_FILE / DTST_CLIPBOARD */
ULONG *dt_read_argb(APTR name, ULONG srctype, LONG *pw, LONG *ph, char *err)
{
    Object *o;
    struct BitMapHeader *bmh = NULL;
    ULONG *buf;
    LONG w, h, i;
    struct gpLayout gpl;
    struct pdtBlitPixelArray bpa;

    if (!DataTypesBase && !(DataTypesBase = OpenLibrary("datatypes.library", 43))) {
        strcpy(err, "datatypes.library V43 is missing");
        return NULL;
    }
    o = NewDTObject(name,
                    DTA_SourceType, srctype,
                    DTA_GroupID, GID_PICTURE,
                    PDTA_DestMode, PMODE_V43,
                    PDTA_Remap, FALSE,
                    TAG_DONE);
    if (!o) {
        strcpy(err, "Unknown file format");
        return NULL;
    }
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
    if (!bmh) {
        DisposeDTObject(o);
        strcpy(err, "Not a picture");
        return NULL;
    }
    w = bmh->bmh_Width;
    h = bmh->bmh_Height;

    gpl.MethodID = DTM_PROCLAYOUT;
    gpl.gpl_GInfo = NULL;
    gpl.gpl_Initial = 1;
    DoMethodA(o, (Msg)&gpl);

    buf = AllocVec(w * h * 4, MEMF_ANY);
    if (buf) {
        bpa.MethodID = PDTM_READPIXELARRAY;
        bpa.pbpa_PixelData = buf;
        bpa.pbpa_PixelFormat = PBPAFMT_ARGB;
        bpa.pbpa_PixelArrayMod = w * 4;
        bpa.pbpa_Left = 0;
        bpa.pbpa_Top = 0;
        bpa.pbpa_Width = w;
        bpa.pbpa_Height = h;
        DoMethodA(o, (Msg)&bpa);
        if (bmh->bmh_Masking != mskHasAlpha)
            for (i = 0; i < w * h; i++)
                buf[i] |= 0xFF000000;
        *pw = w;
        *ph = h;
    } else
        strcpy(err, "Not enough memory");
    DisposeDTObject(o);
    return buf;
}

static Doc *load_datatypes(const char *path, char *err)
{
    LONG w, h;
    ULONG *buf = dt_read_argb((APTR)path, DTST_FILE, &w, &h, err);
    Doc *d;
    if (!buf)
        return NULL;
    d = doc_from_argb(w, h, buf);
    FreeVec(buf);
    if (!d)
        strcpy(err, "Not enough memory");
    return d;
}

/* oeffentlich fuer die Zwischenablage */
Doc *doc_from_argb_pub(LONG w, LONG h, const ULONG *src)
{
    return doc_from_argb(w, h, src);
}

/* ---- eigenes Format ---- */

static BOOL rd(BPTR fh, APTR p, LONG n) { return Read(fh, p, n) == n; }
static BOOL wr(BPTR fh, CONST_APTR p, LONG n) { return Write(fh, (APTR)p, n) == n; }
static BOOL wr32(BPTR fh, ULONG v) { return wr(fh, &v, 4); }

static BOOL rd32(BPTR fh, ULONG *v) { return Read(fh, v, 4) == 4; }

static BOOL write_chunks(BPTR fh, Doc *d, Layer *l)
{
    LONG nt = d->tw * d->th, i, k;
    if (l->mask) {
        UBYTE *g = malloc(nt * TILE_PIX);
        unsigned char *comp;
        int clen = 0;
        BOOL ok;
        if (!g)
            return FALSE;
        for (i = 0; i < nt; i++)
            for (k = 0; k < TILE_PIX; k++)
                g[i * TILE_PIX + k] = l->mask[i] ? (UBYTE)l->mask[i]->px[k] : 255;
        comp = stbi_zlib_compress(g, nt * TILE_PIX, &clen, 8);
        free(g);
        if (!comp)
            return FALSE;
        ok = wr32(fh, TAG_MASK) && wr32(fh, 8 + clen) && wr32(fh, l->mask_on) &&
             wr32(fh, clen) && wr(fh, comp, clen);
        free(comp);
        if (!ok)
            return FALSE;
    }
    if (l->text && (!wr32(fh, TAG_TEXT) || !wr32(fh, sizeof(TextInfo)) || !wr(fh, l->text, sizeof(TextInfo))))
        return FALSE;
    if (l->movf) {
        Ovf *o = l->movf;
        int clen = 0;
        unsigned char *comp = stbi_zlib_compress((unsigned char *)o->px, o->w * o->h * 4, &clen, 8);
        BOOL ok;
        if (!comp)
            return FALSE;
        ok = wr32(fh, TAG_MOVF) && wr32(fh, 20 + clen) && wr32(fh, o->x) && wr32(fh, o->y) &&
             wr32(fh, o->w) && wr32(fh, o->h) && wr32(fh, clen) && wr(fh, comp, clen);
        free(comp);
        if (!ok)
            return FALSE;
    }
    if (l->ovf) {
        Ovf *o = l->ovf;
        int clen = 0;
        unsigned char *comp = stbi_zlib_compress((unsigned char *)o->px, o->w * o->h * 4, &clen, 8);
        BOOL ok;
        if (!comp)
            return FALSE;
        ok = wr32(fh, TAG_OVF) && wr32(fh, 20 + clen) && wr32(fh, o->x) && wr32(fh, o->y) &&
             wr32(fh, o->w) && wr32(fh, o->h) && wr32(fh, clen) && wr(fh, comp, clen);
        free(comp);
        if (!ok)
            return FALSE;
    }
    if ((l->kind || l->depth) &&
        (!wr32(fh, TAG_KIND) || !wr32(fh, 16) || !wr32(fh, l->kind) || !wr32(fh, l->depth) ||
         !wr32(fh, l->pass) || !wr32(fh, l->collapsed)))
        return FALSE;
    if (l->clip && (!wr32(fh, TAG_CLIP) || !wr32(fh, 0)))
        return FALSE;
    if (l->adj && (!wr32(fh, TAG_ADJ) || !wr32(fh, sizeof(AdjInfo)) || !wr(fh, l->adj, sizeof(AdjInfo))))
        return FALSE;
    if ((l->fill != 255 || l->lock) &&
        (!wr32(fh, TAG_FLLK) || !wr32(fh, 8) || !wr32(fh, l->fill) || !wr32(fh, l->lock)))
        return FALSE;
    if (l->fx && (!wr32(fh, TAG_LFX) || !wr32(fh, sizeof(LayerFx)) || !wr(fh, l->fx, sizeof(LayerFx))))
        return FALSE;
    return wr32(fh, 0);
}

static BOOL read_chunks(BPTR fh, Doc *d, Layer *l, char *err)
{
    LONG nt = d->tw * d->th, i, k;
    for (;;) {
        ULONG tag, len;
        if (!rd32(fh, &tag))
            break;
        if (!tag)
            return TRUE;
        if (!rd32(fh, &len))
            break;
        if (tag == TAG_MASK && len >= 8) {
            ULONG on, clen;
            char *comp, *raw;
            int rawlen = 0;
            if (!rd32(fh, &on) || !rd32(fh, &clen) || !(comp = malloc(clen)))
                break;
            if (!rd(fh, comp, clen)) {
                free(comp);
                break;
            }
            raw = stbi_zlib_decode_malloc_guesssize(comp, clen, nt * TILE_PIX, &rawlen);
            free(comp);
            if (!raw || rawlen != nt * TILE_PIX) {
                if (raw) free(raw);
                break;
            }
            if ((l->mask = AllocVec(nt * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR))) {
                for (i = 0; i < nt; i++)
                    if ((l->mask[i] = tile_new_gray(255)))
                        for (k = 0; k < TILE_PIX; k++)
                            l->mask[i]->px[k] = 0xFF000000 | ((UBYTE)raw[i * TILE_PIX + k] * 0x010101);
                l->mask_on = on ? 1 : 0;
                mask_compact(d, l);
            }
            free(raw);
        } else if ((tag == TAG_OVF || tag == TAG_MOVF) && len >= 20) {
            ULONG v[5];
            char *comp, *raw;
            int rawlen = 0;
            if (!rd(fh, v, 20) || !(comp = malloc(v[4])))
                break;
            if (!rd(fh, comp, v[4])) {
                free(comp);
                break;
            }
            raw = stbi_zlib_decode_malloc_guesssize(comp, v[4], v[2] * v[3] * 4, &rawlen);
            free(comp);
            if (!raw || rawlen != (int)(v[2] * v[3] * 4)) {
                if (raw) free(raw);
                break;
            }
            {
                Ovf *o = ovf_new(v[0], v[1], v[2], v[3]);
                if (o) {
                    CopyMem(raw, o->px, rawlen);
                    if (tag == TAG_MOVF)
                        l->movf = o;
                    else
                        l->ovf = o;
                }
            }
            free(raw);
        } else if (tag == TAG_CLIP && len == 0) {
            l->clip = 1;
        } else if (tag == TAG_KIND && len == 16) {
            ULONG v[4];
            if (!rd(fh, v, 16))
                break;
            l->kind = (UBYTE)(v[0] <= LK_ADJUST ? v[0] : LK_PIXEL);
            l->depth = (UBYTE)(v[1] < MAX_DEPTH ? v[1] : MAX_DEPTH - 1);
            l->pass = (UBYTE)v[2];
            l->collapsed = (UBYTE)v[3];
        } else if (tag == TAG_ADJ && len == sizeof(AdjInfo)) {
            if ((l->adj = AllocVec(sizeof(AdjInfo), MEMF_ANY))) {
                if (!rd(fh, l->adj, sizeof(AdjInfo)))
                    break;
            } else
                Seek(fh, len, OFFSET_CURRENT);
        } else if (tag == TAG_FLLK && len == 8) {
            ULONG v[2];
            if (!rd(fh, v, 8))
                break;
            l->fill = (UBYTE)v[0];
            l->lock = (UBYTE)(v[1] & LOCK_ALL);
        } else if (tag == TAG_LFX && len == sizeof(LayerFx)) {
            if ((l->fx = AllocVec(sizeof(LayerFx), MEMF_ANY))) {
                if (!rd(fh, l->fx, sizeof(LayerFx)))
                    break;
            } else
                Seek(fh, len, OFFSET_CURRENT);
        } else if (tag == TAG_TEXT && (len == sizeof(TextInfo) || len == TEXTINFO_V1 || len == TEXTINFO_V2)) {
            if ((l->text = AllocVec(sizeof(TextInfo), MEMF_ANY | MEMF_CLEAR))) {
                if (!rd(fh, l->text, len))
                    break;
                l->text->str[sizeof(l->text->str) - 1] = 0;
                l->text->font[sizeof(l->text->font) - 1] = 0;
            } else
                Seek(fh, len, OFFSET_CURRENT);
        } else
            Seek(fh, len, OFFSET_CURRENT);  /* unbekannt: ueberspringen */
    }
    strcpy(err, "The file is damaged");
    return FALSE;
}

static Doc *load_aps(const char *path, char *err)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    ULONG hdr[6];
    Doc *d = NULL;
    UBYTE *present = NULL;
    LONG li, i;

    if (!fh) {
        strcpy(err, "File not found");
        return NULL;
    }
    if (!rd(fh, hdr, sizeof(hdr)) || hdr[0] != APS_MAGIC || hdr[1] < 2 || hdr[1] > APS_VERSION ||
        hdr[2] < 1 || hdr[3] < 1 || hdr[2] > 16000 || hdr[3] > 16000 ||
        hdr[4] < 1 || hdr[4] > MAX_LAYERS) {
        strcpy(err, "Not a valid .aps file");
        goto fail;
    }
    if (!(d = doc_new(hdr[2], hdr[3], DOC_EMPTY)) ||
        !(present = AllocVec(d->tw * d->th, MEMF_ANY))) {
        strcpy(err, "Not enough memory");
        goto fail;
    }
    for (li = 0; li < (LONG)hdr[4]; li++) {
        struct { char name[32]; UBYTE opacity, visible, mode, pad; } lh;
        ULONG clen;
        LONG np = 0, k = 0;
        char *comp, *raw;
        int rawlen = 0;
        Layer *l;

        if (!rd(fh, &lh, sizeof(lh)) || !rd(fh, present, d->tw * d->th) || !rd(fh, &clen, 4)) {
            strcpy(err, "The file is damaged");
            goto fail;
        }
        lh.name[31] = 0;
        if (li == 0) {
            l = d->layers[0];
            strcpy(l->name, lh.name);
        } else {
            if (!(l = layer_new(d, lh.name))) {
                strcpy(err, "Not enough memory");
                goto fail;
            }
            d->layers[d->nlayers++] = l;
        }
        l->opacity = lh.opacity;
        l->visible = lh.visible;
        l->mode = lh.mode < BM_COUNT ? lh.mode : BM_NORMAL;
        for (i = 0; i < d->tw * d->th; i++)
            np += present[i] != 0;
        if (np) {
            if (!(comp = malloc(clen))) {
                strcpy(err, "Not enough memory");
                goto fail;
            }
            if (!rd(fh, comp, clen)) {
                free(comp);
                strcpy(err, "The file is damaged");
                goto fail;
            }
            raw = stbi_zlib_decode_malloc_guesssize(comp, clen, np * TILE_PIX * 4, &rawlen);
            free(comp);
            if (!raw || rawlen != np * TILE_PIX * 4) {
                if (raw)
                    free(raw);
                strcpy(err, "The file is damaged");
                goto fail;
            }
            for (i = 0; i < d->tw * d->th; i++) {
                Tile *t;
                if (!present[i])
                    continue;
                if (!(t = tile_new())) {
                    free(raw);
                    strcpy(err, "Not enough memory");
                    goto fail;
                }
                tile_decode((UBYTE *)raw + k * TILE_PIX * 4, t->px);
                k++;
                tile_update_flag(d, t, i % d->tw, i / d->tw);
                l->tiles[i] = t;
            }
            free(raw);
        }
        if (hdr[1] >= 3 && !read_chunks(fh, d, l, err))
            goto fail;
    }
    d->active = hdr[5] < (ULONG)d->nlayers ? (LONG)hdr[5] : d->nlayers - 1;
    {
        /* Bloecke hinter den Ebenen: Dokument-Angaben, Arbeitspfad */
        ULONG tag = 0, len, np, ns, v[2];
        Path *p;
        while (rd32(fh, &tag) && tag == TAG_AMIG && rd32(fh, &len) && len == 8 && rd(fh, v, 8)) {
            d->colors = (LONG)v[0];
            d->ppi = (LONG)v[1];
        }
        if (tag == TAG_PATH && rd32(fh, &len) && rd32(fh, &np) && rd32(fh, &ns) &&
            np <= PATH_MAXPTS && ns <= PATH_MAXSUB && (p = AllocVec(sizeof(Path), MEMF_ANY | MEMF_CLEAR))) {
            BOOL ok = TRUE;
            p->npts = np;
            p->nsub = ns;
            for (i = 0; i < (LONG)ns && ok; i++) {
                ok = rd(fh, v, 8) && v[0] < np;
                p->sub[i] = v[0];
                p->closed[i] = v[1] != 0;
            }
            if (ok && rd(fh, p->pt, np * sizeof(PathPt)))
                d->wpath = p;
            else
                FreeVec(p);
        }
    }
    d->layer_counter = d->nlayers;
    FreeVec(present);
    Close(fh);
    return d;
fail:
    if (present) FreeVec(present);
    if (d) doc_free(d);
    Close(fh);
    return NULL;
}

static BOOL save_aps(Doc *d, const char *path, char *err)
{
    BPTR fh = Open(path, MODE_NEWFILE);
    UBYTE *present = NULL;
    BOOL ok = FALSE;
    LONG li, i, nt = d->tw * d->th;

    if (!fh) {
        strcpy(err, "Cannot create the file");
        return FALSE;
    }
    if (!(present = AllocVec(nt, MEMF_ANY)))
        goto out;
    if (!wr32(fh, APS_MAGIC) || !wr32(fh, APS_VERSION) || !wr32(fh, d->w) || !wr32(fh, d->h) ||
        !wr32(fh, d->nlayers) || !wr32(fh, d->active))
        goto out;
    for (li = 0; li < d->nlayers; li++) {
        Layer *l = d->layers[li];
        struct { char name[32]; UBYTE opacity, visible, mode, pad; } lh;
        LONG np = 0, k = 0;
        UBYTE *raw;
        unsigned char *comp;
        int clen = 0;

        memset(&lh, 0, sizeof(lh));
        strncpy(lh.name, l->name, 31);
        lh.opacity = l->opacity;
        lh.visible = l->visible;
        lh.mode = l->mode;
        for (i = 0; i < nt; i++) {
            present[i] = l->tiles[i] && l->tiles[i]->flag != TF_EMPTY;
            np += present[i];
        }
        if (!wr(fh, &lh, sizeof(lh)) || !wr(fh, present, nt))
            goto out;
        if (!np) {
            if (!wr32(fh, 0) || !write_chunks(fh, d, l))
                goto out;
            continue;
        }
        if (!(raw = malloc(np * TILE_PIX * 4)))
            goto out;
        for (i = 0; i < nt; i++)
            if (present[i])
                tile_encode(l->tiles[i]->px, raw + (k++) * TILE_PIX * 4);
        comp = stbi_zlib_compress(raw, np * TILE_PIX * 4, &clen, 8);
        free(raw);
        if (!comp)
            goto out;
        if (!wr32(fh, clen) || !wr(fh, comp, clen)) {
            free(comp);
            goto out;
        }
        free(comp);
        if (!write_chunks(fh, d, l))
            goto out;
    }
    /* Dokument-Angaben (aeltere Versionen lesen hinter den Ebenen nichts mehr) */
    if ((d->colors || d->ppi) &&
        (!wr32(fh, TAG_AMIG) || !wr32(fh, 8) || !wr32(fh, d->colors) || !wr32(fh, d->ppi)))
        goto out;
    /* Arbeitspfad */
    if (d->wpath && d->wpath->npts) {
        Path *p = d->wpath;
        LONG len = 8 + p->nsub * 8 + p->npts * sizeof(PathPt);
        if (!wr32(fh, TAG_PATH) || !wr32(fh, len) || !wr32(fh, p->npts) || !wr32(fh, p->nsub))
            goto out;
        for (i = 0; i < p->nsub; i++)
            if (!wr32(fh, p->sub[i]) || !wr32(fh, p->closed[i]))
                goto out;
        if (!wr(fh, p->pt, p->npts * sizeof(PathPt)))
            goto out;
    }
    ok = TRUE;
out:
    if (present)
        FreeVec(present);
    Close(fh);
    if (!ok) {
        strcpy(err, "Write error (disk full?)");
        DeleteFile(path);
    }
    return ok;
}

/* ---- Komposit als RGBA-Bytes (straight) ---- */

/* Einzige sichtbare Ebene mit voller Deckkraft? Dann verlustfrei direkt
 * aus den Kacheln exportieren statt aus dem vormultiplizierten Komposit. */
static Layer *single_layer(Doc *d)
{
    Layer *found = NULL;
    LONG i;
    for (i = 0; i < d->nlayers; i++)
        if (d->layers[i]->visible && d->layers[i]->opacity) {
            if (found || d->layers[i]->opacity != 255)
                return NULL;
            found = d->layers[i];
        }
    return found;
}

static UBYTE *flat_rgba(Doc *d, BOOL on_white, BOOL *has_alpha)
{
    UBYTE *buf = malloc(d->w * d->h * 4), *o;
    Layer *l = single_layer(d);
    LONG x, y;
    BOOL alpha = FALSE;
    if (!buf)
        return NULL;
    o = buf;
    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++) {
            ULONG p, a, r, g, b;
            if (l) {
                Tile *t = l->tiles[(y >> TILE_SHIFT) * d->tw + (x >> TILE_SHIFT)];
                p = t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
                a = p >> 24;
                r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
                if (on_white) {
                    r = (r * a + 255 * (255 - a)) / 255;
                    g = (g * a + 255 * (255 - a)) / 255;
                    b = (b * a + 255 * (255 - a)) / 255;
                    a = 255;
                }
            } else {
                p = d->flat[y * d->w + x];
                a = p >> 24;
                r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
                if (on_white) {
                    /* premultiplied ueber Weiss */
                    r += 255 - a; g += 255 - a; b += 255 - a;
                    a = 255;
                } else if (a && a != 255) {
                    r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                }
            }
            if (a != 255)
                alpha = TRUE;
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
            o += 4;
        }
    if (has_alpha)
        *has_alpha = alpha;
    return buf;
}

/* RGBA -> RGB im selben Puffer */
static void rgba_to_rgb(UBYTE *buf, LONG n)
{
    UBYTE *s = buf, *d = buf;
    LONG i;
    for (i = 0; i < n; i++) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        d += 3;
        s += 4;
    }
}

/* ---- oeffentlich ---- */

Doc *file_load(const char *path, char *err)
{
    Doc *d;
    err[0] = 0;
    if (ext_is(path, ".aps"))
        d = load_aps(path, err);
    else if (ext_is(path, ".psd") && ((d = load_psd(path, err)) || err[0])) {
        /* zu gross fuer alle Ebenen: wenigstens das zusammengesetzte Bild */
        if (!d && strstr(err, "memory") && (d = load_stb(path))) {
            err[0] = 0;
            status_message("Not enough memory for all layers - opened the flattened image");
        }
    }
    else if (!(d = load_stb(path)))
        d = load_datatypes(path, err);
    if (d) {
        strncpy(d->path, path, sizeof(d->path) - 1);
        d->modified = FALSE;
    }
    return d;
}

BOOL file_save(Doc *d, const char *path, char *err)
{
    UBYTE *buf;
    BOOL ok, alpha;
    err[0] = 0;

    if (ext_is(path, ".aps"))
        return save_aps(d, path, err);

    doc_process_dirty();
    if (ext_is(path, ".png")) {
        if (!(buf = flat_rgba(d, FALSE, &alpha))) {
            strcpy(err, "Not enough memory");
            return FALSE;
        }
        if (!alpha)
            rgba_to_rgb(buf, d->w * d->h);
        ok = stbi_write_png(path, d->w, d->h, alpha ? 4 : 3, buf, d->w * (alpha ? 4 : 3));
    } else if (ext_is(path, ".jpg") || ext_is(path, ".jpeg")) {
        if (!(buf = flat_rgba(d, TRUE, NULL))) {
            strcpy(err, "Not enough memory");
            return FALSE;
        }
        rgba_to_rgb(buf, d->w * d->h);
        ok = stbi_write_jpg(path, d->w, d->h, 3, buf, 90);
    } else {
        strcpy(err, "Unknown file extension: use .aps, .png or .jpg");
        return FALSE;
    }
    free(buf);
    if (!ok)
        strcpy(err, "Write error");
    return ok;
}
