/*
 * ai.c - KI-Funktionen ueber OpenRouter: Generative Fill und Remove
 *
 * Ablauf: Ausschnitt um die Auswahl (mit Umgebung) als JPEG und die Auswahl
 * als Schwarz-Weiss-Maske an ein Bildmodell schicken. Die Anfrage laeuft in
 * einem eigenen Prozess; das Ergebnis wird auf den Ausschnitt skaliert, in
 * der Farbe an die Umgebung angeglichen und als neue Ebene mit der Auswahl
 * als Ebenenmaske eingesetzt. Das Original bleibt unveraendert.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include "apx.h"
#include "net.h"
#include "stb_image.h"
#include "stb_image_write.h"

extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels, int stride_bytes,
                                            int x, int y, int n, int *out_len);
void main_activate_doc(Doc *d);
BOOL crop_active(void);

#define AI_HOST     "openrouter.ai"
#define AI_MODEL    "google/gemini-nano-banana-2.1"
#define AI_MINSIDE  768         /* lange Seite des gesendeten Ausschnitts */
#define AI_MAXSIDE  1024

typedef struct AiJob {
    NetCtl ctl;
    char *body;                 /* JSON-Anfrage (net_alloc) */
    LONG blen;
    char hdr[320];              /* Kopfzeilen mit Schluessel */
    struct Task *parent;
    LONG sig;
    volatile LONG done;
    BOOL ok;
    char err[160];
    UBYTE *img;                 /* Ergebnisbild, PNG/JPEG (net_alloc) */
    LONG imglen;
    char cost[24];
    /* Platzierung */
    Doc *doc;
    LONG x, y, w, h;            /* Ausschnitt im Dokument */
    UBYTE *sel;                 /* Auswahl im Ausschnitt, w*h */
    char name[32];              /* Ebenenname */
    ULONG t0;
} AiJob;

static AiJob *job;              /* laufende Anfrage oder NULL */
static LONG ai_sigbit = -1;
static char ai_model[96];

/* ---------- Hilfen ---------- */

static BOOL read_small(const char *path, char *buf, LONG max)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;
    buf[0] = 0;
    if (!fh)
        return FALSE;
    n = Read(fh, buf, max - 1);
    Close(fh);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    while (n > 0 && (UBYTE)buf[n - 1] <= ' ')
        buf[--n] = 0;
    return n > 0;
}

static BOOL read_key(char *key, LONG max)
{
    return read_small("ENV:AmiPixelShop/openrouter.key", key, max) ||
           read_small("ENVARC:AmiPixelShop/openrouter.key", key, max);
}

static const char *model_name(void)
{
    if (!read_small("ENV:AmiPixelShop/ai_model", ai_model, sizeof(ai_model)) &&
        !read_small("ENVARC:AmiPixelShop/ai_model", ai_model, sizeof(ai_model)))
        strcpy(ai_model, AI_MODEL);
    return ai_model;
}

/* waechst beim JPEG-Schreiben mit */
typedef struct { UBYTE *p; LONG n, cap; } MemBuf;

static void mem_write(void *ctx, void *data, int size)
{
    MemBuf *m = ctx;
    if (!m->p || m->n + size > m->cap) {
        LONG nc = (m->cap ? m->cap * 2 : 65536) + size;
        UBYTE *q = malloc(nc);
        if (!q)
            return;
        if (m->p) {
            memcpy(q, m->p, m->n);
            free(m->p);
        }
        m->p = q;
        m->cap = nc;
    }
    memcpy(m->p + m->n, data, size);
    m->n += size;
}

/* Seitenverhaeltnisse, die die Bildmodelle liefern */
static const UBYTE ratios[][2] = {
    { 1, 1 }, { 5, 4 }, { 4, 5 }, { 4, 3 }, { 3, 4 }, { 3, 2 }, { 2, 3 },
    { 16, 9 }, { 9, 16 }, { 21, 9 }
};

/* Ausschnitt: Auswahl plus Umgebung, auf ein unterstuetztes Seitenverhaeltnis
 * erweitert; liefert den Index in ratios */
static LONG crop_box(LONG *bx, LONG *by, LONG *bw, LONG *bh)
{
    LONG sw = doc->selx1 - doc->selx0, sh = doc->sely1 - doc->sely0;
    LONG pad = (sw > sh ? sw : sh) * 35 / 100 + 16, best = 0, i;
    LONG x0 = doc->selx0 - pad, y0 = doc->sely0 - pad, x1 = doc->selx1 + pad, y1 = doc->sely1 + pad;
    float a, bd = 1e9f;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    a = (float)(x1 - x0) / (float)(y1 - y0);
    for (i = 0; i < (LONG)(sizeof(ratios) / 2); i++) {
        float r = (float)ratios[i][0] / ratios[i][1];
        float d = r > a ? r / a : a / r;
        /* nur erweitern, wenn das Bild dafuer Platz hat */
        if ((r > a && (y1 - y0) * r > doc->w + 0.5f) || (r < a && (x1 - x0) / r > doc->h + 0.5f))
            continue;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    {
        float r = (float)ratios[best][0] / ratios[best][1];
        LONG w = x1 - x0, h = y1 - y0, nw = w, nh = h;
        if (r > a)
            nw = (LONG)(h * r + 0.5f);
        else
            nh = (LONG)(w / r + 0.5f);
        if (nw > doc->w) nw = doc->w;
        if (nh > doc->h) nh = doc->h;
        x0 -= (nw - w) / 2;
        y0 -= (nh - h) / 2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 + nw > doc->w) x0 = doc->w - nw;
        if (y0 + nh > doc->h) y0 = doc->h - nh;
        *bx = x0;
        *by = y0;
        *bw = nw;
        *bh = nh;
    }
    return best;
}

/* Komposit (premultiplied) bilinear in RGB ueber Weiss abtasten */
static void sample_rgb(UBYTE *dst, LONG dw, LONG dh, LONG bx, LONG by, LONG bw, LONG bh)
{
    LONG x, y;
    for (y = 0; y < dh; y++) {
        float fy = (y + 0.5f) * bh / dh - 0.5f;
        LONG y0 = (LONG)floorf(fy), y1;
        float wy = fy - y0;
        if (y0 < 0) { y0 = 0; wy = 0; }
        y1 = y0 + 1 < bh ? y0 + 1 : bh - 1;
        for (x = 0; x < dw; x++) {
            float fx = (x + 0.5f) * bw / dw - 0.5f;
            LONG x0 = (LONG)floorf(fx), x1, c;
            float wx = fx - x0;
            ULONG p[4];
            if (x0 < 0) { x0 = 0; wx = 0; }
            x1 = x0 + 1 < bw ? x0 + 1 : bw - 1;
            p[0] = doc->flat[(by + y0) * doc->w + bx + x0];
            p[1] = doc->flat[(by + y0) * doc->w + bx + x1];
            p[2] = doc->flat[(by + y1) * doc->w + bx + x0];
            p[3] = doc->flat[(by + y1) * doc->w + bx + x1];
            for (c = 0; c < 3; c++) {
                LONG sh = 16 - c * 8, k;
                float v[4], r;
                for (k = 0; k < 4; k++)
                    v[k] = (float)(((p[k] >> sh) & 0xFF) + 255 - (p[k] >> 24));
                r = (v[0] * (1 - wx) + v[1] * wx) * (1 - wy) + (v[2] * (1 - wx) + v[3] * wx) * wy;
                dst[(y * dw + x) * 3 + c] = r > 255 ? 255 : (UBYTE)(r + 0.5f);
            }
        }
    }
}

/* ---------- Hintergrundprozess ----------
 * Hier keine stdio-Funktionen von libnix (sprintf usw.): im fremden Prozess
 * zerstoerten sie den Auftrag. Nur exec/dos und net.c. */

static void ai_worker(void)
{
    AiJob *j = job;
    NetResp r;
    const char *v;
    LONG vl;

    if (!net_https(AI_HOST, "POST", "/api/v1/chat/completions", j->hdr, j->body, j->blen, &r, &j->ctl)) {
        strcpy(j->err, r.err);
    } else {
        const char *js = (const char *)r.body;
        if (r.status != 200) {
            if ((v = json_get(js, "error.message", &vl)))
                json_str(v, vl, j->err, sizeof(j->err));
            else
                strcpy(j->err, "Server error");
        } else if ((v = json_get(js, "choices.0.message.images.0.image_url.url", &vl))) {
            const char *e = v + vl, *b = v;
            while (b < e && *b != ',')
                b++;
            if (b < e && (j->img = net_alloc(vl)))
                j->imglen = b64_decode(b + 1, e - b - 1, j->img);
            if (j->imglen > 0)
                j->ok = TRUE;
            else
                strcpy(j->err, "Invalid image in the answer");
            if ((v = json_get(js, "usage.cost", &vl)) && vl < (LONG)sizeof(j->cost)) {
                memcpy(j->cost, v, vl);
                j->cost[vl] = 0;
            }
        } else if ((v = json_get(js, "choices.0.message.content", &vl)) && *v == '"') {
            strcpy(j->err, "No image: ");
            json_str(v, vl, j->err + 10, sizeof(j->err) - 10);
        } else if ((v = json_get(js, "error.message", &vl))) {
            json_str(v, vl, j->err, sizeof(j->err));
        } else {
            strcpy(j->err, "The model returned no image");
        }
        net_free(&r);
    }
    Forbid();                   /* Prozess endet, bevor der Hauptprozess weiterlaeuft */
    j->done = 1;
    Signal(j->parent, 1UL << j->sig);
}

static void job_free(AiJob *j)
{
    if (!j)
        return;
    net_freemem(j->body);
    net_freemem(j->img);
    free(j->sel);
    FreeVec(j);
}

/* ---------- Ergebnis einsetzen ---------- */

static BOOL doc_alive(Doc *d)
{
    LONG i;
    for (i = 0; i < ndocs; i++)
        if (docs[i] == d)
            return TRUE;
    return FALSE;
}

/* Maskenrand leicht weich (Kastenfilter, zwei Durchgaenge) */
static void soften(UBYTE *m, LONG w, LONG h, LONG r)
{
    UBYTE *t = malloc(w > h ? w : h);
    LONG pass, x, y;
    if (!t || r < 1) {
        free(t);
        return;
    }
    for (pass = 0; pass < 2; pass++) {
        for (y = 0; y < h; y++) {
            UBYTE *row = m + y * w;
            LONG s = 0, n = 0;
            for (x = -r; x < w + r; x++) {
                if (x + r < w) { s += row[x + r < 0 ? 0 : x + r]; n++; }
                if (x - r - 1 >= 0) { s -= row[x - r - 1]; n--; }
                if (x >= 0 && x < w)
                    t[x] = (s + n / 2) / n;
            }
            memcpy(row, t, w);
        }
        for (x = 0; x < w; x++) {
            LONG s = 0, n = 0;
            for (y = -r; y < h + r; y++) {
                if (y + r < h) { s += m[(y + r < 0 ? 0 : y + r) * w + x]; n++; }
                if (y - r - 1 >= 0) { s -= m[(y - r - 1) * w + x]; n--; }
                if (y >= 0 && y < h)
                    t[y] = (s + n / 2) / n;
            }
            for (y = 0; y < h; y++)
                m[y * w + x] = t[y];
        }
    }
    free(t);
}

static void place_result(AiJob *j)
{
    UBYTE *rgb;
    ULONG *gen;
    int iw, ih, n;
    LONG x, y, tx, ty, i, ntiles;
    LONG sum[3] = { 0, 0, 0 }, cnt = 0, off[3] = { 0, 0, 0 };
    Layer *src, *l;
    Tile **m;
    UndoRec *u;
    char msg[80];

    doc_process_dirty();
    if (!(rgb = stbi_load_from_memory(j->img, j->imglen, &iw, &ih, &n, 3))) {
        status_message("AI: cannot read the returned image");
        return;
    }
    if (!(gen = malloc(j->w * j->h * 4))) {
        stbi_image_free(rgb);
        status_message("Not enough memory");
        return;
    }
    /* auf den Ausschnitt skalieren (bilinear) */
    for (y = 0; y < j->h; y++) {
        float fy = (y + 0.5f) * ih / j->h - 0.5f;
        LONG y0 = (LONG)floorf(fy), y1;
        float wy = fy - y0;
        if (y0 < 0) { y0 = 0; wy = 0; }
        y1 = y0 + 1 < ih ? y0 + 1 : ih - 1;
        for (x = 0; x < j->w; x++) {
            float fx = (x + 0.5f) * iw / j->w - 0.5f;
            LONG x0 = (LONG)floorf(fx), x1, c;
            float wx = fx - x0;
            ULONG p = 0xFF000000;
            if (x0 < 0) { x0 = 0; wx = 0; }
            x1 = x0 + 1 < iw ? x0 + 1 : iw - 1;
            for (c = 0; c < 3; c++) {
                float v = (rgb[(y0 * iw + x0) * 3 + c] * (1 - wx) + rgb[(y0 * iw + x1) * 3 + c] * wx) * (1 - wy) +
                          (rgb[(y1 * iw + x0) * 3 + c] * (1 - wx) + rgb[(y1 * iw + x1) * 3 + c] * wx) * wy;
                p |= (ULONG)(v + 0.5f) << (16 - c * 8);
            }
            gen[y * j->w + x] = p;
        }
    }
    stbi_image_free(rgb);

    /* Farbangleich: mittlere Abweichung ausserhalb der Auswahl, wo das Modell
     * kaum etwas veraendert hat (stark veraenderte Stellen zaehlen nicht) */
    for (y = 0; y < j->h; y += 2)
        for (x = 0; x < j->w; x += 2) {
            ULONG o = doc->flat[(j->y + y) * doc->w + j->x + x], g = gen[y * j->w + x];
            LONG a = o >> 24, d[3], c;
            if (j->sel[y * j->w + x] || a < 255)
                continue;
            for (c = 0; c < 3; c++)
                d[c] = (LONG)((o >> (16 - c * 8)) & 0xFF) - (LONG)((g >> (16 - c * 8)) & 0xFF);
            if (abs(d[0]) + abs(d[1]) + abs(d[2]) > 60)
                continue;
            sum[0] += d[0];
            sum[1] += d[1];
            sum[2] += d[2];
            cnt++;
        }
    if (cnt > 64)
        for (i = 0; i < 3; i++)
            off[i] = sum[i] / cnt;
    if (off[0] || off[1] || off[2])
        for (i = 0; i < j->w * j->h; i++) {
            ULONG p = gen[i], q = 0xFF000000;
            LONG c;
            for (c = 0; c < 3; c++) {
                LONG v = (LONG)((p >> (16 - c * 8)) & 0xFF) + off[c];
                q |= (ULONG)(v < 0 ? 0 : v > 255 ? 255 : v) << (16 - c * 8);
            }
            gen[i] = q;
        }

    /* neue Ebene ueber der aktiven */
    if (doc->nlayers >= MAX_LAYERS || !(l = layer_new(doc, j->name))) {
        free(gen);
        status_message("Too many layers");
        return;
    }
    ntiles = doc->tw * doc->th;
    if (!(m = AllocVec(ntiles * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR))) {
        layer_free(l, doc);
        free(gen);
        return;
    }
    soften(j->sel, j->w, j->h, (j->w > j->h ? j->w : j->h) / 300 + 1);
    for (ty = j->y >> TILE_SHIFT; ty <= (j->y + j->h - 1) >> TILE_SHIFT; ty++)
        for (tx = j->x >> TILE_SHIFT; tx <= (j->x + j->w - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx, x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
            Tile *t = tile_new(), *mt = tile_new_gray(0);
            if (!t || !mt) {
                if (t) FreeVec(t);
                if (mt) FreeVec(mt);
                continue;
            }
            for (y = 0; y < TILE_SIZE; y++) {
                LONG dy = y0 + y - j->y;
                if (dy < 0 || dy >= j->h)
                    continue;
                for (x = 0; x < TILE_SIZE; x++) {
                    LONG dx = x0 + x - j->x;
                    if (dx < 0 || dx >= j->w)
                        continue;
                    t->px[(y << TILE_SHIFT) | x] = gen[dy * j->w + dx];
                    mt->px[(y << TILE_SHIFT) | x] = 0xFF000000 | (j->sel[dy * j->w + dx] * 0x010101);
                }
            }
            tile_update_flag(doc, t, tx, ty);
            l->tiles[idx] = t;
            m[idx] = mt;
        }
    free(gen);

    src = doc->layers[doc->active];
    l->depth = src->depth;
    u = undo_begin_struct("Generative Fill");
    l->mask = m;
    l->mask_on = 1;
    mask_compact(doc, l);
    for (x = doc->nlayers; x > doc->active + 1; x--)
        doc->layers[x] = doc->layers[x - 1];
    doc->layers[doc->active + 1] = l;
    doc->nlayers++;
    doc->active++;
    doc->edit_mask = FALSE;
    doc->modified = TRUE;
    doc_mark_all_dirty();
    undo_commit(u);
    ui_draw_panels();
    view_inval_all();
    if (j->cost[0]) {
        LONG mc = (LONG)(atof(j->cost) * 10000.0 + 0.5);      /* 1/100 Cent */
        sprintf(msg, "Generative Fill: %ld s, %ld.%02ld cents", (long)((now_us() - j->t0) / 1000000),
                (long)(mc / 100), (long)(mc % 100));
    } else
        sprintf(msg, "Generative Fill: %ld s", (long)((now_us() - j->t0) / 1000000));
    status_message(msg);
}

/* ---------- Start / Abbruch / Abfrage ---------- */

BOOL ai_busy(void)
{
    return job != NULL;
}

ULONG ai_sigmask(void)
{
    return ai_sigbit >= 0 ? 1UL << ai_sigbit : 0;
}

void ai_cancel(void)
{
    if (job)
        job->ctl.cancel = 1;
}

static void bar_inval(void);

/* prompt NULL/leer: Inhalt entfernen und passend zur Umgebung fuellen */
BOOL ai_start(const char *prompt)
{
    static const char intro[] =
        "Image editing task. The first image is the photo to edit. The second image is a black-and-white "
        "mask of the same size: white marks the region to change, black must stay unchanged. ";
    static const char outro[] =
        "Match perspective, lighting, colors, sharpness and grain of the photo. Keep everything outside the "
        "white region identical. Return the full edited photo with exactly the same framing and aspect "
        "ratio, without the mask.";
    AiJob *j;
    LONG bx, by, bw, bh, sw, sh, lng, x, y, ri, plen, l64a, l64b, jl;
    UBYTE *rgb = NULL, *msk = NULL, *png = NULL;
    MemBuf jpg = { NULL, 0, 0 };
    char *e64 = NULL, *m64 = NULL, *esc = NULL, *p;
    int pngl = 0;
    BOOL remove = !prompt || !prompt[0];
    char key[160];

    if (job) {
        status_message("An AI request is already running");
        return FALSE;
    }
    if (!doc->hassel) {
        status_message("Make a selection first");
        return FALSE;
    }
    if (!(j = AllocVec(sizeof(AiJob), MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    if (!read_key(key, sizeof(key))) {
        FreeVec(j);
        request("No OpenRouter API key found.\n\nPut your key into the file\n"
                "ENVARC:AmiPixelShop/openrouter.key", "OK");
        return FALSE;
    }
    if (ai_sigbit < 0 && (ai_sigbit = AllocSignal(-1)) < 0) {
        FreeVec(j);
        return FALSE;
    }

    doc_process_dirty();
    ri = crop_box(&bx, &by, &bw, &bh);
    lng = bw > bh ? bw : bh;
    x = lng < AI_MINSIDE ? AI_MINSIDE : lng > AI_MAXSIDE ? AI_MAXSIDE : lng;
    sw = (bw * x + lng / 2) / lng;
    sh = (bh * x + lng / 2) / lng;

    sprintf(j->hdr, "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
                    "X-Title: AmiPixelShop\r\n", key);
    j->doc = doc;
    j->x = bx;
    j->y = by;
    j->w = bw;
    j->h = bh;
    j->parent = FindTask(NULL);
    j->sig = ai_sigbit;
    j->ctl.timeout = 180;
    if (remove)
        strcpy(j->name, "Gen: Remove");
    else
        sprintf(j->name, "Gen: %.26s", prompt);

    /* Auswahl im Ausschnitt merken (darf sich waehrenddessen aendern) */
    if (!(j->sel = malloc(bw * bh)) || !(rgb = malloc(sw * sh * 3)) || !(msk = malloc(sw * sh)))
        goto fail;
    for (y = 0; y < bh; y++)
        memcpy(j->sel + y * bw, doc->sel + (by + y) * doc->w + bx, bw);
    sample_rgb(rgb, sw, sh, bx, by, bw, bh);
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
            msk[y * sw + x] = j->sel[(y * bh / sh) * bw + x * bw / sw] ? 255 : 0;

    if (!stbi_write_jpg_to_func(mem_write, &jpg, sw, sh, 3, rgb, 92) || !jpg.p ||
        !(png = stbi_write_png_to_mem(msk, sw, sw, sh, 1, &pngl)))
        goto fail;
    free(rgb);
    free(msk);
    rgb = msk = NULL;
    if (!(e64 = b64_encode(jpg.p, jpg.n, &l64a)) || !(m64 = b64_encode(png, pngl, &l64b)))
        goto fail;
    free(jpg.p);
    free(png);
    jpg.p = png = NULL;

    plen = remove ? 0 : strlen(prompt);
    if (!(esc = malloc(plen * 2 + 8)))
        goto fail;
    json_esc(remove ? "" : prompt, esc, plen * 2 + 8);

    jl = l64a + l64b + strlen(esc) + sizeof(intro) + sizeof(outro) + 1024;
    if (!(j->body = net_alloc(jl)))
        goto fail;
    p = j->body;
    sprintf(p, "{\"model\":\"%s\",\"modalities\":[\"image\",\"text\"],"
               "\"image_config\":{\"aspect_ratio\":\"%ld:%ld\"},\"usage\":{\"include\":true},"
               "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"%s",
            model_name(), (long)ratios[ri][0], (long)ratios[ri][1], intro);
    p += strlen(p);
    if (remove)
        strcpy(p, "Remove whatever is in the white region and fill it so it blends seamlessly "
                  "with the surroundings, as if it had never been there. ");
    else
        sprintf(p, "Fill the white region with: %s. ", esc);
    p += strlen(p);
    strcpy(p, outro);
    strcat(p, "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,");
    p += strlen(p);
    memcpy(p, e64, l64a);
    p += l64a;
    strcpy(p, "\"}},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,");
    p += strlen(p);
    memcpy(p, m64, l64b);
    p += l64b;
    strcpy(p, "\"}}]}]}");
    p += strlen(p);
    j->blen = p - j->body;
    net_freemem(e64);
    net_freemem(m64);
    free(esc);

    j->t0 = now_us();
    job = j;
    if (!CreateNewProcTags(NP_Entry, (ULONG)ai_worker, NP_Name, (ULONG)"AmiPixelShop AI",
                           NP_StackSize, 65536, NP_Priority, -1, NP_Output, 0, NP_Input, 0,
                           NP_CloseOutput, FALSE, NP_CloseInput, FALSE, TAG_DONE)) {
        job = NULL;
        job_free(j);
        status_message("Cannot start the AI process");
        return FALSE;
    }
    bar_inval();
    return TRUE;

fail:
    free(rgb);
    free(msk);
    free(jpg.p);
    free(png);
    net_freemem(e64);
    net_freemem(m64);
    free(esc);
    job_free(j);
    status_message("Not enough memory");
    return FALSE;
}

/* nach einem Signal oder zyklisch: fertige Anfrage auswerten */
void ai_poll(void)
{
    AiJob *j = job;
    if (!j || !j->done)
        return;
    if (j->ok && !j->ctl.cancel) {
        /* nicht mitten in eine andere Aktion hinein */
        if (transform_active() || text_active() || liquify_active() || crop_active() || ui_dragging())
            return;
        if (doc_alive(j->doc)) {
            if (j->doc != doc)
                main_activate_doc(j->doc);
            place_result(j);
        }
    } else if (!j->ctl.cancel) {
        char msg[200];
        sprintf(msg, "Generative Fill failed:\n\n%s", j->err);
        job = NULL;
        bar_inval();
        job_free(j);
        request(msg, "OK");
        return;
    } else {
        status_message("Generative Fill cancelled");
    }
    job = NULL;
    bar_inval();
    job_free(j);
}

/* beim Beenden: laufende Anfrage abbrechen und abwarten */
void ai_shutdown(void)
{
    if (job) {
        job->ctl.cancel = 1;
        while (!job->done)
            Wait(1UL << ai_sigbit);
        job_free(job);
        job = NULL;
    }
    if (ai_sigbit >= 0) {
        FreeSignal(ai_sigbit);
        ai_sigbit = -1;
    }
}

/* ---------- schwebende Taskleiste unter der Auswahl ---------- */

enum { BAR_IDLE, BAR_PROMPT };
static LONG bar_mode = BAR_IDLE;
static char bar_prompt[200];
static LONG bar_x, bar_y, bar_w, bar_h;         /* zuletzt gezeichnet, w = 0: keine */
static LONG bar_secs = -1;
static LONG bar_sel[4];                         /* Auswahl, fuer die die Leiste offen ist */
#define BAR_H 34

static BOOL bar_visible(void)
{
    if (job)
        return TRUE;
    return doc && doc->hassel && !transform_active() && !text_active() && !liquify_active() &&
           !crop_active() && !ui_menu_open();
}

static void bar_rect(LONG *x, LONG *y, LONG *w, LONG *h)
{
    static LONG lx, ly;
    LONG ww = job ? 250 : bar_mode == BAR_PROMPT ? 480 : 252;
    *w = ww;
    *h = BAR_H;
    if (doc && doc->hassel) {
        LONG sx0 = doc2scrx(doc->selx0), sx1 = doc2scrx(doc->selx1);
        LONG sy0 = doc2scry(doc->sely0), sy1 = doc2scry(doc->sely1);
        lx = (sx0 + sx1) / 2 - ww / 2;
        ly = sy1 + 14;
        if (ly + BAR_H > cvy + cvh - 8)
            ly = sy0 - 14 - BAR_H;
        if (ly < cvy + 8)
            ly = cvy + cvh - BAR_H - 8;
    }
    if (lx + ww > cvx + cvw - 8)
        lx = cvx + cvw - ww - 8;
    if (lx < cvx + 8)
        lx = cvx + 8;
    *x = lx;
    *y = ly;
}

static void bar_button(LONG x, LONG y, LONG w, const char *s, BOOL accent)
{
    fill(x, y, w, 24, accent ? COL_ACCENT : COL_LIGHT);
    frame(x, y, w, 24, COL_LINE);
    text(x + (w - text_width(s)) / 2, y + 3, s, accent ? 0xFFFFFFFF : COL_TEXT);
}

/* Felder: liefert die x-Positionen der Teile */
#define FIELD_X(bx)   ((bx) + 8)
#define FIELD_W(bw)   ((bw) - 8 - 96 - 8 - 28 - 8)

void ai_overlay(void)
{
    LONG x, y, w, h;
    if (!bar_visible())
        return;
    bar_rect(&x, &y, &w, &h);
    fill(x + 3, y + 3, w, h, 0xFF151515);    /* Schatten */
    fill(x, y, w, h, COL_PANEL);
    frame(x, y, w, h, COL_LINE);
    if (job) {
        char s[48];
        LONG secs = (now_us() - job->t0) / 1000000;
        if (job->ctl.recvd > 0)
            sprintf(s, "Generating... %ld s  %ld KB", (long)secs, (long)(job->ctl.recvd >> 10));
        else
            sprintf(s, "Generating... %ld s", (long)secs);
        text(x + 10, y + 8, s, COL_TEXT);
        bar_button(x + w - 78, y + 5, 70, "Cancel", FALSE);
    } else if (bar_mode == BAR_PROMPT) {
        LONG fx = FIELD_X(x), fw = FIELD_W(w);
        fill(fx, y + 5, fw, 24, COL_FIELD);
        frame(fx, y + 5, fw, 24, COL_LINE);
        gfx_clip(fx + 2, y + 6, fx + fw - 2, y + 28);
        if (bar_prompt[0])
            text(fx + 5, y + 8, bar_prompt, COL_TEXT);
        else
            text(fx + 5, y + 8, "Describe what to generate (optional)", COL_DIM);
        gfx_noclip();
        bar_button(fx + fw + 8, y + 5, 96, "Generate", TRUE);
        bar_button(x + w - 36, y + 5, 28, "x", FALSE);
    } else {
        bar_button(x + 8, y + 5, 140, "Generative Fill", TRUE);
        bar_button(x + 156, y + 5, 88, "Remove", FALSE);
    }
}

static void bar_inval(void)
{
    LONG x, y, w, h;
    if (bar_w)
        view_inval(bar_x, bar_y, bar_x + bar_w + 3, bar_y + bar_h + 3);
    if (bar_visible()) {
        bar_rect(&x, &y, &w, &h);
        view_inval(x, y, x + w + 3, y + h + 3);
        bar_x = x;
        bar_y = y;
        bar_w = w;
        bar_h = h;
    } else {
        bar_w = 0;
    }
}

/* zyklisch (INTUITICKS): Lage/Zeit nachfuehren, Ergebnis abholen */
void ai_tick(void)
{
    LONG x = 0, y = 0, w = 0, h = 0;
    BOOL vis = bar_visible();
    ai_poll();
    if (vis)
        bar_rect(&x, &y, &w, &h);
    if (!vis && !doc->hassel)
        bar_mode = BAR_IDLE;
    /* neue Auswahl: wieder mit Generative Fill / Remove beginnen */
    if (doc->hassel && !job && (doc->selx0 != bar_sel[0] || doc->sely0 != bar_sel[1] ||
                                doc->selx1 != bar_sel[2] || doc->sely1 != bar_sel[3])) {
        bar_sel[0] = doc->selx0;
        bar_sel[1] = doc->sely0;
        bar_sel[2] = doc->selx1;
        bar_sel[3] = doc->sely1;
        if (bar_mode != BAR_IDLE) {
            bar_mode = BAR_IDLE;
            bar_inval();
        }
    }
    if ((vis ? w : 0) != bar_w || (vis && (x != bar_x || y != bar_y)))
        bar_inval();
    else if (job && (now_us() - job->t0) / 500000 != bar_secs) {
        bar_secs = (now_us() - job->t0) / 500000;
        bar_inval();
    }
}

static void bar_edit(void)
{
    LONG x, y, w, h;
    extern BOOL ui_edit_enter;
    bar_rect(&x, &y, &w, &h);
    if (ui_edit_text(FIELD_X(x), y + 5, FIELD_W(w), bar_prompt, sizeof(bar_prompt)) && ui_edit_enter)
        ai_start(bar_prompt);
    bar_inval();
    view_flush();
}

/* Edit > Generative Fill...: Leiste mit Eingabefeld oeffnen */
void ai_open_prompt(void)
{
    if (job || !doc->hassel) {
        status_message(job ? "An AI request is already running" : "Make a selection first");
        return;
    }
    bar_sel[0] = doc->selx0;
    bar_sel[1] = doc->sely0;
    bar_sel[2] = doc->selx1;
    bar_sel[3] = doc->sely1;
    bar_mode = BAR_PROMPT;
    bar_inval();
    view_flush();
    bar_edit();
}

/* Mausklick: TRUE, wenn er die Leiste getroffen hat */
BOOL ai_bar_click(LONG mx, LONG my)
{
    LONG x, y, w, h;
    if (!bar_visible())
        return FALSE;
    bar_rect(&x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y || my >= y + h)
        return FALSE;
    if (job) {
        if (mx >= x + w - 78)
            ai_cancel();
    } else if (bar_mode == BAR_PROMPT) {
        LONG fx = FIELD_X(x), fw = FIELD_W(w);
        if (mx >= fx && mx < fx + fw)
            bar_edit();
        else if (mx >= fx + fw + 8 && mx < fx + fw + 104)
            ai_start(bar_prompt);
        else if (mx >= x + w - 36) {
            bar_mode = BAR_IDLE;
            bar_inval();
        }
    } else if (mx >= x + 8 && mx < x + 148) {
        ai_open_prompt();
    } else if (mx >= x + 156 && mx < x + 244) {
        ai_start(NULL);
    }
    return TRUE;
}
