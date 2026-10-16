/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    brcm-dma-rows -- what a rectangle costs, row by row. For every geometry and every number of rows the job is started and waited for N times
    (default 100) and printed as the average, in microseconds (the system timer, 1 us), of

      start   the return of BDMA_AddJobTagList(): the checks, the cache, the chain of control blocks, the start of the channel (what the CPU pays)
      wait    from there to the end of BDMA_WaitJob() (the task sleeps: what the engine still has to do, and the interrupt)

    The geometries (a row of WIDTH bytes, default 1280; the gap is 64 bytes, so the pitch is WIDTH + 64):

      joined      copy, the rows follow each other: one row for the resource
      gap16       copy, a gap, source and destination on 16 bytes, WIDTH a multiple of 16: one control block a row
      gap7        copy, a gap, the destination 7 bytes off: a head, a body and a tail for every row
      fill        fill of 32 bit pixels, a gap
      move        BDJ_Move by 16 rows, a gap

      brcm-dma-rows [N n] [WIDTH bytes] [NOCACHE]

    NOCACHE sets BDJ_NoCache: the cost of the engine alone, without the cache maintenance of the ranges.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/brcm-dma.h>

#include <resources/brcm-dma.h>

#include "brcm-dma.h"
#include "hw-vc4.h"     /* timer_now() */

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
APTR BrcmDmaBase;
struct BDMAClient *Client;

#define GAP        64
#define MAX_ROWS   1024
#define MOVE_ROWS  16

static ULONG Rows[] = { 1, 8, 32, 128, 256, 512, 1024 };

enum { G_JOINED, G_GAP16, G_GAP7, G_FILL, G_MOVE, G_COUNT };

static const char *Name[G_COUNT] = { "joined", "gap16", "gap7", "fill", "move" };

/* one job of the geometry, started and waited for; returns the start and the wait in us */
static LONG Job(ULONG geometry, UBYTE *s, UBYTE *d, ULONG width, ULONG rows, BOOL nocache, ULONG *start, ULONG *wait)
{
    ULONG pitch = geometry == G_JOINED ? width : width + GAP;
    ULONG t0, t1, t2;
    struct BDMAJob *job;
    ULONG code = BDERR_OK;

    t0 = timer_now();
    switch (geometry)
    {
        case G_FILL:
            job = BDMA_AddJobTags(Client, BDJ_FillValue, 0x12345678UL, BDJ_Dst, (ULONG)d, BDJ_Length, width, BDJ_Rows, rows, BDJ_DstPitch, pitch,
                                  BDJ_NoCache, nocache, BDJ_ErrorCode, (ULONG)&code, TAG_DONE);
            break;

        case G_MOVE:
            /* d is in the same block as s: the destination is MOVE_ROWS rows after the source */
            job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)s + MOVE_ROWS * pitch, BDJ_Length, width, BDJ_Rows, rows,
                                  BDJ_SrcPitch, pitch, BDJ_DstPitch, pitch, BDJ_Move, TRUE, BDJ_NoCache, nocache, BDJ_ErrorCode, (ULONG)&code, TAG_DONE);
            break;

        case G_GAP7:
            job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d + 7, BDJ_Length, width, BDJ_Rows, rows,
                                  BDJ_SrcPitch, pitch, BDJ_DstPitch, pitch, BDJ_NoCache, nocache, BDJ_ErrorCode, (ULONG)&code, TAG_DONE);
            break;

        default:
            job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, width, BDJ_Rows, rows,
                                  BDJ_SrcPitch, pitch, BDJ_DstPitch, pitch, BDJ_NoCache, nocache, BDJ_ErrorCode, (ULONG)&code, TAG_DONE);
            break;
    }
    t1 = timer_now();

    if (job == NULL)
        return (LONG)code;

    code = BDMA_WaitJob(job);
    t2 = timer_now();

    *start = t1 - t0;
    *wait = t2 - t1;

    return (LONG)code;
}

int main(int argc, char **argv, struct WBStartup *wbmsg)
{
    struct RDArgs *rda;
    LONG args[3] = { 0, 0, 0 };
    ULONG n = 100, width = 1280, available = 0, g, r, i;
    BOOL nocache = FALSE;
    UBYTE *s, *d, *block;
    ULONG pitch, size;

    (void)argc;
    (void)argv;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("N/K/N,WIDTH/K/N,NOCACHE/S", args, NULL);
    if (rda != NULL)
    {
        if (args[0] && *(ULONG *)args[0] != 0)
            n = *(ULONG *)args[0];
        if (args[1] && *(ULONG *)args[1] != 0)
            width = *(ULONG *)args[1];
        nocache = args[2] != 0;
        FreeArgs(rda);
    }
    if (width > 4096)
        width = 4096;

    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (BrcmDmaBase == NULL)
    {
        Printf("%s is not there\n", (LONG)BRCMDMANAME);
        return 10;
    }
    BDMA_QueryInfoTags(BDI_Available, (ULONG)&available, TAG_DONE);
    if (!available)
    {
        Printf("engine not available\n");
        return 10;
    }
    Client = BDMA_OpenClientTags(BDC_Name, (ULONG)"brcm-dma-rows", TAG_DONE);
    if (Client == NULL)
    {
        Printf("no client\n");
        return 10;
    }

    pitch = width + GAP;
    size = (MAX_ROWS + MOVE_ROWS + 1) * pitch + 64;
    block = AllocMem(2 * size, MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);
    if (block == NULL)
    {
        Printf("no memory\n");
        BDMA_CloseClient(Client);
        return 20;
    }
    s = (UBYTE *)(((ULONG)block + 63) & ~63UL);
    d = (UBYTE *)(((ULONG)s + size + 63) & ~63UL);
    if ((width & 15) != 0)
        Printf("WIDTH is not a multiple of 16: gap16 is not a job of one control block a row\n");

    Printf("rows of %ld bytes, a gap of %ld, %ld jobs a cell, %s; averages in us: start + wait (start per row)\n", (LONG)width, (LONG)GAP, (LONG)n,
           (LONG)(nocache ? "BDJ_NoCache" : "with the cache maintenance"));
    Printf("%-8s", (LONG)"rows");
    for (g = 0; g < G_COUNT; g++)
        Printf("  %-22s", (LONG)Name[g]);
    Printf("\n");

    /* a first job, so that nothing is measured while the first caller pays for the start of the engine */
    {
        ULONG a, b;
        Job(G_GAP16, s, d, width, 8, nocache, &a, &b);
    }

    for (r = 0; r < sizeof(Rows) / sizeof(Rows[0]); r++)
    {
        Printf("%-8ld", (LONG)Rows[r]);
        for (g = 0; g < G_COUNT; g++)
        {
            ULONG sum_start = 0, sum_wait = 0, start, wait;
            LONG e = BDERR_OK;

            for (i = 0; i < n && e == BDERR_OK; i++)
            {
                e = Job(g, s, d, width, Rows[r], nocache, &start, &wait);
                sum_start += start;
                sum_wait += wait;
            }
            if (e != BDERR_OK)
                Printf("  error %-16ld", e);
            else
                Printf("  %5ld + %-5ld (%3ld ns)", (LONG)(sum_start / n), (LONG)(sum_wait / n), (LONG)(sum_start * 1000 / n / Rows[r]));
        }
        Printf("\n");
    }

    FreeMem(block, 2 * size);
    BDMA_CloseClient(Client);
    CloseLibrary((struct Library *)DOSBase);

    return 0;
}
