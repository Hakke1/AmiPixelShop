/*
 * AmiPixelShop - Prototyp 0
 * Eigener P96-Screen, Backbuffer im Fast-RAM, Dirty-Rect-Blit,
 * Benchmark und weicher Pinsel mit Alpha (RMB = Radierer).
 *
 * Aufruf: proto0         interaktiv (ESC beendet, [ ] Pinselgroesse,
 *                         1-5 Farbe, H Haerte umschalten)
 *         proto0 BENCH   nur Benchmark, Ausgabe in die Shell
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/modeid.h>
#include <devices/timer.h>
#include <libraries/Picasso96.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/Picasso96.h>
#include <string.h>
#include <math.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *P96Base;
struct Device *TimerBase;

const char ver[] = "$VER: proto0 0.1 (27.10.2026)";

/* ---- Farben: Photoshop CS6 dunkel ---- */
#define COL_APP      0xFF282828
#define COL_PANEL    0xFF535353
#define COL_PANELDK  0xFF424242
#define COL_BAR      0xFF3C3C3C
#define COL_LINE     0xFF1E1E1E
#define COL_TOOL     0xFF4A4A4A
#define COL_TOOLSEL  0xFF2D2D2D
#define COL_CHK1     0xFFFFFFFF
#define COL_CHK2     0xFFCCCCCC

#define TOOLBAR_W 40
#define OPTBAR_H  32
#define PANEL_W   260

static struct Screen *scr;
static struct Window *win;
static struct MsgPort *tport;
static struct timerequest *treq;
static BOOL timer_open;

static LONG SW, SH;
static ULONG *bb;               /* Backbuffer, ARGB, SW*SH */
static RGBFTYPE scrfmt;

/* Canvas-Viewport (Bildschirmkoordinaten) */
static LONG cvx, cvy, cvw, cvh;

/* Dokument: ULONG ARGB, straight alpha */
static LONG docw = 1024, doch = 768;
static ULONG *doc;
static ULONG inv_tab[256];      /* 0x1000000 / n */
static LONG docx, docy;         /* Lage des Dokuments auf dem Schirm */

/* Pinsel */
static LONG bsize = 40, bhard = 50;
static UBYTE *bmask;
static ULONG bcolor = 0x000000;
static UBYTE bopacity = 255;

static const ULONG palette[5] = { 0x000000, 0xD02020, 0x20A040, 0x2050D0, 0xF0C020 };

static ULONG now_us(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000000UL + tv.tv_micro;
}

/* ---- Backbuffer-Zeichenhilfen ---- */
static void fill(LONG x, LONG y, LONG w, LONG h, ULONG c)
{
    LONG i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    for (j = 0; j < h; j++) {
        ULONG *p = bb + (y + j) * SW + x;
        for (i = 0; i < w; i++)
            p[i] = c;
    }
}

/* ---- Blit Backbuffer -> VRAM ---- */
static void blit(LONG x, LONG y, LONG w, LONG h)
{
    struct BitMap *bm = scr->RastPort.BitMap;
    struct RenderInfo ri;
    LONG lock, i, j;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0)
        return;

    lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
    if (lock) {
        BOOL done = TRUE;
        UBYTE *dst = (UBYTE *)ri.Memory;
        LONG bpr = ri.BytesPerRow;
        for (j = 0; j < h && done; j++) {
            ULONG *s = bb + (y + j) * SW + x;
            UBYTE *d = dst + (y + j) * bpr;
            switch (ri.RGBFormat) {
            case RGBFB_A8R8G8B8:
                memcpy(d + x * 4, s, w * 4);
                break;
            case RGBFB_B8G8R8A8: {
                ULONG *dl = (ULONG *)(d + x * 4);
                for (i = 0; i < w; i++) {
                    ULONG v = s[i];
                    dl[i] = (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
                }
                break; }
            case RGBFB_R8G8B8A8: {
                ULONG *dl = (ULONG *)(d + x * 4);
                for (i = 0; i < w; i++)
                    dl[i] = (s[i] << 8) | 0xFF;
                break; }
            case RGBFB_R8G8B8: {
                UBYTE *db = d + x * 3;
                for (i = 0; i < w; i++) {
                    ULONG v = s[i];
                    db[0] = v >> 16; db[1] = v >> 8; db[2] = v;
                    db += 3;
                }
                break; }
            case RGBFB_B8G8R8: {
                UBYTE *db = d + x * 3;
                for (i = 0; i < w; i++) {
                    ULONG v = s[i];
                    db[0] = v; db[1] = v >> 8; db[2] = v >> 16;
                    db += 3;
                }
                break; }
            case RGBFB_R5G6B5: {
                UWORD *dw = (UWORD *)(d + x * 2);
                for (i = 0; i < w; i++) {
                    ULONG v = s[i];
                    dw[i] = ((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F);
                }
                break; }
            case RGBFB_R5G6B5PC: {
                UWORD *dw = (UWORD *)(d + x * 2);
                for (i = 0; i < w; i++) {
                    ULONG v = s[i];
                    UWORD p = ((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F);
                    dw[i] = (UWORD)((p << 8) | (p >> 8));
                }
                break; }
            default:
                done = FALSE;
                break;
            }
        }
        p96UnlockBitMap(bm, lock);
        if (done)
            return;
    }

    /* Fallback: P96 konvertiert selbst */
    ri.Memory = bb;
    ri.BytesPerRow = SW * 4;
    ri.pad = 0;
    ri.RGBFormat = RGBFB_A8R8G8B8;
    p96WritePixelArray(&ri, x, y, win->RPort, x, y, w, h);
}

/* Variante nur ueber p96WritePixelArray (fuer den Benchmark) */
static void blit_wpa(LONG x, LONG y, LONG w, LONG h)
{
    struct RenderInfo ri;
    ri.Memory = bb;
    ri.BytesPerRow = SW * 4;
    ri.pad = 0;
    ri.RGBFormat = RGBFB_A8R8G8B8;
    p96WritePixelArray(&ri, x, y, win->RPort, x, y, w, h);
}

/* ---- UI-Geruest ---- */
static void draw_ui(void)
{
    LONG i;
    fill(0, 0, SW, SH, COL_APP);
    /* Optionsleiste oben */
    fill(0, 0, SW, OPTBAR_H, COL_BAR);
    fill(0, OPTBAR_H - 1, SW, 1, COL_LINE);
    /* Werkzeugleiste links */
    fill(0, OPTBAR_H, TOOLBAR_W, SH - OPTBAR_H, COL_BAR);
    fill(TOOLBAR_W - 1, OPTBAR_H, 1, SH - OPTBAR_H, COL_LINE);
    for (i = 0; i < 16; i++) {
        LONG y = OPTBAR_H + 8 + i * 30;
        fill(8, y, 24, 24, i == 4 ? COL_TOOLSEL : COL_TOOL);
    }
    /* Farbfelder unten in der Werkzeugleiste */
    fill(8, OPTBAR_H + 8 + 16 * 30 + 10, 18, 18, 0xFF000000 | bcolor);
    fill(16, OPTBAR_H + 8 + 16 * 30 + 18, 18, 18, 0xFFFFFFFF);
    fill(8, OPTBAR_H + 8 + 16 * 30 + 10, 18, 18, 0xFF000000 | bcolor);
    /* Panels rechts */
    fill(SW - PANEL_W, OPTBAR_H, PANEL_W, SH - OPTBAR_H, COL_PANELDK);
    fill(SW - PANEL_W, OPTBAR_H, 1, SH - OPTBAR_H, COL_LINE);
    fill(SW - PANEL_W + 4, OPTBAR_H + 4, PANEL_W - 8, 200, COL_PANEL);
    fill(SW - PANEL_W + 4, OPTBAR_H + 4, PANEL_W - 8, 22, COL_BAR);
    fill(SW - PANEL_W + 4, OPTBAR_H + 210, PANEL_W - 8, SH - OPTBAR_H - 214, COL_PANEL);
    fill(SW - PANEL_W + 4, OPTBAR_H + 210, PANEL_W - 8, 22, COL_BAR);
    for (i = 0; i < 4; i++)
        fill(SW - PANEL_W + 8, OPTBAR_H + 240 + i * 42, PANEL_W - 16, 40, i == 0 ? 0xFF6A6A6A : 0xFF4E4E4E);
}

/* ---- Canvas-Komposition: Schachbrett + Ebene ---- */
/* d = chk + (c - chk) * a, R und B parallel, ohne Verzweigung */
static inline ULONG blend_over(ULONG c, ULONG chk)
{
    ULONG a = c >> 24;
    ULONG rb, g;
    a += a >> 7;                                    /* 0..256 */
    rb = ((c & 0xFF00FF) * a + (chk & 0xFF00FF) * (256 - a)) >> 8;
    g  = ((c & 0x00FF00) * a + (chk & 0x00FF00) * (256 - a)) >> 8;
    return 0xFF000000 | (rb & 0xFF00FF) | (g & 0x00FF00);
}

static void composite(LONG x0, LONG y0, LONG x1, LONG y1)
{
    LONG x, y, dxs, dxe;
    if (x0 < cvx) x0 = cvx;
    if (y0 < cvy) y0 = cvy;
    if (x1 > cvx + cvw) x1 = cvx + cvw;
    if (y1 > cvy + cvh) y1 = cvy + cvh;
    if (x0 >= x1)
        return;
    /* Spaltenbereich, der das Dokument trifft */
    dxs = x0 < docx ? docx : x0;
    dxe = x1 > docx + docw ? docx + docw : x1;
    for (y = y0; y < y1; y++) {
        ULONG *d = bb + y * SW;
        LONG dy = y - docy;
        if (dy < 0 || dy >= doch || dxs >= dxe) {
            for (x = x0; x < x1; x++)
                d[x] = COL_APP;
            continue;
        }
        for (x = x0; x < dxs; x++)
            d[x] = COL_APP;
        {
            const ULONG *s = doc + dy * docw - docx;
            ULONG chkrow = ((dy >> 3) & 1) ? 8 : 0;
            for (x = dxs; x < dxe; x++) {
                /* Schachbrett 8x8: 0xFFFFFF / 0xCCCCCC */
                ULONG chk = (((ULONG)(x - docx) ^ chkrow) & 8) ? 0xCCCCCC : 0xFFFFFF;
                d[x] = blend_over(s[x], chk);
            }
        }
        for (x = dxe; x < x1; x++)
            d[x] = COL_APP;
    }
}

/* ---- Pinsel ---- */
static void make_brush(void)
{
    LONG x, y;
    float r = bsize * 0.5f, h = bhard / 100.0f;
    if (bmask)
        FreeVec(bmask);
    bmask = AllocVec(bsize * bsize, MEMF_ANY);
    if (!bmask)
        return;
    for (y = 0; y < bsize; y++)
        for (x = 0; x < bsize; x++) {
            float fx = x + 0.5f - r, fy = y + 0.5f - r;
            float d = sqrtf(fx * fx + fy * fy) / r, v;
            if (d >= 1.0f)
                v = 0.0f;
            else if (d <= h)
                v = 1.0f;
            else {
                float t = (d - h) / (1.0f - h);
                v = 1.0f - t * t * (3.0f - 2.0f * t);
            }
            bmask[y * bsize + x] = (UBYTE)(v * 255.0f + 0.5f);
        }
}

/* Dirty-Rect in Dokumentkoordinaten */
static LONG dx0, dy0, dx1, dy1;

static void dirty_reset(void) { dx0 = dy0 = 0x7FFFFFFF; dx1 = dy1 = -0x7FFFFFFF; }

static void dab(float cx, float cy, BOOL erase)
{
    LONG ox = (LONG)(cx - bsize * 0.5f + 0.5f);
    LONG oy = (LONG)(cy - bsize * 0.5f + 0.5f);
    LONG x, y, xs = 0, ys = 0, xe = bsize, ye = bsize;
    ULONG srb = bcolor & 0xFF00FF, sg = bcolor & 0x00FF00;

    if (!bmask)
        return;
    if (ox < 0) xs = -ox;
    if (oy < 0) ys = -oy;
    if (ox + xe > docw) xe = docw - ox;
    if (oy + ye > doch) ye = doch - oy;
    if (xs >= xe || ys >= ye)
        return;

    for (y = ys; y < ye; y++) {
        const UBYTE *m = bmask + y * bsize;
        ULONG *p = doc + (oy + y) * docw + ox;
        for (x = xs; x < xe; x++) {
            ULONG sa = (m[x] * bopacity * 257 + 0x8080) >> 16;
            ULONG q, da, oa, inv, rb, g;
            if (!sa)
                continue;
            q = p[x];
            da = q >> 24;
            if (erase) {
                da = (da * (255 - sa) * 257 + 0x8080) >> 16;
                p[x] = (da << 24) | (q & 0xFFFFFF);
                continue;
            }
            /* src-over, straight alpha: c = (s*sa + d*da') / oa */
            da = (da * (255 - sa) * 257 + 0x8080) >> 16;
            oa = sa + da;
            inv = inv_tab[oa];                       /* 2^16 / oa */
            /* Summe <= 65025, * inv <= 2^32: passt in ULONG */
            rb = (((srb >> 16) * sa + ((q >> 16) & 0xFF) * da) * inv >> 16) << 16;
            rb |= ((srb & 0xFF) * sa + (q & 0xFF) * da) * inv >> 16;
            g  = (((sg >> 8) * sa + ((q >> 8) & 0xFF) * da) * inv >> 16) << 8;
            p[x] = (oa << 24) | rb | g;
        }
    }
    if (ox + xs < dx0) dx0 = ox + xs;
    if (oy + ys < dy0) dy0 = oy + ys;
    if (ox + xe > dx1) dx1 = ox + xe;
    if (oy + ye > dy1) dy1 = oy + ye;
}

static float lastx, lasty, carry;

static void stroke_to(float x, float y, BOOL erase)
{
    float ddx = x - lastx, ddy = y - lasty;
    float dist = sqrtf(ddx * ddx + ddy * ddy);
    float spacing = bsize * 0.15f, t;
    if (spacing < 1.0f)
        spacing = 1.0f;
    t = spacing - carry;
    while (t <= dist) {
        float f = t / dist;
        dab(lastx + ddx * f, lasty + ddy * f, erase);
        t += spacing;
    }
    carry = dist - (t - spacing);
    lastx = x;
    lasty = y;
}

static void flush_dirty(void)
{
    LONG x0, y0, x1, y1;
    if (dx1 <= dx0)
        return;
    x0 = dx0 + docx; y0 = dy0 + docy;
    x1 = dx1 + docx; y1 = dy1 + docy;
    composite(x0, y0, x1, y1);
    if (x0 < cvx) x0 = cvx;
    if (y0 < cvy) y0 = cvy;
    if (x1 > cvx + cvw) x1 = cvx + cvw;
    if (y1 > cvy + cvh) y1 = cvy + cvh;
    blit(x0, y0, x1 - x0, y1 - y0);
    dirty_reset();
}

/* ---- Benchmark ---- */
static LONG rate(ULONG units, ULONG us) { return us ? (LONG)((unsigned long long)units * 1000000ULL / us) : 0; }

static void benchmark(void)
{
    ULONG t, i, n;
    LONG x, y;
    ULONG pix = SW * SH;

    Printf("Screen %ldx%ld, RGBFormat %ld\n", SW, SH, (LONG)scrfmt);

    n = 50;
    t = now_us();
    for (i = 0; i < n; i++)
        fill(0, 0, SW, SH, 0xFF000000 | i);
    t = now_us() - t;
    Printf("Fill Backbuffer:        %ld MPix/s\n", rate(n * pix, t) / 1000000);

    {
        static const char *names[3] = { "Alpha 0  ", "Alpha 255", "gemischt " };
        LONG k;
        for (k = 0; k < 3; k++) {
            for (y = 0; y < doch; y++)
                for (x = 0; x < docw; x++) {
                    ULONG a = k == 0 ? 0 : k == 1 ? 255 : ((x + y) & 0xFF);
                    doc[y * docw + x] = (a << 24) | ((x & 0xFF) << 16) | ((y & 0xFF) << 8) | ((x ^ y) & 0xFF);
                }
            n = 30;
            t = now_us();
            for (i = 0; i < n; i++)
                composite(cvx, cvy, cvx + cvw, cvy + cvh);
            t = now_us() - t;
            Printf("Composite %s:    %ld MPix/s  (%ld fps Canvas)\n", (ULONG)names[k],
                   rate(n * cvw * cvh, t) / 1000000, rate(n, t));
        }
    }

    draw_ui();
    composite(cvx, cvy, cvx + cvw, cvy + cvh);

    n = 30;
    t = now_us();
    for (i = 0; i < n; i++)
        blit(0, 0, SW, SH);
    t = now_us() - t;
    Printf("Blit direkt (Lock):     %ld fps  %ld MB/s\n", rate(n, t), rate(n * pix * 4, t) / 1000000);

    t = now_us();
    for (i = 0; i < n; i++)
        blit_wpa(0, 0, SW, SH);
    t = now_us() - t;
    Printf("Blit p96WritePixelArray:%ld fps  %ld MB/s\n", rate(n, t), rate(n * pix * 4, t) / 1000000);

    memset(doc, 0, docw * doch * 4);
    bcolor = 0x2050D0;
    bsize = 64; bhard = 50;
    make_brush();
    n = 5000;
    t = now_us();
    for (i = 0; i < n; i++)
        dab((float)(i * 7 % docw), (float)(i * 13 % doch), FALSE);
    t = now_us() - t;
    Printf("Pinsel-Dabs 64px:       %ld Dabs/s\n", rate(n, t));

    memset(doc, 0, docw * doch * 4);
    bcolor = 0;
    bsize = 40;
    make_brush();
}

/* ---- Main ---- */
static void cleanup(void)
{
    if (win) CloseWindow(win);
    if (scr) CloseScreen(scr);
    if (bb) FreeVec(bb);
    if (doc) FreeVec(doc);
    if (bmask) FreeVec(bmask);
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (P96Base) CloseLibrary(P96Base);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
}

int main(int argc, char **argv)
{
    struct Screen *wb;
    struct TagItem bidtags[5];
    ULONG modeid;
    BOOL bench_only = (argc > 1 && (argv[1][0] == 'B' || argv[1][0] == 'b'));
    BOOL done = FALSE, painting = FALSE, erasing = FALSE;
    LONG x;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    P96Base = OpenLibrary("Picasso96API.library", 2);
    if (!IntuitionBase || !GfxBase || !P96Base) {
        PutStr("Libraries fehlen (Picasso96API.library?)\n");
        cleanup();
        return RETURN_FAIL;
    }
    if (!(tport = CreateMsgPort()) ||
        !(treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest))) ||
        OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)treq, 0) != 0) {
        cleanup();
        return RETURN_FAIL;
    }
    timer_open = TRUE;
    TimerBase = treq->tr_node.io_Device;

    /* Aufloesung wie die Workbench, 32 Bit bevorzugt */
    wb = LockPubScreen(NULL);
    if (!wb) {
        cleanup();
        return RETURN_FAIL;
    }
    SW = wb->Width;
    SH = wb->Height;
    bidtags[0].ti_Tag = P96BIDTAG_NominalWidth;  bidtags[0].ti_Data = SW;
    bidtags[1].ti_Tag = P96BIDTAG_NominalHeight; bidtags[1].ti_Data = SH;
    bidtags[2].ti_Tag = P96BIDTAG_Depth;         bidtags[2].ti_Data = 32;
    bidtags[3].ti_Tag = P96BIDTAG_FormatsAllowed;
    bidtags[3].ti_Data = RGBFF_A8R8G8B8 | RGBFF_B8G8R8A8 | RGBFF_R8G8B8A8;
    bidtags[4].ti_Tag = TAG_DONE;
    modeid = p96BestModeIDTagList(bidtags);
    if (modeid == (ULONG)INVALID_ID)
        modeid = GetVPModeID(&wb->ViewPort);
    UnlockPubScreen(NULL, wb);

    scr = OpenScreenTags(NULL,
        SA_DisplayID, modeid,
        SA_Width, SW, SA_Height, SH,
        SA_Depth, 24,
        SA_Quiet, TRUE,
        SA_ShowTitle, FALSE,
        SA_Type, CUSTOMSCREEN,
        SA_Title, (ULONG)"AmiPixelShop",
        TAG_DONE);
    if (!scr) {
        PutStr("Screen konnte nicht geoeffnet werden\n");
        cleanup();
        return RETURN_FAIL;
    }
    SW = scr->Width;
    SH = scr->Height;
    scrfmt = (RGBFTYPE)p96GetBitMapAttr(scr->RastPort.BitMap, P96BMA_RGBFORMAT);

    win = OpenWindowTags(NULL,
        WA_CustomScreen, (ULONG)scr,
        WA_Left, 0, WA_Top, 0, WA_Width, SW, WA_Height, SH,
        WA_Backdrop, TRUE, WA_Borderless, TRUE,
        WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
        WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY,
        TAG_DONE);
    bb = AllocVec(SW * SH * 4, MEMF_ANY);
    doc = AllocVec(docw * doch * 4, MEMF_ANY | MEMF_CLEAR);
    if (!win || !bb || !doc) {
        PutStr("Kein Speicher / Fenster\n");
        cleanup();
        return RETURN_FAIL;
    }

    cvx = TOOLBAR_W;
    cvy = OPTBAR_H;
    cvw = SW - TOOLBAR_W - PANEL_W;
    cvh = SH - OPTBAR_H;
    docx = cvx + (cvw - docw) / 2;
    docy = cvy + (cvh - doch) / 2;

    for (x = 1; x < 256; x++)
        inv_tab[x] = (65536 + x / 2) / x;
    inv_tab[0] = 0;

    benchmark();
    if (bench_only) {
        cleanup();
        return RETURN_OK;
    }

    draw_ui();
    composite(cvx, cvy, cvx + cvw, cvy + cvh);
    blit(0, 0, SW, SH);
    dirty_reset();

    while (!done) {
        ULONG winsig = 1UL << win->UserPort->mp_SigBit;
        ULONG sigs = Wait(winsig | SIGBREAKF_CTRL_C);
        struct IntuiMessage *im;
        BOOL redraw_ui = FALSE;

        if (sigs & SIGBREAKF_CTRL_C)
            done = TRUE;
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            float mx = (float)(im->MouseX - docx), my = (float)(im->MouseY - docy);
            ReplyMsg((struct Message *)im);

            switch (class) {
            case IDCMP_MOUSEBUTTONS:
                if (code == SELECTDOWN || code == MENUDOWN) {
                    painting = TRUE;
                    erasing = (code == MENUDOWN);
                    lastx = mx; lasty = my; carry = 0.0f;
                    dab(mx, my, erasing);
                } else if (code == SELECTUP || code == MENUUP) {
                    painting = FALSE;
                }
                break;
            case IDCMP_MOUSEMOVE:
                if (painting)
                    stroke_to(mx, my, erasing);
                break;
            case IDCMP_RAWKEY:
                if (code == 0x45)
                    done = TRUE;
                else if (code == 0x1A || code == 0x1B) {
                    bsize += (code == 0x1B) ? (bsize >= 20 ? bsize / 5 : 2) : -(bsize >= 20 ? bsize / 5 : 2);
                    if (bsize < 2) bsize = 2;
                    if (bsize > 500) bsize = 500;
                    make_brush();
                } else if (code >= 0x01 && code <= 0x05) {
                    bcolor = palette[code - 1];
                    redraw_ui = TRUE;
                } else if (code == 0x25) {          /* H */
                    bhard = (bhard == 50) ? 100 : (bhard == 100 ? 0 : 50);
                    make_brush();
                }
                break;
            }
        }
        flush_dirty();
        if (redraw_ui) {
            fill(8, OPTBAR_H + 8 + 16 * 30 + 10, 18, 18, 0xFF000000 | bcolor);
            blit(0, OPTBAR_H + 8 + 16 * 30 + 10, TOOLBAR_W, 40);
        }
    }

    cleanup();
    return RETURN_OK;
}
