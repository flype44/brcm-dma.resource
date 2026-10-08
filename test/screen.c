/*
    brcm-dma-screen -- opens a 32 bit RTG screen of the size asked for, makes it the default public screen, and keeps it until it is told to stop.
    The programs that open their windows on the default public screen (vcwin, the benchmarks) then work at that resolution, without a restart.

      brcm-dma-screen 1920 1080            until Ctrl-C, or until the file T:bdma.stop exists
      brcm-dma-screen 1280 720 SECS 120    ... or after 120 seconds

    On exit the default public screen is the Workbench again, and the screen is closed (once its visitors are gone).
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <libraries/Picasso96.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/Picasso96.h>

#define STOPFILE "T:bdma.stop"
#define PUBNAME "BDMA"

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
struct IntuitionBase * IntuitionBase;
struct GfxBase * GfxBase;
struct Library * P96Base;

static BOOL StopAsked(void)
{
    BPTR lock = Lock(STOPFILE, ACCESS_READ);

    if (lock != 0)
    {
        UnLock(lock);
        return TRUE;
    }

    return FALSE;
}

int main(int argc, struct WBStartup *wbmsg)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rda;
    struct Screen *scr;
    ULONG w, h, secs = 0, id, err = 0, i;
    int rc = 10;
    struct TagItem bid[5];
    struct TagItem sat[10];

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("W/N/A,H/N/A,SECS/K/N", args, NULL);
    if (rda == NULL)
    {
        Printf("usage: brcm-dma-screen width height [SECS seconds]\n");
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    w = *(ULONG *)args[0];
    h = *(ULONG *)args[1];
    if (args[2])
        secs = *(ULONG *)args[2];
    FreeArgs(rda);

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
    bid[2].ti_Tag = P96BIDTAG_Depth;          bid[2].ti_Data = 32;
    bid[3].ti_Tag = P96BIDTAG_FormatsAllowed; bid[3].ti_Data = RGBFF_B8G8R8A8;
    bid[4].ti_Tag = TAG_DONE;                 bid[4].ti_Data = 0;

    id = p96BestModeIDTagList(bid);
    if (id == INVALID_ID)
    {
        Printf("no %ldx%ldx32 RTG mode\n", (LONG)w, (LONG)h);
        goto done;
    }

    sat[0].ti_Tag = SA_DisplayID;  sat[0].ti_Data = id;
    sat[1].ti_Tag = SA_Width;      sat[1].ti_Data = w;
    sat[2].ti_Tag = SA_Height;     sat[2].ti_Data = h;
    sat[3].ti_Tag = SA_Depth;      sat[3].ti_Data = 32;
    sat[4].ti_Tag = SA_PubName;    sat[4].ti_Data = (ULONG)PUBNAME;
    sat[5].ti_Tag = SA_Quiet;      sat[5].ti_Data = TRUE;
    sat[6].ti_Tag = SA_ShowTitle;  sat[6].ti_Data = FALSE;
    sat[7].ti_Tag = SA_ErrorCode;  sat[7].ti_Data = (ULONG)&err;
    sat[8].ti_Tag = SA_Type;       sat[8].ti_Data = PUBLICSCREEN;
    sat[9].ti_Tag = TAG_DONE;      sat[9].ti_Data = 0;

    scr = OpenScreenTagList(NULL, sat);
    if (scr == NULL)
    {
        Printf("OpenScreen failed, error %ld\n", (LONG)err);
        goto done;
    }

    if ((ULONG)scr->Width != w || (ULONG)scr->Height != h)
    {
        Printf("the mode is %ldx%ld, not %ldx%ld\n", (LONG)scr->Width, (LONG)scr->Height, (LONG)w, (LONG)h);
    }

    PubScreenStatus(scr, 0);                 /* from private to public */
    SetDefaultPubScreen(PUBNAME);
    ScreenToFront(scr);

    Printf("screen %ldx%ldx32 is the default public screen\n", (LONG)scr->Width, (LONG)scr->Height);

    DeleteFile(STOPFILE);                    /* a stop file of an earlier run must not stop this one */

    for (i = 0; ; i++)
    {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;

        if (secs != 0 && i >= secs * 5)
            break;

        if (StopAsked())
            break;

        Delay(10);
    }

    DeleteFile(STOPFILE);
    SetDefaultPubScreen(NULL);               /* the Workbench again */
    PubScreenStatus(scr, PSNF_PRIVATE);

    for (i = 0; i < 100 && !CloseScreen(scr); i++)
        Delay(10);

    rc = 0;

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
