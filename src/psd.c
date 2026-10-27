/*
 * psd.c - Photoshop-Dateien (.psd) mit Ebenen laden
 *
 * Unterstuetzt: RGB und Graustufen, 8 Bit, unkomprimiert oder RLE (PackBits).
 * Uebernommen werden Ebenen mit Name (Unicode), Deckkraft, Fill, Sichtbarkeit,
 * Fuellmethode, Schnittmaske, Ebenenmaske und Gruppen (auch Pass Through).
 * Pixel ausserhalb der Flaeche landen im Ueberhang der Ebene.
 * Text- und Smart-Objekt-Ebenen kommen als Pixel. Korrekturebenen Levels,
 * Hue/Saturation, Brightness/Contrast, Invert, Threshold und Posterize werden
 * uebernommen, andere als ausgeblendete leere Ebene angelegt. Einfache
 * Ebenenstile (lrFX: Schlagschatten, Schein nach aussen) kommen mit.
 * Kanaele: RAW, RLE, ZIP (mit und ohne Vorhersage). Ohne Ebenenteil (oder bei anderen
 * Formaten) liefert load_psd NULL mit leerem err: dann laedt stb_image das
 * zusammengesetzte Bild.
 * Die Datei wird gepuffert gelesen (nicht ganz in den Speicher: PSDs werden
 * schnell 100 MB gross).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "apx.h"

extern char *stbi_zlib_decode_malloc_guesssize(const char *buffer, int len, int initial_size, int *outlen);

#define RD_BUF 65536

typedef struct {
    BPTR fh;
    LONG pos, len, idx;         /* Dateilage von buf[0], gueltige Bytes, Lesestelle */
    BOOL bad;
    UBYTE buf[RD_BUF];
} Rd;

static LONG tell(Rd *r) { return r->pos + r->idx; }

static BOOL refill(Rd *r)
{
    r->pos += r->len;
    r->idx = 0;
    r->len = Read(r->fh, r->buf, RD_BUF);
    if (r->len <= 0) {
        r->len = 0;
        r->bad = TRUE;
        return FALSE;
    }
    return TRUE;
}

static void seek_to(Rd *r, LONG at)
{
    if (at >= r->pos && at <= r->pos + r->len) {
        r->idx = at - r->pos;
        return;
    }
    if (Seek(r->fh, at, OFFSET_BEGINNING) < 0)
        r->bad = TRUE;
    r->pos = at;
    r->len = r->idx = 0;
}

static void skip(Rd *r, ULONG n) { seek_to(r, tell(r) + (LONG)n); }

static ULONG u8(Rd *r)
{
    if (r->idx >= r->len && !refill(r))
        return 0;
    return r->buf[r->idx++];
}
static ULONG u16(Rd *r) { ULONG v = u8(r) << 8; return v | u8(r); }
static ULONG u32(Rd *r) { ULONG v = u16(r) << 16; return v | u16(r); }

static BOOL rd_bytes(Rd *r, UBYTE *dst, LONG n)
{
    while (n > 0) {
        LONG k;
        if (r->idx >= r->len && !refill(r))
            return FALSE;
        k = r->len - r->idx;
        if (k > n)
            k = n;
        CopyMem(r->buf + r->idx, dst, k);
        r->idx += k;
        dst += k;
        n -= k;
    }
    return TRUE;
}

#define K(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (ULONG)(d))

static const struct { ULONG key; UBYTE mode; } bmap[] = {
    { K('n','o','r','m'), BM_NORMAL }, { K('d','i','s','s'), BM_NORMAL },
    { K('d','a','r','k'), BM_DARKEN }, { K('m','u','l',' '), BM_MULTIPLY },
    { K('i','d','i','v'), BM_COLORBURN }, { K('l','b','r','n'), BM_LINEARBURN },
    { K('d','k','C','l'), BM_DARKEN }, { K('l','i','t','e'), BM_LIGHTEN },
    { K('s','c','r','n'), BM_SCREEN }, { K('d','i','v',' '), BM_COLORDODGE },
    { K('l','d','d','g'), BM_LINEARDODGE }, { K('l','g','C','l'), BM_LIGHTEN },
    { K('o','v','e','r'), BM_OVERLAY }, { K('s','L','i','t'), BM_SOFTLIGHT },
    { K('h','L','i','t'), BM_HARDLIGHT }, { K('v','L','i','t'), BM_VIVIDLIGHT },
    { K('l','L','i','t'), BM_LINEARLIGHT }, { K('p','L','i','t'), BM_PINLIGHT },
    { K('h','M','i','x'), BM_HARDMIX }, { K('d','i','f','f'), BM_DIFFERENCE },
    { K('s','m','u','d'), BM_EXCLUSION }, { K('f','s','u','b'), BM_SUBTRACT },
    { K('f','d','i','v'), BM_DIVIDE }, { K('h','u','e',' '), BM_HUE },
    { K('s','a','t',' '), BM_SATURATION }, { K('c','o','l','r'), BM_COLOR },
    { K('l','u','m',' '), BM_LUMINOSITY },
};

static UBYTE blend_mode(ULONG key)
{
    LONG i;
    for (i = 0; i < (LONG)(sizeof(bmap) / sizeof(bmap[0])); i++)
        if (bmap[i].key == key)
            return bmap[i].mode;
    return BM_NORMAL;
}

/* ein Kanal (w x h) nach out (Abstand step Bytes); row: Puffer >= 2*w+16;
 * clen: Laenge der Kanaldaten samt Kompressionskennung */
static BOOL read_channel(Rd *r, LONG w, LONG h, UBYTE *out, LONG step, UBYTE *row, ULONG clen)
{
    ULONG comp = u16(r);
    LONG y, x;
    if (w <= 0 || h <= 0)
        return !r->bad;
    if (comp == 2 || comp == 3) {
        /* ZIP (zlib), 3 = mit Vorhersage: jedes Byte als Differenz zum linken */
        char *src, *raw;
        int rawlen = 0;
        if (clen < 3 || !(src = AllocVec(clen - 2, MEMF_ANY)))
            return FALSE;
        if (!rd_bytes(r, (UBYTE *)src, clen - 2)) {
            FreeVec(src);
            return FALSE;
        }
        raw = stbi_zlib_decode_malloc_guesssize(src, clen - 2, w * h, &rawlen);
        FreeVec(src);
        if (!raw || rawlen < w * h) {
            if (raw) free(raw);
            return FALSE;
        }
        for (y = 0; y < h; y++) {
            UBYTE *ln = (UBYTE *)raw + y * w, *o = out + y * w * step;
            if (comp == 3)
                for (x = 1; x < w; x++)
                    ln[x] += ln[x - 1];
            for (x = 0; x < w; x++)
                o[x * step] = ln[x];
        }
        free(raw);
        return TRUE;
    }
    if (comp == 0) {
        for (y = 0; y < h; y++) {
            UBYTE *o = out + y * w * step;
            if (!rd_bytes(r, row, w))
                return FALSE;
            for (x = 0; x < w; x++)
                o[x * step] = row[x];
        }
        return TRUE;
    }
    if (comp == 1) {
        UWORD *cnt = AllocVec(h * 2, MEMF_ANY);
        BOOL ok = TRUE;
        if (!cnt || !rd_bytes(r, (UBYTE *)cnt, h * 2)) {
            if (cnt)
                FreeVec(cnt);
            return FALSE;
        }
        for (y = 0; y < h && ok; y++) {
            LONG len = cnt[y];      /* Big-Endian wie der 68k */
            const UBYTE *s = row, *e = row + len;
            UBYTE *o = out + y * w * step;
            if (len > 2 * w + 16 || !rd_bytes(r, row, len)) {
                ok = FALSE;
                break;
            }
            x = 0;
            while (s < e && x < w) {
                LONG c = (BYTE)*s++;
                if (c >= 0) {
                    for (c++; c > 0 && s < e && x < w; c--)
                        o[(x++) * step] = *s++;
                } else if (c != -128) {
                    UBYTE v = s < e ? *s++ : 0;
                    for (c = 1 - c; c > 0 && x < w; c--)
                        o[(x++) * step] = v;
                }
            }
        }
        FreeVec(cnt);
        return ok;
    }
    return FALSE;               /* ZIP: nicht unterstuetzt */
}

/* UTF-16BE -> UTF-8 (nur bis U+07FF, sonst '?') */
static void utf16_name(Rd *r, char *dst, LONG max)
{
    ULONG n = u32(r), i;
    LONG k = 0;
    for (i = 0; i < n && !r->bad; i++) {
        ULONG c = u16(r);
        if (!c)
            continue;
        if (c < 0x80) {
            if (k + 1 < max) dst[k++] = (char)c;
        } else if (c < 0x800) {
            if (k + 2 < max) {
                dst[k++] = (char)(0xC0 | (c >> 6));
                dst[k++] = (char)(0x80 | (c & 0x3F));
            }
        } else if (k + 1 < max)
            dst[k++] = '?';
    }
    dst[k] = 0;
}

typedef struct {
    LONG top, left, bottom, right, nch;
    WORD chid[32];
    ULONG chlen[32];
    ULONG bkey;
    UBYTE opacity, clip, flags, fill;
    LONG section;               /* lsct: 0 normal, 1/2 Gruppe offen/zu, 3 Gruppenende */
    BOOL pass, adjust;
    LONG mtop, mleft, mbottom, mright;
    UBYTE mdef, mflags;
    BOOL hasmask;
    char name[32];
    LONG adj_cmd;               /* uebernommene Korrektur (CMD_ADJ_*) oder 0 */
    LONG adj_v[6];
    LayerFx fx;
    BOOL has_fx;
} Rec;

/* Farbe aus 2 Byte Farbraum + 4 x 2 Byte (16 Bit je Kanal) */
static ULONG rd_color(Rd *r)
{
    ULONG c0, c1, c2;
    u16(r);
    c0 = u16(r) >> 8;
    c1 = u16(r) >> 8;
    c2 = u16(r) >> 8;
    u16(r);
    return (c0 << 16) | (c1 << 8) | c2;
}

/* Prozent: aeltere Dateien 0..100, manche 0..255 */
static LONG pct(ULONG v)
{
    return v > 100 ? (LONG)(v * 100 / 255) : (LONG)v;
}

/* Wert in Pixeln: ganzzahlig oder 16.16-Festkomma */
static LONG px16(ULONG v)
{
    return v > 0xFFFF ? (LONG)(v >> 16) : (LONG)v;
}

/* einfache Ebeneneffekte (lrFX): Schlagschatten und Schein nach aussen */
static void read_lrfx(Rd *r, Rec *rc, LONG end)
{
    LONG n, i;
    u16(r);                     /* Version */
    n = u16(r);
    for (i = 0; i < n && tell(r) + 12 <= end && !r->bad; i++) {
        ULONG sig = u32(r), key = u32(r), size = u32(r);
        LONG nx = tell(r) + size;
        if (sig != K('8','B','I','M'))
            break;
        if (key == K('d','s','d','w') && size >= 41) {
            LONG blur, angle, dist, on, op;
            ULONG col;
            u32(r);             /* Version */
            blur = px16(u32(r));
            u32(r);             /* Intensitaet */
            angle = (LONG)u32(r);
            dist = px16(u32(r));
            col = rd_color(r);
            u32(r); u32(r);     /* Fuellmethode */
            on = u8(r);
            u8(r);
            op = pct(u8(r));
            rc->fx.sh_on = on != 0;
            rc->fx.sh_opacity = op;
            rc->fx.sh_angle = angle;
            rc->fx.sh_dist = dist;
            rc->fx.sh_size = blur;
            rc->fx.sh_color = col;
            rc->has_fx |= on != 0;
        } else if (key == K('o','g','l','w') && size >= 32) {
            LONG blur, on, op;
            ULONG col;
            u32(r);
            blur = px16(u32(r));
            u32(r);             /* Intensitaet */
            col = rd_color(r);
            u32(r); u32(r);
            on = u8(r);
            op = pct(u8(r));
            rc->fx.gl_on = on != 0;
            rc->fx.gl_opacity = op;
            rc->fx.gl_size = blur;
            rc->fx.gl_color = col;
            rc->has_fx |= on != 0;
        }
        seek_to(r, nx);
    }
}

/* Pixel einer Ebene (Rechteck) auf Kacheln und Ueberhang verteilen */
static const ULONG *pk_buf;
static LONG pk_x, pk_y, pk_w, pk_h;

static ULONG pk_get(LONG x, LONG y)
{
    x -= pk_x;
    y -= pk_y;
    return x >= 0 && y >= 0 && x < pk_w && y < pk_h ? pk_buf[y * pk_w + x] : 0;
}

static BOOL put_pixels(Doc *d, Layer *l, const ULONG *buf, LONG x0, LONG y0, LONG w, LONG h)
{
    LONG x, y;
    for (y = y0 < 0 ? 0 : y0; y < y0 + h && y < d->h; y++)
        for (x = x0 < 0 ? 0 : x0; x < x0 + w && x < d->w; x++) {
            ULONG v = buf[(y - y0) * w + (x - x0)];
            LONG idx = (y >> TILE_SHIFT) * d->tw + (x >> TILE_SHIFT);
            if (!(v >> 24))
                continue;
            if (!l->tiles[idx] && !(l->tiles[idx] = tile_new()))
                return FALSE;
            l->tiles[idx]->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] = v;
        }
    for (x = 0; x < d->tw * d->th; x++)
        if (l->tiles[x])
            tile_update_flag(d, l->tiles[x], x % d->tw, x / d->tw);
    if (x0 < 0 || y0 < 0 || x0 + w > d->w || y0 + h > d->h) {
        pk_buf = buf; pk_x = x0; pk_y = y0; pk_w = w; pk_h = h;
        l->ovf = ovf_build(x0, y0, x0 + w, y0 + h, d->w, d->h, pk_get);
    }
    return TRUE;
}

static BOOL put_mask(Doc *d, Layer *l, const UBYTE *m, Rec *rc)
{
    LONG i, x, y, w = rc->mright - rc->mleft, n = w * (rc->mbottom - rc->mtop);
    /* ganz weiss (innen und aussen): wirkungslos, Speicher sparen */
    if (rc->mdef == 255) {
        for (i = 0; i < n && m[i] == 255; i++)
            ;
        if (i == n)
            return TRUE;
    }
    if (l->mask)                /* Korrekturebene: leere Tabelle schon angelegt */
        FreeVec(l->mask);
    if (!(l->mask = AllocVec(d->tw * d->th * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (i = 0; i < d->tw * d->th; i++)
        if (!(l->mask[i] = tile_new_gray(rc->mdef)))
            return FALSE;
    for (y = rc->mtop < 0 ? 0 : rc->mtop; y < rc->mbottom && y < d->h; y++)
        for (x = rc->mleft < 0 ? 0 : rc->mleft; x < rc->mright && x < d->w; x++) {
            ULONG v = m[(y - rc->mtop) * w + (x - rc->mleft)];
            l->mask[(y >> TILE_SHIFT) * d->tw + (x >> TILE_SHIFT)]->px[((y & TILE_MASK) << TILE_SHIFT) |
                (x & TILE_MASK)] = 0xFF000000 | (v * 0x010101);
        }
    for (i = 0; i < d->tw * d->th; i++)
        tile_update_flag(d, l->mask[i], i % d->tw, i / d->tw);
    l->mask_on = !(rc->mflags & 2);
    mask_compact(d, l);
    return TRUE;
}

static Doc *parse(Rd *r, char *err)
{
    ULONG w, h, depth, mode, lmlen, lilen;
    LONG lmend, liend, nl, i, k, dep = 0, created = 0;
    Rec *rec = NULL;
    Doc *d = NULL;
    UBYTE *row = NULL;
    BOOL first = TRUE;

    if (u32(r) != K('8','B','P','S') || u16(r) != 1)
        return NULL;
    skip(r, 6);
    u16(r);                     /* Kanaele */
    h = u32(r);
    w = u32(r);
    depth = u16(r);
    mode = u16(r);
    if (depth != 8 || (mode != 3 && mode != 1) || w < 1 || h < 1 || w > 16000 || h > 16000)
        return NULL;            /* stb_image versuchen */
    skip(r, u32(r));            /* Farbmodus-Daten */
    skip(r, u32(r));            /* Bild-Ressourcen */
    lmlen = u32(r);
    lmend = tell(r) + lmlen;
    if (r->bad || !lmlen)
        return NULL;
    lilen = u32(r);
    liend = tell(r) + lilen;
    if (!lilen || liend > lmend)
        return NULL;
    nl = (WORD)u16(r);
    if (nl < 0)
        nl = -nl;
    if (!nl || nl > 2000)
        return NULL;
    if (!(rec = AllocVec(nl * sizeof(Rec), MEMF_ANY | MEMF_CLEAR))) {
        strcpy(err, "Not enough memory");
        return NULL;
    }
    /* 1. Ebenen-Datensaetze */
    for (i = 0; i < nl && !r->bad; i++) {
        Rec *rc = &rec[i];
        ULONG nlen;
        LONG eend;
        rc->top = (LONG)u32(r); rc->left = (LONG)u32(r);
        rc->bottom = (LONG)u32(r); rc->right = (LONG)u32(r);
        rc->nch = u16(r);
        if (rc->nch > 32) {
            r->bad = TRUE;
            break;
        }
        for (k = 0; k < rc->nch; k++) {
            rc->chid[k] = (WORD)u16(r);
            rc->chlen[k] = u32(r);
        }
        if (u32(r) != K('8','B','I','M')) {
            r->bad = TRUE;
            break;
        }
        rc->bkey = u32(r);
        rc->opacity = u8(r);
        rc->clip = u8(r);
        rc->flags = u8(r);
        u8(r);
        rc->fill = 255;
        eend = (LONG)u32(r);
        eend += tell(r);
        if (eend > liend) {
            r->bad = TRUE;
            break;
        }
        /* Maske */
        {
            LONG mlen = (LONG)u32(r), mend = tell(r) + mlen;
            if (mlen >= 18) {
                rc->mtop = (LONG)u32(r); rc->mleft = (LONG)u32(r);
                rc->mbottom = (LONG)u32(r); rc->mright = (LONG)u32(r);
                rc->mdef = u8(r);
                rc->mflags = u8(r);
                rc->hasmask = TRUE;
            }
            seek_to(r, mend);
        }
        skip(r, u32(r));        /* Blending Ranges */
        nlen = u8(r);
        {
            UBYTE nb[256];
            rd_bytes(r, nb, nlen);
            for (k = 0; k < (LONG)nlen && k < 31; k++)
                rc->name[k] = nb[k];
            rc->name[k] = 0;
        }
        skip(r, ((nlen + 1 + 3) & ~3) - 1 - nlen);
        /* Zusatzinformationen */
        while (tell(r) + 12 <= eend && !r->bad) {
            ULONG sig = u32(r), key = u32(r), len = u32(r);
            LONG nx = tell(r) + len;
            if (sig != K('8','B','I','M') && sig != K('8','B','6','4'))
                break;
            if (key == K('l','u','n','i'))
                utf16_name(r, rc->name, sizeof(rc->name));
            else if (key == K('l','s','c','t') || key == K('l','s','d','k')) {
                rc->section = u32(r);
                if (len >= 12) {
                    u32(r);
                    rc->pass = u32(r) == K('p','a','s','s');
                }
            } else if (key == K('i','O','p','a'))
                rc->fill = u8(r);
            else if (key == K('l','e','v','l') && len >= 12) {
                /* Version, dann je Kanal 5 Werte; der erste Satz gilt fuer RGB */
                u16(r);
                rc->adj_v[0] = u16(r);          /* Input Black */
                rc->adj_v[2] = u16(r);          /* Input White */
                rc->adj_v[3] = u16(r);          /* Output Black */
                rc->adj_v[4] = u16(r);          /* Output White */
                rc->adj_v[1] = u16(r);          /* Gamma * 100 */
                rc->adj_cmd = CMD_ADJ_LEVELS;
                rc->adjust = TRUE;
            } else if (key == K('h','u','e','2') && len >= 16) {
                LONG colorize;
                u16(r);
                colorize = u8(r);
                u8(r);
                u16(r); u16(r); u16(r);         /* Einfaerben: nicht unterstuetzt */
                rc->adj_v[0] = (WORD)u16(r);    /* Master: Farbton, Saettigung, Helligkeit */
                rc->adj_v[1] = (WORD)u16(r);
                rc->adj_v[2] = (WORD)u16(r);
                rc->adj_cmd = colorize ? 0 : CMD_ADJ_HUESAT;
                rc->adjust = TRUE;
            } else if (key == K('b','r','i','t') && len >= 4) {
                rc->adj_v[0] = (WORD)u16(r);
                rc->adj_v[1] = (WORD)u16(r);
                rc->adj_cmd = CMD_ADJ_BRICON;
                rc->adjust = TRUE;
            } else if (key == K('n','v','r','t')) {
                rc->adj_cmd = CMD_ADJ_INVERT;
                rc->adjust = TRUE;
            } else if (key == K('t','h','r','s') && len >= 2) {
                rc->adj_v[0] = u16(r);
                rc->adj_cmd = CMD_ADJ_THRESH;
                rc->adjust = TRUE;
            } else if (key == K('p','o','s','t') && len >= 2) {
                rc->adj_v[0] = u16(r);
                rc->adj_cmd = CMD_ADJ_POSTER;
                rc->adjust = TRUE;
            } else if (key == K('l','r','F','X') && len >= 4)
                read_lrfx(r, rc, tell(r) + len);
            else if (key == K('l','e','v','l') || key == K('c','u','r','v') || key == K('h','u','e','2') ||
                     key == K('b','r','i','t') || key == K('n','v','r','t') || key == K('p','o','s','t') ||
                     key == K('t','h','r','s') || key == K('b','l','n','c') || key == K('s','e','l','c') ||
                     key == K('m','i','x','r') || key == K('e','x','p','A') || key == K('v','i','b','A') ||
                     key == K('p','h','f','l') || key == K('g','r','d','m') || key == K('C','g','E','d') ||
                     key == K('S','o','C','o') || key == K('G','d','F','l') || key == K('P','t','F','l'))
                rc->adjust = TRUE;
            seek_to(r, nx);
        }
        seek_to(r, eend);
        if (rc->bkey == K('p','a','s','s'))
            rc->pass = TRUE;
        if (rc->section != 3)
            created++;
    }
    if (r->bad)
        goto damaged;
    if (created > MAX_LAYERS) {
        sprintf(err, "This file has %ld layers (AmiPixelShop supports %ld)", (long)created, (long)MAX_LAYERS);
        goto fail;
    }

    if (!(d = doc_new(w, h, DOC_EMPTY)) || !(row = AllocVec(2 * (w > 32000 ? w : 32000) + 16, MEMF_ANY)))
        goto nomem;

    /* 2. Kanaldaten, Ebenen von unten nach oben anlegen */
    for (i = 0; i < nl; i++) {
        Rec *rc = &rec[i];
        LONG lw = rc->right - rc->left, lh = rc->bottom - rc->top;
        LONG mw = rc->mright - rc->mleft, mh = rc->mbottom - rc->mtop;
        ULONG *buf = NULL;
        UBYTE *mbuf = NULL;
        Layer *l;
        LONG chstart = tell(r), off = 0;

        if (lw < 0 || lh < 0 || lw > 30000 || lh > 30000 || mw > 30000 || mh > 30000)
            goto damaged;
        if (rc->section == 3) {         /* Gruppenende (unten): Inhalt beginnt */
            for (k = 0; k < rc->nch; k++)
                off += rc->chlen[k];
            seek_to(r, chstart + off);
            dep++;
            continue;
        }
        if (rc->section == 1 || rc->section == 2)
            dep = dep > 0 ? dep - 1 : 0;
        if (first) {
            l = d->layers[0];
            strcpy(l->name, rc->name);
            first = FALSE;
        } else {
            if (d->nlayers >= MAX_LAYERS || !(l = layer_new(d, rc->name)))
                goto nomem;
            d->layers[d->nlayers++] = l;
        }
        l->opacity = rc->opacity;
        l->fill = rc->fill;
        l->visible = !(rc->flags & 2);
        l->mode = blend_mode(rc->bkey);
        l->clip = rc->clip != 0;
        l->depth = dep < MAX_DEPTH ? dep : MAX_DEPTH - 1;
        if (rc->section == 1 || rc->section == 2) {
            l->kind = LK_GROUP;
            l->pass = rc->pass;
            l->collapsed = rc->section == 2;
            l->clip = 0;
        } else if (rc->adjust && rc->adj_cmd &&
                   (l->adj = AllocVec(sizeof(AdjInfo), MEMF_ANY | MEMF_CLEAR)) &&
                   (l->mask = AllocVec(d->tw * d->th * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR))) {
            /* Korrekturebene: Werte uebernehmen, Maske kommt ggf. aus Kanal -2 */
            LONG k2;
            l->kind = LK_ADJUST;
            l->mask_on = 1;
            adjust_defaults(l->adj, rc->adj_cmd);
            if (rc->adj_cmd != CMD_ADJ_INVERT)
                for (k2 = 0; k2 < 6; k2++)
                    l->adj->v[k2] = rc->adj_v[k2];
            if (rc->adj_cmd == CMD_ADJ_LEVELS) {
                if (!l->adj->v[1])
                    l->adj->v[1] = 100;
                if (!l->adj->v[2])
                    l->adj->v[2] = 255;
                if (!l->adj->v[4])
                    l->adj->v[4] = 255;
            }
        } else if (rc->adjust) {
            if (l->adj) {
                FreeVec(l->adj);
                l->adj = NULL;
            }
            l->visible = 0;     /* Korrekturebene: nicht unterstuetzt */
            strcpy(l->name, "(Adjustment) ");
            strncat(l->name, rc->name, 31 - strlen(l->name));
        }
        if (rc->has_fx && l->kind == LK_PIXEL && (l->fx = AllocVec(sizeof(LayerFx), MEMF_ANY))) {
            *l->fx = rc->fx;
            if (l->fx->st_size < 1)
                l->fx->st_size = 3;
        }
        if (lw > 0 && lh > 0 && l->kind == LK_PIXEL) {
            if (!(buf = AllocVec(lw * lh * 4, MEMF_ANY)))
                goto nomem;
            for (k = 0; k < lw * lh; k++)
                buf[k] = 0xFF000000;
        }
        if (rc->hasmask && mw > 0 && mh > 0 && !(mbuf = AllocVec(mw * mh, MEMF_ANY | MEMF_CLEAR))) {
            if (buf) FreeVec(buf);
            goto nomem;
        }
        for (k = 0; k < rc->nch; k++) {
            WORD id = rc->chid[k];
            seek_to(r, chstart + off);
            if (id == -2 && mbuf)
                read_channel(r, mw, mh, mbuf, 1, row, rc->chlen[k]);
            else if (buf && id >= -1 && id <= 2) {
                /* ARGB in Big-Endian-Bytes: A=0, R=1, G=2, B=3 */
                LONG byte = id == -1 ? 0 : id + 1;
                read_channel(r, lw, lh, (UBYTE *)buf + byte, 4, row, rc->chlen[k]);
            }
            off += rc->chlen[k];
        }
        seek_to(r, chstart + off);
        if (r->bad || chstart + off > liend) {
            if (buf) FreeVec(buf);
            if (mbuf) FreeVec(mbuf);
            goto damaged;
        }
        if (buf && mode == 1)       /* Graustufen: G und B von R */
            for (k = 0; k < lw * lh; k++) {
                ULONG v = (buf[k] >> 16) & 0xFF;
                buf[k] = (buf[k] & 0xFF000000) | (v * 0x010101);
            }
        if (buf) {
            BOOL ok = put_pixels(d, l, buf, rc->left, rc->top, lw, lh);
            FreeVec(buf);
            if (!ok) {
                if (mbuf) FreeVec(mbuf);
                goto nomem;
            }
        }
        if (mbuf) {
            BOOL ok = put_mask(d, l, mbuf, rc);
            FreeVec(mbuf);
            if (!ok)
                goto nomem;
        }
    }
    FreeVec(rec);
    FreeVec(row);
    d->active = d->nlayers - 1;
    while (d->active > 0 && !d->layers[d->active]->visible)
        d->active--;
    d->layer_counter = d->nlayers;
    for (i = 0; i < d->nlayers; i++)
        if (d->layers[i]->kind == LK_GROUP)
            d->group_counter++;
    return d;
damaged:
    strcpy(err, "The PSD file is damaged or uses an unsupported feature");
    goto fail;
nomem:
    strcpy(err, "Not enough memory");
fail:
    if (rec) FreeVec(rec);
    if (row) FreeVec(row);
    if (d) doc_free(d);
    return NULL;
}

Doc *load_psd(const char *path, char *err)
{
    Rd *r = AllocVec(sizeof(Rd), MEMF_ANY | MEMF_CLEAR);
    Doc *d;
    if (!r) {
        strcpy(err, "Not enough memory");
        return NULL;
    }
    if (!(r->fh = Open(path, MODE_OLDFILE))) {
        FreeVec(r);
        strcpy(err, "File not found");
        return NULL;
    }
    d = parse(r, err);
    Close(r->fh);
    FreeVec(r);
    return d;
}
