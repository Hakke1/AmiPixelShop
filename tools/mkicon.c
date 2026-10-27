/*
 * mkicon.c - schreibt AmiPixelShop.info (Farb-Icon 64x64) ueber die icon.library
 *
 * Eingabe icon.raw: 256*3 Bytes Palette (RGB), dann 64*64 Bytes Pixel
 * (Index 0 = transparent). Aufruf: mkicon icon.raw Work:AmiPixelShop/AmiPixelShop
 */
#include <exec/types.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>

struct Library *IconBase;
static UBYTE buf[768 + 64 * 64], pal2[768];

int main(int argc, char **argv)
{
    struct DiskObject *dobj;
    BPTR fh;
    LONG i, err = 0;
    if (argc != 3) {
        PutStr("Usage: mkicon icon.raw target\n");
        return 10;
    }
    if (!(fh = Open(argv[1], MODE_OLDFILE)))
        return 10;
    Read(fh, buf, sizeof(buf));
    Close(fh);
    if (!(IconBase = OpenLibrary("icon.library", 46))) {
        PutStr("icon.library 46 needed\n");
        return 10;
    }
    /* ausgewaehlt: etwas dunkler */
    for (i = 0; i < 768; i++)
        pal2[i] = buf[i] * 3 / 4;
    dobj = GetDefDiskObject(WBTOOL);       /* Standard-Werkzeug-Icon als Grundlage */
    if (!dobj)
        dobj = NewDiskObject(WBTOOL);
    if (dobj) {
        LONG cerr = 0;
        dobj->do_CurrentX = NO_ICON_POSITION;
        dobj->do_CurrentY = NO_ICON_POSITION;
        IconControl(dobj,
            ICONCTRLA_SetWidth, 64, ICONCTRLA_SetHeight, 64,
            ICONCTRLA_SetPaletteSize1, 256, ICONCTRLA_SetPalette1, (ULONG)buf,
            ICONCTRLA_SetImageData1, (ULONG)(buf + 768), ICONCTRLA_SetTransparentColor1, 0,
            ICONCTRLA_SetPaletteSize2, 256, ICONCTRLA_SetPalette2, (ULONG)pal2,
            ICONCTRLA_SetImageData2, (ULONG)(buf + 768), ICONCTRLA_SetTransparentColor2, 0,
            ICONA_ErrorCode, (ULONG)&cerr,
            TAG_DONE);
        if (cerr)
            Printf("IconControl error %ld\n", cerr);
        if (!PutIconTags(argv[2], dobj, ICONPUTA_NotifyWorkbench, TRUE, ICONA_ErrorCode, (ULONG)&err, TAG_DONE))
            Printf("PutIcon failed (%ld)\n", err);
        else
            PutStr("Icon written\n");
        FreeDiskObject(dobj);
    }
    CloseLibrary(IconBase);
    return 0;
}
