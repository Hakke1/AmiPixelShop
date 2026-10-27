/*
 * quant.c - Farbreduktion fuer Amiga-Paletten
 *
 * Palette: Wu-Quantisierer (Xiaolin Wu, Varianz-Minimierung auf einem
 * 32x32x32-Histogramm mit kumulierten Momenten), danach optional k-Means auf
 * den Histogramm-Eintraegen mit wahrnehmungsgewichtetem Abstand ("redmean").
 * OCS/ECS: Eintraege auf 4 Bit je Kanal gerastert (auch waehrend k-Means).
 * EHB: 32 Grundfarben, die halbhellen 32 werden beim k-Means mitgedacht.
 * HAM6/HAM8: Basispalette; je Pixel die beste von Palettenfarbe oder
 * Aenderung eines Kanals des Vorgaengers (Zeilenanfang: Farbe 0).
 * Dithering: Floyd-Steinberg und Atkinson (schlangenfoermig, ausser HAM),
 * geordnet nach Bayer 8x8.
 */
#include <stdlib.h>
#include <string.h>
#include "quant.h"

#define WB 33
#define WN (WB * WB * WB)
#define IDX(r, g, b) ((r) * WB * WB + (g) * WB + (b))

/* ---- Abstand ---- */

static LONG dist(LONG r1, LONG g1, LONG b1, LONG r2, LONG g2, LONG b2)
{
    LONG rm = (r1 + r2) >> 1, dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return (((512 + rm) * dr * dr) >> 8) + 4 * dg * dg + (((767 - rm) * db * db) >> 8);
}

static LONG q4(LONG v)
{
    return ((v * 15 + 127) / 255) * 17;
}

/* ---- Wu ---- */

typedef struct { LONG r0, r1, g0, g1, b0, b1, vol; } Box;
enum { DR, DG, DB };
static LONG *wt, *mr, *mg, *mb;
static double *m2;

static LONG vol(const Box *c, const LONG *m)
{
    return m[IDX(c->r1, c->g1, c->b1)] - m[IDX(c->r1, c->g1, c->b0)] - m[IDX(c->r1, c->g0, c->b1)] +
           m[IDX(c->r1, c->g0, c->b0)] - m[IDX(c->r0, c->g1, c->b1)] + m[IDX(c->r0, c->g1, c->b0)] +
           m[IDX(c->r0, c->g0, c->b1)] - m[IDX(c->r0, c->g0, c->b0)];
}

static double volf(const Box *c, const double *m)
{
    return m[IDX(c->r1, c->g1, c->b1)] - m[IDX(c->r1, c->g1, c->b0)] - m[IDX(c->r1, c->g0, c->b1)] +
           m[IDX(c->r1, c->g0, c->b0)] - m[IDX(c->r0, c->g1, c->b1)] + m[IDX(c->r0, c->g1, c->b0)] +
           m[IDX(c->r0, c->g0, c->b1)] - m[IDX(c->r0, c->g0, c->b0)];
}

static LONG bottom(const Box *c, LONG dir, const LONG *m)
{
    switch (dir) {
    case DR:
        return -m[IDX(c->r0, c->g1, c->b1)] + m[IDX(c->r0, c->g1, c->b0)] +
               m[IDX(c->r0, c->g0, c->b1)] - m[IDX(c->r0, c->g0, c->b0)];
    case DG:
        return -m[IDX(c->r1, c->g0, c->b1)] + m[IDX(c->r1, c->g0, c->b0)] +
               m[IDX(c->r0, c->g0, c->b1)] - m[IDX(c->r0, c->g0, c->b0)];
    default:
        return -m[IDX(c->r1, c->g1, c->b0)] + m[IDX(c->r1, c->g0, c->b0)] +
               m[IDX(c->r0, c->g1, c->b0)] - m[IDX(c->r0, c->g0, c->b0)];
    }
}

static LONG top(const Box *c, LONG dir, LONG pos, const LONG *m)
{
    switch (dir) {
    case DR:
        return m[IDX(pos, c->g1, c->b1)] - m[IDX(pos, c->g1, c->b0)] -
               m[IDX(pos, c->g0, c->b1)] + m[IDX(pos, c->g0, c->b0)];
    case DG:
        return m[IDX(c->r1, pos, c->b1)] - m[IDX(c->r1, pos, c->b0)] -
               m[IDX(c->r0, pos, c->b1)] + m[IDX(c->r0, pos, c->b0)];
    default:
        return m[IDX(c->r1, c->g1, pos)] - m[IDX(c->r1, c->g0, pos)] -
               m[IDX(c->r0, c->g1, pos)] + m[IDX(c->r0, c->g0, pos)];
    }
}

static double var(const Box *c)
{
    double dr = vol(c, mr), dg = vol(c, mg), db = vol(c, mb), w = vol(c, wt);
    if (w <= 0)
        return 0;
    return volf(c, m2) - (dr * dr + dg * dg + db * db) / w;
}

static double maximize(const Box *c, LONG dir, LONG first, LONG last, LONG *cut,
                       LONG wr, LONG wg, LONG wb, LONG ww)
{
    LONG br = bottom(c, dir, mr), bg = bottom(c, dir, mg), bb = bottom(c, dir, mb), bw = bottom(c, dir, wt), i;
    double best = 0;
    *cut = -1;
    for (i = first; i < last; i++) {
        double hr = br + top(c, dir, i, mr), hg = bg + top(c, dir, i, mg), hb = bb + top(c, dir, i, mb);
        double hw = bw + top(c, dir, i, wt), t;
        if (hw <= 0)
            continue;
        t = (hr * hr + hg * hg + hb * hb) / hw;
        hr = wr - hr; hg = wg - hg; hb = wb - hb; hw = ww - hw;
        if (hw <= 0)
            continue;
        t += (hr * hr + hg * hg + hb * hb) / hw;
        if (t > best) {
            best = t;
            *cut = i;
        }
    }
    return best;
}

static BOOL cut(Box *s1, Box *s2)
{
    LONG wr = vol(s1, mr), wg = vol(s1, mg), wb = vol(s1, mb), ww = vol(s1, wt), cr, cg, cb, dir;
    double xr = maximize(s1, DR, s1->r0 + 1, s1->r1, &cr, wr, wg, wb, ww);
    double xg = maximize(s1, DG, s1->g0 + 1, s1->g1, &cg, wr, wg, wb, ww);
    double xb = maximize(s1, DB, s1->b0 + 1, s1->b1, &cb, wr, wg, wb, ww);
    if (xr >= xg && xr >= xb) {
        dir = DR;
        if (cr < 0)
            return FALSE;
    } else if (xg >= xr && xg >= xb)
        dir = DG;
    else
        dir = DB;
    /* ueber lokale Kopien: verkettete Zuweisungen ueber zwei Zeiger
     * uebersetzt m68k-gcc 16 mit -O2/-O3 falsch (zweite Box leer) */
    {
        Box a = *s1, c = *s1;
        switch (dir) {
        case DR:
            a.r1 = cr;
            c.r0 = cr;
            break;
        case DG:
            a.g1 = cg;
            c.g0 = cg;
            break;
        default:
            a.b1 = cb;
            c.b0 = cb;
            break;
        }
        a.vol = (a.r1 - a.r0) * (a.g1 - a.g0) * (a.b1 - a.b0);
        c.vol = (c.r1 - c.r0) * (c.g1 - c.g0) * (c.b1 - c.b0);
        *s1 = a;
        *s2 = c;
    }
    return TRUE;
}

/* Histogramm-Eintraege fuer k-Means */
static LONG npt;
static LONG *pr, *pg, *pb, *pw;

static void free_wu(void)
{
    free(wt); free(mr); free(mg); free(mb); free(m2);
    wt = mr = mg = mb = NULL;
    m2 = NULL;
    free(pr); free(pg); free(pb); free(pw);
    pr = pg = pb = pw = NULL;
    npt = 0;
}

static BOOL build_hist(const ULONG *px, LONG n)
{
    LONG i, r, g, b;
    wt = calloc(WN, sizeof(LONG)); mr = calloc(WN, sizeof(LONG));
    mg = calloc(WN, sizeof(LONG)); mb = calloc(WN, sizeof(LONG));
    m2 = calloc(WN, sizeof(double));
    if (!wt || !mr || !mg || !mb || !m2)
        return FALSE;
    for (i = 0; i < n; i++) {
        ULONG p = px[i];
        LONG k;
        if ((p >> 24) < 128)
            continue;
        r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
        k = IDX((r >> 3) + 1, (g >> 3) + 1, (b >> 3) + 1);
        wt[k]++;
        mr[k] += r; mg[k] += g; mb[k] += b;
        m2[k] += (double)(r * r + g * g + b * b);
    }
    /* Eintraege (vor dem Kumulieren) fuer k-Means merken */
    for (i = 0; i < WN; i++)
        if (wt[i])
            npt++;
    pr = malloc(npt * sizeof(LONG)); pg = malloc(npt * sizeof(LONG));
    pb = malloc(npt * sizeof(LONG)); pw = malloc(npt * sizeof(LONG));
    if (!pr || !pg || !pb || !pw)
        return FALSE;
    npt = 0;
    for (i = 0; i < WN; i++)
        if (wt[i]) {
            pr[npt] = mr[i] / wt[i];
            pg[npt] = mg[i] / wt[i];
            pb[npt] = mb[i] / wt[i];
            pw[npt] = wt[i];
            npt++;
        }
    return TRUE;
}

static void moments(void)
{
    LONG r, g, b, area[WB], ar[WB], ag[WB], ab[WB];
    double a2[WB];
    for (r = 1; r < WB; r++) {
        memset(area, 0, sizeof(area)); memset(ar, 0, sizeof(ar));
        memset(ag, 0, sizeof(ag)); memset(ab, 0, sizeof(ab));
        memset(a2, 0, sizeof(a2));
        for (g = 1; g < WB; g++) {
            LONG line = 0, lr = 0, lg = 0, lb = 0;
            double l2 = 0;
            for (b = 1; b < WB; b++) {
                LONG i = IDX(r, g, b), j = i - WB * WB;
                line += wt[i]; lr += mr[i]; lg += mg[i]; lb += mb[i]; l2 += m2[i];
                area[b] += line; ar[b] += lr; ag[b] += lg; ab[b] += lb; a2[b] += l2;
                wt[i] = wt[j] + area[b];
                mr[i] = mr[j] + ar[b];
                mg[i] = mg[j] + ag[b];
                mb[i] = mb[j] + ab[b];
                m2[i] = m2[j] + a2[b];
            }
        }
    }
}

/* Wu: bis zu k Farben nach out; Rueckgabe Anzahl */
static LONG wu(LONG k, ULONG *out)
{
    Box cube[256];
    double vv[256];
    LONG next = 0, i, j, n = k;
    if (k < 1)
        return 0;
    moments();
    cube[0].r0 = cube[0].g0 = cube[0].b0 = 0;
    cube[0].r1 = cube[0].g1 = cube[0].b1 = 32;
    for (i = 1; i < k; i++) {
        double t;
        if (cut(&cube[next], &cube[i])) {
            vv[next] = cube[next].vol > 1 ? var(&cube[next]) : 0;
            vv[i] = cube[i].vol > 1 ? var(&cube[i]) : 0;
        } else {
            vv[next] = 0;
            i--;
        }
        next = 0;
        t = vv[0];
        for (j = 1; j <= i; j++)
            if (vv[j] > t) {
                t = vv[j];
                next = j;
            }
        if (t <= 0) {
            n = i + 1;
            break;
        }
    }
    for (i = j = 0; i < n; i++) {
        LONG w = vol(&cube[i], wt);
        if (w > 0)
            out[j++] = ((ULONG)(vol(&cube[i], mr) / w) << 16) | ((ULONG)(vol(&cube[i], mg) / w) << 8) |
                       (ULONG)(vol(&cube[i], mb) / w);
    }
    return j;
}

/* ---- k-Means auf den Histogramm-Eintraegen ----
 * lock: Eintraege 0..lock-1 bleiben fest; ehb: Eintraege i+32 = halbe Helligkeit */
static void kmeans(ULONG *c, LONG n, LONG lock, BOOL ehb, BOOL ocs, LONG iters, LONG first)
{
    double *sr = malloc(n * sizeof(double)), *sg = malloc(n * sizeof(double));
    double *sb = malloc(n * sizeof(double)), *sw = malloc(n * sizeof(double));
    LONG it, i, k, nc = ehb ? 64 : n;
    LONG cr[256], cg[256], cb[256];
    if (!sr || !sg || !sb || !sw)
        goto out;
    for (it = 0; it < iters; it++) {
        LONG moved = 0;
        for (k = 0; k < nc; k++) {
            ULONG v = ehb && k >= 32 ? c[k - 32] : c[k];
            cr[k] = (v >> 16) & 0xFF; cg[k] = (v >> 8) & 0xFF; cb[k] = v & 0xFF;
            if (ehb && k >= 32) {
                if (ocs) {
                    cr[k] = ((cr[k] / 17) >> 1) * 17; cg[k] = ((cg[k] / 17) >> 1) * 17; cb[k] = ((cb[k] / 17) >> 1) * 17;
                } else {
                    cr[k] >>= 1; cg[k] >>= 1; cb[k] >>= 1;
                }
            }
        }
        for (k = 0; k < n; k++)
            sr[k] = sg[k] = sb[k] = sw[k] = 0;
        for (i = 0; i < npt; i++) {
            LONG best = first, bd = 0x7FFFFFFF;
            for (k = first; k < nc; k++) {
                LONG d = dist(pr[i], pg[i], pb[i], cr[k], cg[k], cb[k]);
                if (d < bd) {
                    bd = d;
                    best = k;
                }
            }
            if (ehb && best >= 32) {
                /* halbhell: zaehlt fuer die Grundfarbe mit doppelter Helligkeit */
                k = best - 32;
                sr[k] += pw[i] * (double)(pr[i] * 2 > 255 ? 255 : pr[i] * 2);
                sg[k] += pw[i] * (double)(pg[i] * 2 > 255 ? 255 : pg[i] * 2);
                sb[k] += pw[i] * (double)(pb[i] * 2 > 255 ? 255 : pb[i] * 2);
                sw[k] += pw[i];
            } else {
                sr[best] += pw[i] * (double)pr[i];
                sg[best] += pw[i] * (double)pg[i];
                sb[best] += pw[i] * (double)pb[i];
                sw[best] += pw[i];
            }
        }
        for (k = lock; k < n; k++) {
            LONG r, g, b;
            ULONG v;
            if (sw[k] <= 0)
                continue;       /* leer: alter Wert bleibt */
            r = (LONG)(sr[k] / sw[k] + 0.5);
            g = (LONG)(sg[k] / sw[k] + 0.5);
            b = (LONG)(sb[k] / sw[k] + 0.5);
            if (ocs) {
                r = q4(r); g = q4(g); b = q4(b);
            }
            v = ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
            if (v != c[k])
                moved++;
            c[k] = v;
        }
        if (!moved)
            break;
    }
out:
    free(sr); free(sg); free(sb); free(sw);
}

static const ULONG magicwb[8] = {
    0x959595, 0x000000, 0xFFFFFF, 0x3B67A2, 0x7B7B7B, 0xAFAFAF, 0xAA907C, 0xFFA997 };

static UWORD *cache;            /* naechster Index + 1 je 6-Bit-Farbe */

static LONG nearest(const QPalette *pal, LONG r, LONG g, LONG b, LONG n)
{
    LONG key = ((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2), k, best, bd = 0x7FFFFFFF;
    if (cache && cache[key])
        return cache[key] - 1;
    best = k = pal->transp && n > 1 ? 1 : 0;   /* Index 0 bleibt transparenten Pixeln */
    for (; k < n; k++) {
        ULONG v = pal->col[k];
        LONG d = dist(r, g, b, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    if (cache)
        cache[key] = (UWORD)(best + 1);
    return best;
}

/* HAM: bis zu 4 Moeglichkeiten fuer Ziel (r,g,b) nach Vorgaenger (pr,pg,pb):
 * beste Palettenfarbe, oder R/G/B des Vorgaengers aendern. Rueckgabe Anzahl. */
typedef struct { LONG r, g, b, code, d; } HamOpt;

static LONG ham_options(const QPalette *pal, LONG nbase, LONG r, LONG g, LONG b,
                        LONG pr, LONG pg, LONG pb, HamOpt *o)
{
    LONG k, best = nearest(pal, r, g, b, nbase), n = 0, v;
    o[n].r = (pal->col[best] >> 16) & 0xFF; o[n].g = (pal->col[best] >> 8) & 0xFF;
    o[n].b = pal->col[best] & 0xFF; o[n].code = best; o[n].d = dist(r, g, b, o[n].r, o[n].g, o[n].b); n++;
    if (pal->mode == QM_HAM6) {
        LONG qr = (r * 15 + 127) / 255, qg = (g * 15 + 127) / 255, qb = (b * 15 + 127) / 255;
        o[n].r = qr * 17; o[n].g = pg; o[n].b = pb; o[n].code = 0x20 | qr; n++;
        o[n].r = pr; o[n].g = qg * 17; o[n].b = pb; o[n].code = 0x30 | qg; n++;
        o[n].r = pr; o[n].g = pg; o[n].b = qb * 17; o[n].code = 0x10 | qb; n++;
    } else {
        LONG qr = (r + 2) >> 2, qg = (g + 2) >> 2, qb = (b + 2) >> 2;
        if (qr > 63) qr = 63;
        if (qg > 63) qg = 63;
        if (qb > 63) qb = 63;
        v = (qr << 2) | (pr & 3);
        o[n].r = v; o[n].g = pg; o[n].b = pb; o[n].code = 0x80 | qr; n++;
        v = (qg << 2) | (pg & 3);
        o[n].r = pr; o[n].g = v; o[n].b = pb; o[n].code = 0xC0 | qg; n++;
        v = (qb << 2) | (pb & 3);
        o[n].r = pr; o[n].g = pg; o[n].b = v; o[n].code = 0x40 | qb; n++;
    }
    for (k = 1; k < n; k++)
        o[k].d = dist(r, g, b, o[k].r, o[k].g, o[k].b);
    return n;
}

static BOOL palette_core(const ULONG *px, LONG n, const QuantOpts *o, QPalette *pal);

/* HAM: Gesamtfehler einer Probe-Kodierung (ohne Dithering); bad: Pixel mit grossem Fehler */
static void CopyPixels(ULONG *d, const ULONG *s, LONG n)
{
    memcpy(d, s, n * sizeof(ULONG));
}

static double ham_trial(const ULONG *px, LONG w, LONG h, const QPalette *pal, ULONG *bad, LONG *nbad)
{
    LONG x, y, nbase = pal->mode == QM_HAM6 ? 16 : 64;
    double sum = 0;
    HamOpt op[4];
    *nbad = 0;
    cache = calloc(1 << 18, sizeof(UWORD));     /* darf fehlen: dann ohne */
    for (y = 0; y < h; y++) {
        LONG pr = (pal->col[0] >> 16) & 0xFF, pg = (pal->col[0] >> 8) & 0xFF, pb = pal->col[0] & 0xFF;
        for (x = 0; x < w; x++) {
            ULONG p = px[y * w + x];
            LONG r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF, k, n, bi = 0;
            if ((p >> 24) < 128)
                continue;
            n = ham_options(pal, nbase, r, g, b, pr, pg, pb, op);
            for (k = 1; k < n; k++)
                if (op[k].d < op[bi].d)
                    bi = k;
            sum += op[bi].d;
            if (op[bi].d > 600 && bad)          /* deutlich daneben: fuer die Palette wichtig */
                bad[(*nbad)++] = p;
            pr = op[bi].r; pg = op[bi].g; pb = op[bi].b;
        }
    }
    free(cache);
    cache = NULL;
    return sum;
}

BOOL quant_palette_img(const ULONG *px, LONG w, LONG h, const QuantOpts *o, QPalette *pal)
{
    QPalette p2;
    ULONG *bad;
    LONG nbad, i;
    double e1, e2;
    if (!palette_core(px, w * h, o, pal))
        return FALSE;
    if ((pal->mode != QM_HAM6 && pal->mode != QM_HAM8) || w * h > 640 * 512)
        return TRUE;            /* Verfeinerung nur in ueblichen HAM-Groessen (Speicher) */
    /* HAM: Palette auf die Stellen ausrichten, an denen das Aendern eines Kanals
     * nicht reicht (Spruenge); bis zu drei Runden, besser gewinnt */
    if (!(bad = malloc(w * h * 4 * sizeof(ULONG))))
        return TRUE;
    e1 = ham_trial(px, w, h, pal, bad, &nbad);
    for (i = 0; i < 3 && nbad > 64; i++) {
        /* alle Pixel, die schwierigen dreifach gewichtet */
        LONG k, m = nbad;
        for (k = 0; k < nbad; k++)
            bad[m++] = bad[k], bad[m++] = bad[k];
        CopyPixels(bad + m, px, w * h);
        if (!palette_core(bad, m + w * h, o, &p2))
            break;
        e2 = ham_trial(px, w, h, &p2, bad, &nbad);
        if (e2 >= e1)
            break;
        *pal = p2;
        e1 = e2;
    }
    free(bad);
    return TRUE;
}

BOOL quant_palette(const ULONG *px, LONG n, const QuantOpts *o, QPalette *pal)
{
    return palette_core(px, n, o, pal);
}

/* EHB: Eintraege 32-63 als halbe Helligkeit von 0-31 */
static void ehb_halves(QPalette *pal)
{
    LONG i;
    for (i = 0; i < 32; i++) {
        ULONG v = pal->col[i];
        LONG r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
        if (pal->ocs) {
            r = ((r / 17) >> 1) * 17; g = ((g / 17) >> 1) * 17; b = ((b / 17) >> 1) * 17;
        } else {
            r >>= 1; g >>= 1; b >>= 1;
        }
        pal->col[32 + i] = ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
    }
}

static BOOL palette_core(const ULONG *px, LONG n, const QuantOpts *o, QPalette *pal)
{
    LONG k, i, lock = 0, want;
    BOOL ocs = o->palmode == QP_OCS || o->mode == QM_HAM6, transp = FALSE;
    memset(pal, 0, sizeof(*pal));
    pal->mode = o->mode;
    pal->ocs = ocs;
    want = o->mode == QM_EHB ? 32 : o->mode == QM_HAM6 ? 16 : o->mode == QM_HAM8 ? 64 :
           o->ncolors < 2 ? 2 : o->ncolors > 256 ? 256 : o->ncolors;
    /* transparente Pixel (Brush): Index 0 frei halten, nicht bei HAM */
    if (o->mode == QM_NORMAL || o->mode == QM_EHB)
        for (i = 0; i < n && !transp; i++)
            transp = (px[i] >> 24) < 128;
    if (o->palmode == QP_FIXED && o->fixed && o->fixed->n > 0) {
        const QPalette *f = o->fixed;
        LONG m = o->mode == QM_NORMAL ? f->n : want;
        for (i = 0; i < m; i++)
            pal->col[i] = i < f->n ? f->col[i] : 0;
        pal->ocs = f->ocs;
        pal->n = m;
        if (o->mode == QM_EHB) {
            ehb_halves(pal);
            pal->n = 64;
        }
        pal->transp = transp && pal->n > 1;
        return TRUE;
    }
    if (o->palmode == QP_GRAY && o->mode == QM_NORMAL) {
        for (i = 0; i < want; i++) {
            LONG v = i * 255 / (want - 1);
            if (ocs)
                v = q4(v);
            pal->col[i] = ((ULONG)v << 16) | ((ULONG)v << 8) | (ULONG)v;
        }
        pal->n = want;
        pal->transp = transp;   /* Index 0 (Schwarz) wird transparent */
        return TRUE;
    }
    if (!build_hist(px, n)) {
        free_wu();
        return FALSE;
    }
    if (o->palmode == QP_WORKBENCH && o->mode == QM_NORMAL) {
        lock = want < 8 ? want : 8;
        for (i = 0; i < lock; i++)
            pal->col[i] = magicwb[i];   /* bei Transparenz: Grau 0 = Hintergrund */
    } else if (transp && want > 1) {
        pal->col[0] = 0;        /* transparent */
        lock = 1;
    }
    k = lock + wu(want - lock, pal->col + lock);
    if (k < 1) {                /* leeres Bild */
        pal->col[0] = 0;
        k = 1;
    }
    if (ocs)
        for (i = lock; i < k; i++)
            pal->col[i] = ((ULONG)q4((pal->col[i] >> 16) & 0xFF) << 16) |
                          ((ULONG)q4((pal->col[i] >> 8) & 0xFF) << 8) | (ULONG)q4(pal->col[i] & 0xFF);
    if (o->mode == QM_EHB)
        while (k < 32)          /* EHB braucht 32 Grundfarben */
            pal->col[k++] = 0;
    if (o->optimize || o->mode == QM_EHB)
        kmeans(pal->col, k, lock, o->mode == QM_EHB, ocs, o->mode == QM_EHB ? 16 : 10, transp && want > 1);
    if (o->mode == QM_EHB) {
        ehb_halves(pal);
        k = 64;
    }
    pal->n = k;
    pal->transp = transp && want > 1;
    free_wu();
    return TRUE;
}

LONG quant_planes(const QPalette *pal)
{
    LONG p = 1;
    if (pal->mode == QM_HAM6 || pal->mode == QM_EHB)
        return 6;
    if (pal->mode == QM_HAM8)
        return 8;
    while ((1 << p) < pal->n)
        p++;
    return p;
}

/* ---- Abbilden ---- */

static const UBYTE bayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37, 63, 31, 55, 23, 61, 29, 53, 21 };

static LONG clamp(LONG v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

BOOL quant_map(ULONG *px, LONG w, LONG h, LONG stride, const QuantOpts *o, const QPalette *pal, UBYTE *idx)
{
    LONG *err[3], x, y, ew = (w + 4) * 3, spread;
    BOOL ham = pal->mode == QM_HAM6 || pal->mode == QM_HAM8, diffuse = o->dither == QD_FS || o->dither == QD_ATKINSON;
    LONG amt = o->amount < 0 ? 0 : o->amount > 100 ? 100 : o->amount;
    LONG nbase = pal->mode == QM_HAM6 ? 16 : pal->mode == QM_HAM8 ? 64 : pal->n;
    /* Bayer-Ausschlag: etwa ein Palettenabstand */
    {
        LONG c = nbase, s = 1;
        while (s * s * s < c)
            s++;
        spread = ham ? 24 : 255 / (s + 1);
    }
    spread = spread * amt / 100;
    cache = calloc(1 << 18, sizeof(UWORD));
    for (x = 0; x < 3; x++)
        err[x] = calloc(ew, sizeof(LONG));
    if (!err[0] || !err[1] || !err[2]) {
        free(err[0]); free(err[1]); free(err[2]); free(cache);
        cache = NULL;
        return FALSE;
    }
    for (y = 0; y < h; y++) {
        ULONG *row = px + y * stride;
        UBYTE *irow = idx ? idx + y * w : NULL;
        BOOL rev = diffuse && !ham && (y & 1);
        LONG pr0 = (pal->col[0] >> 16) & 0xFF, pg0 = (pal->col[0] >> 8) & 0xFF, pb0 = pal->col[0] & 0xFF;
        LONG *e0 = err[0], *t;
        /* Fehlerzeilen weiterschieben: err[0] = aktuelle Zeile */
        for (x = 0; x < w; x++) {
            LONG xx = rev ? w - 1 - x : x, i = (xx + 2) * 3, r, g, b, k, ri, gi, bi, code = 0;
            ULONG p = row[xx];
            if ((p >> 24) < 128) {
                row[xx] = 0;
                if (irow)
                    irow[xx] = 0;
                continue;
            }
            r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
            if (diffuse) {
                r = clamp(r + e0[i] / 16); g = clamp(g + e0[i + 1] / 16); b = clamp(b + e0[i + 2] / 16);
            } else if (o->dither == QD_ORDERED && spread) {
                LONG t2 = ((LONG)bayer8[((y & 7) << 3) | (xx & 7)] * 2 - 63) * spread / 128;
                r = clamp(r + t2); g = clamp(g + t2); b = clamp(b + t2);
            }
            if (!ham) {
                k = nearest(pal, r, g, b, pal->n);
                ri = (pal->col[k] >> 16) & 0xFF; gi = (pal->col[k] >> 8) & 0xFF; bi = pal->col[k] & 0xFF;
                code = k;
            } else {
                /* HAM: Palettenfarbe oder einen Kanal des Vorgaengers aendern;
                 * mit Vorausschau: was bleibt fuer das naechste Pixel moeglich? */
                HamOpt op[4], nx[4];
                LONG no = ham_options(pal, nbase, r, g, b, pr0, pg0, pb0, op), j, bi2 = 0, bc = 0x7FFFFFFF;
                LONG nr = -1, ng = 0, nb = 0;
                if (xx + 1 < w && (row[xx + 1] >> 24) >= 128) {
                    ULONG q = row[xx + 1];
                    nr = (q >> 16) & 0xFF; ng = (q >> 8) & 0xFF; nb = q & 0xFF;
                }
                for (j = 0; j < no; j++) {
                    LONG c = op[j].d * 2, m = 0x7FFFFFFF, k2, nn;
                    if (nr >= 0) {
                        nn = ham_options(pal, nbase, nr, ng, nb, op[j].r, op[j].g, op[j].b, nx);
                        for (k2 = 0; k2 < nn; k2++)
                            if (nx[k2].d < m)
                                m = nx[k2].d;
                        c += m;
                    }
                    if (c < bc) {
                        bc = c;
                        bi2 = j;
                    }
                }
                ri = op[bi2].r; gi = op[bi2].g; bi = op[bi2].b;
                code = op[bi2].code;
                pr0 = ri; pg0 = gi; pb0 = bi;
            }
            row[xx] = 0xFF000000 | ((ULONG)ri << 16) | ((ULONG)gi << 8) | (ULONG)bi;
            if (irow)
                irow[xx] = (UBYTE)code;
            if (diffuse) {
                /* Fehler (in 1/16, mal Staerke) verteilen; Richtung je Zeile */
                LONG er = (r - ri) * amt / 100, eg = (g - gi) * amt / 100, eb = (b - bi) * amt / 100;
                LONG dx = rev ? -3 : 3, c;
                LONG e[3];
                e[0] = er; e[1] = eg; e[2] = eb;
                for (c = 0; c < 3; c++) {
                    if (o->dither == QD_FS) {
                        e0[i + dx + c] += e[c] * 7;
                        err[1][i - dx + c] += e[c] * 3;
                        err[1][i + c] += e[c] * 5;
                        err[1][i + dx + c] += e[c] * 1;
                    } else {        /* Atkinson: 6 x 1/8, Rest verfaellt */
                        e0[i + dx + c] += e[c] * 2;
                        e0[i + 2 * dx + c] += e[c] * 2;
                        err[1][i - dx + c] += e[c] * 2;
                        err[1][i + c] += e[c] * 2;
                        err[1][i + dx + c] += e[c] * 2;
                        err[2][i + c] += e[c] * 2;
                    }
                }
            }
        }
        t = err[0];
        err[0] = err[1];
        err[1] = err[2];
        err[2] = t;
        memset(err[2], 0, ew * sizeof(LONG));
    }
    free(err[0]); free(err[1]); free(err[2]);
    free(cache);
    cache = NULL;
    return TRUE;
}
