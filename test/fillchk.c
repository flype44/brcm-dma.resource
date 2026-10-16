/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    brcm-dma-fillchk -- does a FillRect that the driver sends to the DMA give the same pixels as the FillRect of Picasso96 itself, in every pixel format?

    For each format (8 bit, the 15 and 16 bit ones in both byte orders, 24 and 32 bit) it opens a 640x480 RTG screen, allocates a bitmap on the board (displayable, friend
    of the screen) and a second one in RAM (BMF_USERPRIVATE, which Picasso96 fills with its CPU routines), fills both with the same pseudo random bytes, fills the same rectangle with
    the same colour in both with p96RectFill() and compares both whole bitmaps. The cases have odd columns, one pixel wide rows, rows of the whole width and big areas. It also
    reads the number of jobs of brcm-dma.resource before and after the cases of a format: if it did not move, the fills were done by the CPU.

      brcm-dma-fillchk [FORMAT name]

    FORMAT is one of CLUT, R5G6B5PC, R5G6B5, B5G6R5PC, R5G5B5PC, R5G5B5, B5G5R5PC, R8G8B8, B8G8R8, B8G8R8A8, A8R8G8B8; without it, all of them. For the DMA to take the small cases the
    threshold of the fills must be low: `vcdmaset.v2 0 1024` before, the one of the icon (VC6_DMA_THRESHOLD_FILLRECT) otherwise.

    The exit code is 5 if a case is wrong. Scratch test code of the laboratory.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <libraries/Picasso96.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/Picasso96.h>
#include <proto/brcm-dma.h>

#include <resources/brcm-dma.h>

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
struct IntuitionBase * IntuitionBase;
struct GfxBase * GfxBase;
struct Library * P96Base;
APTR BrcmDmaBase;

#define BW 400
#define BH 300

struct Format
{
    const char *name;
    ULONG rgbfb;
    ULONG rgbff;
    ULONG depth;
    ULONG bytes;
};

static const struct Format Formats[] = {
    { "CLUT",     RGBFB_CLUT,      RGBFF_CLUT,      8,  1 },
    { "R5G6B5PC", RGBFB_R5G6B5PC,  RGBFF_R5G6B5PC,  16, 2 },
    { "R5G6B5",   RGBFB_R5G6B5,    RGBFF_R5G6B5,    16, 2 },
    { "B5G6R5PC", RGBFB_B5G6R5PC,  RGBFF_B5G6R5PC,  16, 2 },
    { "R5G5B5PC", RGBFB_R5G5B5PC,  RGBFF_R5G5B5PC,  15, 2 },
    { "R5G5B5",   RGBFB_R5G5B5,    RGBFF_R5G5B5,    15, 2 },
    { "B5G5R5PC", RGBFB_B5G5R5PC,  RGBFF_B5G5R5PC,  15, 2 },
    { "R8G8B8",   RGBFB_R8G8B8,    RGBFF_R8G8B8,    24, 3 },
    { "B8G8R8",   RGBFB_B8G8R8,    RGBFF_B8G8R8,    24, 3 },
    { "B8G8R8A8", RGBFB_B8G8R8A8,  RGBFF_B8G8R8A8,  32, 4 },
    { "A8R8G8B8", RGBFB_A8R8G8B8,  RGBFF_A8R8G8B8,  32, 4 },
    { NULL, 0, 0, 0, 0 }
};

static ULONG Seed = 4242;

static ULONG Rnd(void)
{
    Seed = Seed * 1664525UL + 1013904223UL;

    return Seed ^ (Seed >> 15);
}

struct Case
{
    WORD x, y, w, h;
    ULONG value;
};

/* the colour: 0x00RRGGBB for a true colour format, a pen for the 8 bit one (the low byte) */
static const struct Case Cases[] = {
    { 20, 20, 40, 30, 0x00FF8040UL },
    { 21, 10, 64, 40, 0x00123456UL },
    { 33, 33, 101, 60, 0x00ABCDEFUL },
    { 7, 5, 255, 17, 0x00000000UL },
    { 4, 4, 380, 280, 0x00FFFFFFUL },
    { 50, 100, 129, 100, 0x00FF0000UL },
    { 3, 150, 31, 140, 0x0000FF00UL },
    { 100, 1, 17, 290, 0x000000FFUL },
    { 12, 12, 300, 3, 0x00010203UL },
    { 0, 20, BW, 5, 0x0055AA33UL },          /* rows of the whole width: they follow each other */
    { 0, 0, BW, BH, 0x00C0FFEEUL },          /* the whole bitmap */
    { 9, 60, 1, 120, 0x00778899UL },         /* one pixel wide */
    { 11, 70, 3, 1, 0x00445566UL },          /* tiny */
    { 0, 0, 0, 0, 0 }
};

static ULONG Jobs(void)
{
    ULONG jobs = 0;

    if (BrcmDmaBase != NULL)
        BDMA_QueryInfoTags(BDI_Jobs, (ULONG)&jobs, TAG_DONE);

    return jobs;
}

/* One format; returns the number of wrong cases, or -1 if it cannot be tried */
static LONG TryFormat(const struct Format *f)
{
    struct Screen *scr = NULL;
    struct BitMap *bm[2] = { NULL, NULL };
    struct RastPort rp[2];
    UBYTE *mem[2];
    ULONG bpr[2], id, i, c, fail = 0, ok = 0, jobs0, jobs1, n;
    LONG rc = -1;
    struct TagItem bid[] = {
        { P96BIDTAG_NominalWidth, 640 },
        { P96BIDTAG_NominalHeight, 480 },
        { P96BIDTAG_Depth, 0 },
        { P96BIDTAG_FormatsAllowed, 0 },
        { TAG_DONE, 0 }
    };

    bid[2].ti_Data = f->depth;
    bid[3].ti_Data = f->rgbff;
    id = p96BestModeIDTagList(bid);
    if (id == INVALID_ID)
    {
        Printf("%-9s no 640x480x%ld mode\n", (LONG)f->name, (LONG)f->depth);
        return -1;
    }

    {
        ULONG err = 0;
        struct TagItem sat[] = {
            { P96SA_DisplayID, 0 },
            { P96SA_Width, 640 },
            { P96SA_Height, 480 },
            { P96SA_Depth, 0 },
            { P96SA_Quiet, TRUE },
            { P96SA_ShowTitle, FALSE },
            { P96SA_ErrorCode, 0 },
            { TAG_DONE, 0 }
        };

        sat[0].ti_Data = id;
        sat[3].ti_Data = f->depth;
        sat[6].ti_Data = (ULONG)&err;
        scr = p96OpenScreenTagList(sat);
        if (scr == NULL)
        {
            Printf("%-9s p96OpenScreen failed, error %ld\n", (LONG)f->name, (LONG)err);
            return -1;
        }
    }

    bm[0] = p96AllocBitMap(BW, BH, f->depth, BMF_DISPLAYABLE, scr->RastPort.BitMap, f->rgbfb);
    bm[1] = p96AllocBitMap(BW, BH, f->depth, BMF_USERPRIVATE, NULL, f->rgbfb);
    if (bm[0] == NULL || bm[1] == NULL)
    {
        Printf("%-9s cannot allocate the bitmaps\n", (LONG)f->name);
        goto done;
    }

    for (i = 0; i < 2; i++)
    {
        mem[i] = (UBYTE *)p96GetBitMapAttr(bm[i], P96BMA_MEMORY);
        bpr[i] = p96GetBitMapAttr(bm[i], P96BMA_BYTESPERROW);
        InitRastPort(&rp[i]);
        rp[i].BitMap = bm[i];
    }

    if (mem[0] == NULL || mem[1] == NULL || !p96GetBitMapAttr(bm[0], P96BMA_ISONBOARD) || p96GetBitMapAttr(bm[1], P96BMA_ISONBOARD) || bpr[0] != bpr[1])
    {
        Printf("%-9s not a (board, RAM) pair with the same rows (on board %ld and %ld, rows %ld and %ld)\n", (LONG)f->name,
               (LONG)p96GetBitMapAttr(bm[0], P96BMA_ISONBOARD), (LONG)p96GetBitMapAttr(bm[1], P96BMA_ISONBOARD), (LONG)bpr[0], (LONG)bpr[1]);
        goto done;
    }

    n = bpr[0] * BH;
    jobs0 = Jobs();

    for (c = 0; Cases[c].w; c++)
    {
        const struct Case *k = &Cases[c];
        ULONG bad = 0, first = 0;

        for (i = 0; i < n; i += 4)
        {
            ULONG v = Rnd();

            *(volatile ULONG *)(mem[0] + i) = v;
            *(volatile ULONG *)(mem[1] + i) = v;
        }

        p96RectFill(&rp[0], k->x, k->y, k->x + k->w - 1, k->y + k->h - 1, k->value);
        p96RectFill(&rp[1], k->x, k->y, k->x + k->w - 1, k->y + k->h - 1, k->value);
        WaitBlit();

        for (i = 0; i < n; i++)
            if (((volatile UBYTE *)mem[0])[i] != ((volatile UBYTE *)mem[1])[i])
            {
                if (!bad)
                    first = i;
                bad++;
            }

        if (bad)
        {
            fail++;
            Printf("  FAIL %ld x %ld at (%ld,%ld) colour %08lx: %ld bytes differ, first at row %ld byte %ld: board %02lx RAM %02lx\n", (LONG)k->w, (LONG)k->h, (LONG)k->x,
                   (LONG)k->y, k->value, (LONG)bad, (LONG)(first / bpr[0]), (LONG)(first % bpr[0]), (ULONG)((volatile UBYTE *)mem[0])[first],
                   (ULONG)((volatile UBYTE *)mem[1])[first]);
        }
        else
            ok++;
    }

    jobs1 = Jobs();
    Printf("%-9s %ld ok, %ld wrong; DMA jobs %ld\n", (LONG)f->name, (LONG)ok, (LONG)fail, (LONG)(jobs1 - jobs0));
    rc = (LONG)fail;

done:
    if (bm[0])
        p96FreeBitMap(bm[0]);
    if (bm[1])
        p96FreeBitMap(bm[1]);
    if (scr)
        CloseScreen(scr);

    return rc;
}

static BOOL Same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        char x = *a, y = *b;

        if (x >= 'a' && x <= 'z')
            x -= 'a' - 'A';
        if (y >= 'a' && y <= 'z')
            y -= 'a' - 'A';
        if (x != y)
            return FALSE;
    }

    return *a == *b;
}

int main(int argc, char **argv, struct WBStartup *wbmsg)
{
    struct RDArgs *rda;
    LONG args[1] = { 0 };
    LONG rc = 0, r;
    ULONG i;
    BOOL any = FALSE;

    (void)argc;
    (void)argv;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("FORMAT/K", args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), NULL);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 36);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 36);
    P96Base = OpenLibrary("Picasso96API.library", 2);
    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (!IntuitionBase || !GfxBase || !P96Base)
    {
        Printf("cannot open the libraries\n");
        rc = 20;
    }
    else
    {
        if (BrcmDmaBase == NULL)
            Printf("brcm-dma.resource is not there: the jobs are not counted\n");

        for (i = 0; Formats[i].name; i++)
        {
            if (args[0] && !Same((const char *)args[0], Formats[i].name))
                continue;

            any = TRUE;
            r = TryFormat(&Formats[i]);
            if (r > 0)
                rc = 5;
        }
        if (!any)
        {
            Printf("unknown format\n");
            rc = 10;
        }
    }

    FreeArgs(rda);
    if (P96Base)
        CloseLibrary(P96Base);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    CloseLibrary((struct Library *)DOSBase);

    return rc;
}
