/*
 * bench.c - "AmiPixelShop BENCH": C-Referenz gegen Assembler
 *
 * Jeder Kern laeuft mit identischen Zufallsdaten in beiden Fassungen.
 * Ausgegeben werden MPix/s, der Faktor und ob die Ergebnisse bitgenau
 * uebereinstimmen.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include "apx.h"

#define BW 640
#define BH 480
#define BN (BW * BH)

static ULONG seed = 12345;

static ULONG rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* gueltiges premultiplied ARGB, Alpha zu je einem Drittel 0 / 255 / gemischt */
static ULONG rnd_premul(void)
{
    ULONG k = rnd() % 3, a = k == 0 ? 0 : k == 1 ? 255 : rnd() & 0xFF, r, g, b;
    if (!a)
        return 0;
    r = rnd() % (a + 1); g = rnd() % (a + 1); b = rnd() % (a + 1);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static ULONG rnd_straight(void)
{
    ULONG k = rnd() % 3, a = k == 0 ? 0 : k == 1 ? 255 : rnd() & 0xFF;
    return (a << 24) | (rnd() & 0xFFFFFF);
}

/* MPix/s * 10 */
static LONG rate10(ULONG pix, ULONG us)
{
    return us ? (LONG)((unsigned long long)pix * 10 / us) : 0;
}

static void report(const char *name, ULONG pix, ULONG tc, ULONG ta, BOOL same)
{
    LONG c = rate10(pix, tc), a = rate10(pix, ta);
    LONG f = tc && ta ? (LONG)((unsigned long long)tc * 10 / ta) : 0;
    Printf("%-26s C %4ld.%ld  ASM %4ld.%ld MPix/s  x%ld.%ld  %s\n", (ULONG)name,
           c / 10, c % 10, a / 10, a % 10, f / 10, f % 10, (ULONG)(same ? "ok" : "MISMATCH!"));
}

void bench_run(void)
{
    ULONG *src = AllocVec(BN * 4, MEMF_ANY), *dst = AllocVec(BN * 4, MEMF_ANY);
    ULONG *oc = AllocVec(BN * 4, MEMF_ANY), *oa = AllocVec(BN * 4, MEMF_ANY);
    ULONG *sums = AllocVec(4 * BW * 4, MEMF_ANY), *pad = AllocVec((BW + 2 * 128 + 2) * 4, MEMF_ANY);
    LONG *xmap = AllocVec(BW * sizeof(LONG), MEMF_ANY);
    LONG i, k, rep;
    ULONG t, tc, ta;
    static const LONG radii[2] = { 3, 30 };

    if (!src || !dst || !oc || !oa || !sums || !xmap || !pad) {
        PutStr("Out of memory\n");
        goto out;
    }
    Printf("Test image %ldx%ld (%ld pixels), each kernel repeated\n\n", (LONG)BW, (LONG)BH, (LONG)BN);

    /* Anzeige: premultiplied ueber Schachbrett */
    for (i = 0; i < BN; i++)
        src[i] = rnd_premul();
    rep = 10;
    t = now_us();
    for (k = 0; k < rep; k++)
        for (i = 0; i < BH; i++)
            span_show_mix_c(oc + i * BW, src + i * BW, BW, 3, i >> 3 & 1);
    tc = now_us() - t;
    t = now_us();
    for (k = 0; k < rep; k++)
        for (i = 0; i < BH; i++)
            span_show_mix_asm(oa + i * BW, src + i * BW, BW, 3, i >> 3 & 1);
    ta = now_us() - t;
    report("Display (mixed)", BN * rep, tc, ta, !memcmp(oc, oa, BN * 4));

    /* Anzeige mit Zoom 2:1 ueber Spaltentabelle */
    for (i = 0; i < BW; i++)
        xmap[i] = i / 2;
    t = now_us();
    for (k = 0; k < rep; k++)
        for (i = 0; i < BH; i++)
            span_show_mix_map_c(oc + i * BW, src + (i / 2) * BW, xmap, BW, 0, i >> 3 & 1);
    tc = now_us() - t;
    t = now_us();
    for (k = 0; k < rep; k++)
        for (i = 0; i < BH; i++)
            span_show_mix_map_asm(oa + i * BW, src + (i / 2) * BW, xmap, BW, 0, i >> 3 & 1);
    ta = now_us() - t;
    report("Display zoom (mixed)", BN * rep, tc, ta, !memcmp(oc, oa, BN * 4));

    /* Ebene ueber Komposit legen */
    for (i = 0; i < BN; i++) {
        src[i] = rnd_straight();
        dst[i] = rnd_premul();
    }
    for (k = 0; k < 2; k++) {
        ULONG op = k ? 160 : 255;
        tc = ta = 0;
        for (i = 0; i < rep; i++) {
            CopyMemQuick(dst, oc, BN * 4);
            t = now_us();
            span_layer_over_c(oc, src, BN, op);
            tc += now_us() - t;
            CopyMemQuick(dst, oa, BN * 4);
            t = now_us();
            span_layer_over_asm(oa, src, BN, op);
            ta += now_us() - t;
        }
        report(k ? "Layer over (62%)" : "Layer over (100%)", BN * rep, tc, ta,
               !memcmp(oc, oa, BN * 4));
    }

    /* Box-Unschaerfe */
    for (i = 0; i < BN; i++)
        src[i] = rnd_premul();
    rep = 3;
    for (k = 0; k < 2; k++) {
        char name[32];
        LONG r = radii[k];
        t = now_us();
        for (i = 0; i < rep; i++)
            blur_h_c(src, oc, BW, BH, r, pad);
        tc = now_us() - t;
        t = now_us();
        for (i = 0; i < rep; i++)
            blur_h_asm(src, oa, BW, BH, r, pad);
        ta = now_us() - t;
        sprintf(name, "Blur horizontal r=%ld", (long)r);
        report(name, BN * rep, tc, ta, !memcmp(oc, oa, BN * 4));

        t = now_us();
        for (i = 0; i < rep; i++)
            blur_v_c(src, oc, BW, BH, r, sums);
        tc = now_us() - t;
        t = now_us();
        for (i = 0; i < rep; i++)
            blur_v_asm(src, oa, BW, BH, r, sums);
        ta = now_us() - t;
        sprintf(name, "Blur vertical r=%ld", (long)r);
        report(name, BN * rep, tc, ta, !memcmp(oc, oa, BN * 4));
    }
    /* Farbton/Saettigung: exakte HSL-Rechnung gegen Gitter-Interpolation */
    {
        extern void bench_huesat(ULONG *, ULONG *, LONG, ULONG *, ULONG *, LONG *);
        LONG dev, c, a, f;
        for (i = 0; i < BN; i++)
            src[i] = 0xFF000000 | (rnd() & 0xFFFFFF);
        bench_huesat(src, oc, BN, &tc, &ta, &dev);
        c = rate10(BN, tc);
        a = rate10(BN, ta);
        f = ta ? (LONG)((unsigned long long)tc * 10 / ta) : 0;
        Printf("%-26s exact %ld.%ld  grid %ld.%ld MPix/s  x%ld.%ld  max. deviation %ld\n",
               (ULONG)"Hue/Saturation", c / 10, c % 10, a / 10, a % 10, f / 10, f % 10, dev);
    }
out:
    if (src) FreeVec(src);
    if (dst) FreeVec(dst);
    if (oc) FreeVec(oc);
    if (oa) FreeVec(oa);
    if (sums) FreeVec(sums);
    if (xmap) FreeVec(xmap);
    if (pad) FreeVec(pad);
}
