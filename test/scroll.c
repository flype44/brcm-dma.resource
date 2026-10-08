/*
    brcm-dma-scroll -- ScrollRaster() on a screen of a given size and depth, and the number of operations a second: a test of the scrolls to the side
    (the case that went through the CPU) and of the scrolls up and down, at 8, 16, 24 and 32 bits.

      brcm-dma-scroll W H DEPTH DX DY [RW RH] [SECS n]

    W, H     size of the screen (a 640x480 screen is what P96Speed uses)
    DEPTH    8, 16, 24 or 32
    DX, DY   the scroll of one operation: it goes one way, then the other (a negative number is a scroll the other way)
    RW, RH   the size of the scrolled area (default: the whole screen)
    SECS     duration (default 3)

    The screen is opened, scrolled and closed; the operations are counted with the clock of the system (20 ms). Read brcm-dma-info before and after
    to see what went through the DMA.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/rastport.h>
#include <libraries/Picasso96.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/Picasso96.h>

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
struct IntuitionBase * IntuitionBase;
struct GfxBase * GfxBase;
struct Library * P96Base;

static ULONG NowMs(void)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ds.ds_Minute * 60UL * 50UL + ds.ds_Tick) * 20UL;
}

int main(int argc, struct WBStartup *wbmsg)
{
    LONG args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    struct Screen *scr;
    struct Window *win;
    ULONG w, h, depth, rw, rh, secs = 3, id, formats, err = 0, ops = 0, t0, t1;
    LONG dx, dy;
    int rc = 10;
    struct TagItem bid[5];
    struct TagItem sat[8];
    struct TagItem wat[6];

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("W/N/A,H/N/A,DEPTH/N/A,DX/N/A,DY/N/A,RW/N,RH/N,SECS/K/N", args, NULL);
    if (rda == NULL)
    {
        Printf("usage: brcm-dma-scroll W H DEPTH DX DY [RW RH] [SECS n]\n");
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    w = *(ULONG *)args[0];
    h = *(ULONG *)args[1];
    depth = *(ULONG *)args[2];
    dx = *(LONG *)args[3];
    dy = *(LONG *)args[4];
    rw = args[5] ? *(ULONG *)args[5] : w;
    rh = args[6] ? *(ULONG *)args[6] : h;
    if (args[7])
        secs = *(ULONG *)args[7];
    FreeArgs(rda);

    formats = depth == 8 ? RGBFF_CLUT : depth == 16 ? RGBFF_R5G6B5PC : depth == 24 ? RGBFF_R8G8B8 : RGBFF_B8G8R8A8;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    P96Base = OpenLibrary("Picasso96API.library", 2);

    if (IntuitionBase == NULL || GfxBase == NULL || P96Base == NULL)
    {
        Printf("cannot open intuition / graphics / Picasso96API\n");
        goto done;
    }

    bid[0].ti_Tag = P96BIDTAG_NominalWidth;   bid[0].ti_Data = w;
    bid[1].ti_Tag = P96BIDTAG_NominalHeight;  bid[1].ti_Data = h;
    bid[2].ti_Tag = P96BIDTAG_Depth;          bid[2].ti_Data = depth;
    bid[3].ti_Tag = P96BIDTAG_FormatsAllowed; bid[3].ti_Data = formats;
    bid[4].ti_Tag = TAG_DONE;                 bid[4].ti_Data = 0;

    id = p96BestModeIDTagList(bid);
    if (id == INVALID_ID)
    {
        Printf("no %ldx%ldx%ld RTG mode\n", (LONG)w, (LONG)h, (LONG)depth);
        goto done;
    }

    sat[0].ti_Tag = SA_DisplayID;  sat[0].ti_Data = id;
    sat[1].ti_Tag = SA_Width;      sat[1].ti_Data = w;
    sat[2].ti_Tag = SA_Height;     sat[2].ti_Data = h;
    sat[3].ti_Tag = SA_Depth;      sat[3].ti_Data = depth;
    sat[4].ti_Tag = SA_Quiet;      sat[4].ti_Data = TRUE;
    sat[5].ti_Tag = SA_ShowTitle;  sat[5].ti_Data = FALSE;
    sat[6].ti_Tag = SA_ErrorCode;  sat[6].ti_Data = (ULONG)&err;
    sat[7].ti_Tag = TAG_DONE;      sat[7].ti_Data = 0;

    scr = OpenScreenTagList(NULL, sat);
    if (scr == NULL)
    {
        Printf("OpenScreen failed, error %ld\n", (LONG)err);
        goto done;
    }

    wat[0].ti_Tag = WA_CustomScreen; wat[0].ti_Data = (ULONG)scr;
    wat[1].ti_Tag = WA_Borderless;   wat[1].ti_Data = TRUE;
    wat[2].ti_Tag = WA_Left;         wat[2].ti_Data = 0;
    wat[3].ti_Tag = WA_Top;          wat[3].ti_Data = 0;
    wat[4].ti_Tag = WA_Width;        wat[4].ti_Data = w;
    wat[5].ti_Tag = TAG_DONE;        wat[5].ti_Data = 0;

    win = OpenWindowTagList(NULL, wat);
    if (win != NULL)
    {
        ULONG x;
        struct RastPort *rp = win->RPort;

        /* something to move: bars of pens */
        for (x = 0; x < w; x += 8)
        {
            SetAPen(rp, (x >> 3) & 255);
            RectFill(rp, x, 0, x + 7 < w ? x + 7 : w - 1, h - 1);
        }

        WaitBlit();

        t0 = NowMs();
        t1 = t0;

        while (t1 - t0 < secs * 1000)
        {
            ScrollRaster(rp, dx, dy, 0, 0, rw - 1, rh - 1);
            ScrollRaster(rp, -dx, -dy, 0, 0, rw - 1, rh - 1);
            ops += 2;
            t1 = NowMs();
        }

        WaitBlit();
        t1 = NowMs();

        Printf("ScrollRaster %ldx%ldx%ld, area %ldx%ld, dx %ld dy %ld: %ld operations in %ld ms = %ld ops/s\n", (LONG)w, (LONG)h, (LONG)depth, (LONG)rw, (LONG)rh,
               dx, dy, (LONG)ops, (LONG)(t1 - t0), (LONG)(ops * 1000 / (t1 - t0)));

        CloseWindow(win);
        rc = 0;
    }
    else
        Printf("OpenWindow failed\n");

    CloseScreen(scr);

done:
    if (P96Base)
        CloseLibrary(P96Base);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    CloseLibrary((struct Library *)DOSBase);

    return rc;
}
