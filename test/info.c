/*
    brcm-dma-info -- what brcm-dma.resource says about itself (read only): the engine, and the counters since the start of the OS.

      brcm-dma-info                 one report
      brcm-dma-info WATCH 5         the report, then every 5 seconds the activity of the interval: jobs and MB a second, load of each channel
      brcm-dma-info WATCH 5 COUNT 12    ... stops after 12 intervals (Ctrl-C stops it at any time)
      brcm-dma-info WATCH 5 LOG     ... a new pair of lines per interval, no escape codes (to redirect to a file)

    By default the two lines of an interval are drawn over the previous ones (console escape sequences: two lines up, erase), so the output stays short.

    Run it twice to see whether somebody else (VideoCore.card, with the ToolType VC6_DMA_RESOURCE=Yes) sends jobs to the resource: the counters move.
    The maxima (queue, wait) are those since the start of the OS, not of the interval.
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

struct Snap
{
    ULONG available, version, interrupt, model, features, channels, classes, maxrows, vbase, vsize;
    ULONG jobs, megabytes, failures, aborts, timeouts, busy, slices, deferred, queuemax, waitmax;
    ULONG busyms[2], size[5];
};

static const char * const sizeName[5] = { "<=64B", "<=4K", "<=32K", "<=1M", ">1M" };

static const struct { ULONG bit; const char *name; } featureName[] =
{
    { BDFF_RECT, "rect" }, { BDFF_MOVE, "move" }, { BDFF_WIDE, "wide" }, { BDFF_FILL, "fill" }, { BDFF_BIGBLOCKS, "bigblocks" }
};

static const struct { ULONG bit; const char *name; } className[] =
{
    { BDCLASS_DMA40, "dma40" }, { BDCLASS_NORMAL, "normal" }, { BDCLASS_LITE, "lite" }
};

static void Query(struct Snap *s)
{
    ULONG i;

    for (i = 0; i < sizeof(*s) / sizeof(ULONG); i++)
    {
        ((ULONG *)s)[i] = 0;
    }

    BDMA_QueryInfoTags(
        BDI_Available, (ULONG)&s->available,
        BDI_Version,   (ULONG)&s->version,
        BDI_Interrupt, (ULONG)&s->interrupt,
        BDI_Model,     (ULONG)&s->model,
        BDI_Features,  (ULONG)&s->features,
        BDI_Channels,  (ULONG)&s->channels,
        BDI_Classes,   (ULONG)&s->classes,
        BDI_MaxRows,   (ULONG)&s->maxrows,
        BDI_VideoBase, (ULONG)&s->vbase,
        BDI_VideoSize, (ULONG)&s->vsize,
        BDI_Jobs,      (ULONG)&s->jobs,
        BDI_MegaBytes, (ULONG)&s->megabytes,
        BDI_Failures,  (ULONG)&s->failures,
        BDI_Aborts,    (ULONG)&s->aborts,
        BDI_Timeouts,  (ULONG)&s->timeouts,
        BDI_Busy,      (ULONG)&s->busy,
        BDI_Slices,    (ULONG)&s->slices,
        BDI_Deferred,  (ULONG)&s->deferred,
        BDI_QueueMax,  (ULONG)&s->queuemax,
        BDI_WaitMaxUs, (ULONG)&s->waitmax,
        BDI_BusyMs0,   (ULONG)&s->busyms[0],
        BDI_BusyMs1,   (ULONG)&s->busyms[1],
        BDI_Size64,    (ULONG)&s->size[0],
        BDI_Size4K,    (ULONG)&s->size[1],
        BDI_Size32K,   (ULONG)&s->size[2],
        BDI_Size1M,    (ULONG)&s->size[3],
        BDI_SizeBig,   (ULONG)&s->size[4],
        TAG_DONE);
}

/* The channels managed, in ascending order: the first two name the columns of the busy times */
static ULONG ChannelNumber(ULONG mask, ULONG index)
{
    ULONG n;

    for (n = 0; n < 32; n++)
    {
        if ((mask & (1UL << n)) && index-- == 0)
        {
            return n;
        }
    }

    return 99;
}

static void Report(const struct Snap *s)
{
    ULONG i;

    Printf("%s %ld.%ld: %s, %s, interrupt %s\n", (LONG)BRCMDMANAME, (LONG)(s->version >> 16), (LONG)(s->version & 0xffff),
        (LONG)(s->available ? "available" : "NO ENGINE"),
        (LONG)(s->model == BDM_BCM2711 ? "BCM2711" : s->model == BDM_BCM2835 ? "BCM2835 family" : "unknown SoC"),
        (LONG)(s->interrupt ? "yes" : "no"));

    if (!s->available)
    {
        return;
    }

    Printf("channels:");
    for (i = 0; i < 32; i++)
    {
        if (s->channels & (1UL << i))
        {
            Printf(" %ld", (LONG)i);
        }
    }

    Printf("; classes:");
    for (i = 0; i < sizeof(className) / sizeof(className[0]); i++)
    {
        if (s->classes & className[i].bit)
        {
            Printf(" %s", (LONG)className[i].name);
        }
    }

    Printf("; features:");
    for (i = 0; i < sizeof(featureName) / sizeof(featureName[0]); i++)
    {
        if (s->features & featureName[i].bit)
        {
            Printf(" %s", (LONG)featureName[i].name);
        }
    }

    Printf("; max rows %ld\n", (LONG)s->maxrows);
    Printf("RTG memory: %08lx, %ld KB\n", s->vbase, (LONG)(s->vsize >> 10));
    Printf("jobs %ld (%ld MB), failed %ld, aborted %ld, timeouts %ld, busy %ld, slices %ld, deferred %ld\n",
        (LONG)s->jobs, (LONG)s->megabytes, (LONG)s->failures, (LONG)s->aborts, (LONG)s->timeouts, (LONG)s->busy, (LONG)s->slices, (LONG)s->deferred);
    Printf("queue max %ld, longest wait %ld us; busy: channel %ld %ld ms, channel %ld %ld ms\n",
        (LONG)s->queuemax, (LONG)s->waitmax,
        (LONG)ChannelNumber(s->channels, 0), (LONG)s->busyms[0], (LONG)ChannelNumber(s->channels, 1), (LONG)s->busyms[1]);
    Printf("sizes:");
    for (i = 0; i < 5; i++)
    {
        Printf(" %s %ld", (LONG)sizeName[i], (LONG)s->size[i]);
    }

    Printf("\n");
}

/* The activity between two reads; ms is the time between them */
static void Interval(const struct Snap *a, const struct Snap *b, ULONG ms, BOOL redraw)
{
    ULONG i, mbs10 = ms ? (b->megabytes - a->megabytes) * 10000 / ms : 0;

    if (redraw)
    {
        Printf("\x9b" "2A\r" "\x9b" "J");     /* CSI 2 A: two lines up; CSI J: erase to the end of the display */
    }

    Printf("%lds %ld jobs/s %ld.%ld MB/s ch%ld %ld%% ch%ld %ld%% wait %ld us q %ld",
        (LONG)((ms + 500) / 1000),
        (LONG)(ms ? (b->jobs - a->jobs) * 1000 / ms : 0), (LONG)(mbs10 / 10), (LONG)(mbs10 % 10),
        (LONG)ChannelNumber(b->channels, 0), (LONG)(ms ? (b->busyms[0] - a->busyms[0]) * 100 / ms : 0),
        (LONG)ChannelNumber(b->channels, 1), (LONG)(ms ? (b->busyms[1] - a->busyms[1]) * 100 / ms : 0),
        (LONG)b->waitmax, (LONG)b->queuemax);

    if (b->failures != a->failures) Printf(" +%ld fail", (LONG)(b->failures - a->failures));
    if (b->aborts != a->aborts)     Printf(" +%ld abort", (LONG)(b->aborts - a->aborts));
    if (b->timeouts != a->timeouts) Printf(" +%ld tmo", (LONG)(b->timeouts - a->timeouts));
    if (b->busy != a->busy)         Printf(" +%ld busy", (LONG)(b->busy - a->busy));
    if (b->deferred != a->deferred) Printf(" +%ld defer", (LONG)(b->deferred - a->deferred));

    Printf("\n   sizes:");
    for (i = 0; i < 5; i++)
    {
        Printf(" %s %ld", (LONG)sizeName[i], (LONG)(b->size[i] - a->size[i]));
    }

    Printf("\n");
}

static ULONG NowMs(void)
{
    struct DateStamp ds;

    DateStamp(&ds);

    return (ds.ds_Minute * 60UL * 50UL + ds.ds_Tick) * 20UL;
}

static BOOL Interrupted(void)
{
    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

int main(int argc, struct WBStartup *wbmsg)
{
    struct Snap before, after;
    struct RDArgs *rda;
    LONG args[3] = { 0, 0, 0 };
    ULONG watch = 0, count = 0, shown = 0, t0, t1, i;
    BOOL log;
    int rc = 0;

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("WATCH/K/N,COUNT/K/N,LOG/S", args, NULL);
    if (rda == NULL)
    {
        Printf("usage: brcm-dma-info [WATCH seconds [COUNT intervals] [LOG]]\n");
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    if (args[0]) watch = *(ULONG *)args[0];
    if (args[1]) count = *(ULONG *)args[1];
    log = args[2] != 0;
    FreeArgs(rda);

    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (BrcmDmaBase == NULL)
    {
        Printf("%s is not there\n", (LONG)BRCMDMANAME);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    Query(&before);
    Report(&before);

    if (watch != 0 && before.available)
    {
        t0 = NowMs();

        while (!Interrupted() && (count == 0 || shown < count))
        {
            for (i = 0; i < watch && !Interrupted(); i++)
            {
                Delay(50);
            }

            if (Interrupted())
            {
                break;
            }

            Query(&after);
            t1 = NowMs();
            Interval(&before, &after, t1 >= t0 ? t1 - t0 : t1 + 1440UL * 60UL * 50UL * 20UL - t0, shown > 0 && !log);
            before = after;
            t0 = t1;
            shown++;
        }
    }
    else if (watch != 0)
    {
        rc = 5;
    }

    CloseLibrary((struct Library *)DOSBase);

    return rc;
}
