/*
    brcm-dma-info -- what brcm-dma.resource says about itself (read only): availability, version, model, features, channels, classes, the memory of the RTG
    board, and the counters: jobs done, megabytes moved, jobs that failed. Run it twice to see whether somebody else (VideoCore.card, with the ToolType
    VC6_DMA_RESOURCE=Yes) sends jobs to the resource: the counters move.

      brcm-dma-info
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/brcm-dma.h>

#include <resources/brcm-dma.h>

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
APTR BrcmDmaBase;

int main(int argc, struct WBStartup *wbmsg)
{
    ULONG available = 0, version = 0, model = 0, features = 0, channels = 0, classes = 0, maxrows = 0;
    ULONG jobs = 0, megabytes = 0, failures = 0, vbase = 0, vsize = 0, interrupt = 0;

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (BrcmDmaBase == NULL)
    {
        Printf("%s is not there\n", (LONG)BRCMDMANAME);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    BDMA_QueryInfoTags(
        BDI_Available, (ULONG)&available,
        BDI_Version,   (ULONG)&version,
        BDI_Interrupt, (ULONG)&interrupt,
        BDI_Model,     (ULONG)&model,
        BDI_Features,  (ULONG)&features,
        BDI_Channels,  (ULONG)&channels,
        BDI_Classes,   (ULONG)&classes,
        BDI_MaxRows,   (ULONG)&maxrows,
        BDI_VideoBase, (ULONG)&vbase,
        BDI_VideoSize, (ULONG)&vsize,
        BDI_Jobs,      (ULONG)&jobs,
        BDI_MegaBytes, (ULONG)&megabytes,
        BDI_Failures,  (ULONG)&failures,
        TAG_DONE);

    Printf("%s %ld.%ld: %s, interrupt %ld, model %ld, features %08lx, channels %08lx, classes %lx, max rows %ld\n",
           (LONG)BRCMDMANAME,
           (LONG)(version >> 16),
           (LONG)(version & 0xffff),
           (LONG)(available ? "available" : "no engine"),
           (LONG)interrupt,
           (LONG)model,
           features,
           channels,
           classes,
           (LONG)maxrows);
    Printf("memory of the RTG board: %08lx, %ld KB\n", vbase, (LONG)(vsize >> 10));
    Printf("jobs done: %ld, megabytes moved: %ld, failures: %ld\n", (LONG)jobs, (LONG)megabytes, (LONG)failures);

    CloseLibrary((struct Library *)DOSBase);

    return 0;
}
