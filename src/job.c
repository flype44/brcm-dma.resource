/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <common/compiler.h>

#include "brcm-dma.h"
#include "cache.h"

/* Checks what the tags could not know: the memory, the overlap, the sizes;
   fills the footprint of the job */
LONG BDMA_CheckRequest(
    struct BDMABase *BDMABase, 
    struct BDMAJob *job)
{
    const struct BDMARequest *r = &job->bj_Request;
    ULONG length = r->bdr_Length, rows = r->bdr_Rows;
    ULONG dstbytes, srcbytes = 0;
    BOOL fill = (r->bdr_Given & BDRF_FILL) != 0;

    job->bj_Bounce = 0;

    /* no channel of the classes that the job allows */
    if (!(r->bdr_Classes & BDMABase->bdb_Classes))
    {
        return BDERR_UNAVAILABLE;
    }

    if (length > 0x3fffffffUL)
    {
        return BDERR_TOOBIG;
    }

    dstbytes = (rows - 1) * r->bdr_DstPitch + length;
    job->bj_WriteLo = r->bdr_Dst;
    job->bj_WriteHi = r->bdr_Dst + dstbytes;

    if (!BDMA_InMemory(BDMABase, job->bj_Client, r->bdr_Dst, dstbytes, TRUE))
    {
        job->bj_Request.bdr_BadAddress = r->bdr_Dst;

        return BDERR_RANGE;
    }

    job->bj_WriteUncached = BDMA_IsUncached(BDMABase, job->bj_Client, job->bj_WriteLo, job->bj_WriteHi);
    job->bj_ReadUncached = 0;
    job->bj_Stale = 0;

    if (!fill)
    {
        srcbytes = (rows - 1) * r->bdr_SrcPitch + length;
        job->bj_ReadLo = r->bdr_Src;
        job->bj_ReadHi = r->bdr_Src + srcbytes;

        if (!BDMA_InMemory(BDMABase, job->bj_Client, r->bdr_Src, srcbytes, FALSE))
        {
            job->bj_Request.bdr_BadAddress = r->bdr_Src;

            return BDERR_RANGE;
        }

        job->bj_ReadUncached = BDMA_IsUncached(BDMABase, job->bj_Client, job->bj_ReadLo, job->bj_ReadHi);

        if (r->bdr_Given & BDRF_MOVE)
        {
            /* A shift by whole rows (a scroll) is done by the order of the rows. A shift inside a row (less than the width) is done
               through the buffer of the channel, which must hold a row. */
            LONG gap = (LONG)r->bdr_Dst - (LONG)r->bdr_Src;

            if (gap > -(LONG)length && gap < (LONG)length)
            {
                if (length > BDMA_BOUNCE_SIZE)
                {
                    return BDERR_OVERLAP;
                }

                job->bj_Bounce = 1;
            }

            job->bj_Reverse = r->bdr_Dst > r->bdr_Src;
        }
        else if (job->bj_ReadLo < job->bj_WriteHi && job->bj_WriteLo < job->bj_ReadHi)
        {
            return BDERR_OVERLAP;
        }
    }

    /* The geometry that the engine runs. Rows that follow each other, in the source and in the destination (a pitch equal to the length), are one row of all the
       bytes: a rectangle costs the CPU a block of control for each row (about 0.5 us a row, 645 us for a picture of 1080 rows of 1280 bytes), one row costs the
       same whatever its length (173 us for the same bytes). A move keeps its rows: it is done by their order. The request itself is not changed. */
    job->bj_Width = length;
    job->bj_Height = rows;
    job->bj_SrcStep = r->bdr_SrcPitch;
    job->bj_DstStep = r->bdr_DstPitch;

    if (rows > 1 &&
        !(r->bdr_Given & BDRF_MOVE) &&
        r->bdr_DstPitch == length &&
        (fill || r->bdr_SrcPitch == length) &&
        length <= 0x3fffffffUL / rows)
    {
        job->bj_Width = length * rows;
        job->bj_Height = 1;
        job->bj_SrcStep = job->bj_Width;
        job->bj_DstStep = job->bj_Width;
    }

    return BDERR_OK;
}

/* The job goes back to its reply port:
   the message is replied, a task wakes or a software interrupt is caused
*/
void BDMA_ReplyJob(
    struct BDMABase *BDMABase, 
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct Task *waiter;

    ReplyMsg(&job->bj_Msg);

    /* a task that does not own the port sleeps on a signal of its own */
    waiter = job->bj_Waiter;
    if (waiter != NULL)
    {
        Signal(waiter, job->bj_WaitMask);
    }
}

/* Stops the job, in the queue or on its channels,
   and replies it with BDERR_ABORTED (when a slice is being armed, its starter does it a moment later);
   a job that is over already is left alone
*/
void BDMA_AbortInternal(
    struct BDMABase *BDMABase,
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    BOOL settled = FALSE;
    ULONG ipl;

    ipl = BDMA_Lock();

    if (job->bj_State == BJS_QUEUED || job->bj_State == BJS_RUNNING)
    {
        settled = BDMA_KillJob(BDMABase, job, BDERR_ABORTED);
    }

    BDMA_Unlock(ipl);

    if (settled)
    {
        BDMA_ReplyJob(BDMABase, job);

        /* the channels it held, and what it was holding back */
        BDMA_Run(BDMABase);
    }
}

/* The software interrupt that Finish() causes:
   for each job that is over, the cache (what the DMA wrote is in the memory,
   not in the data cache; nothing to do for the source, which the DMA only read),
   then the reply. The maintenance of a big area is too long for the interrupt of the channel
   (above BDMA_INTERRUPT_MAX).
*/
ULONG BDMA_DoneCode(
    REGARG(struct BDMABase *BDMABase, "a1"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;

    for (;;)
    {
        struct BDMAJob *job;
        struct Node *node;
        ULONG ipl;

        ipl = BDMA_Lock();
        node = (struct Node *)RemHead((struct List *)&BDMABase->bdb_Done);
        BDMA_Unlock(ipl);

        if (node == NULL)
            break;

        job = BDMA_NODEJOB(node);

        if (job->bj_State == BJS_DONE && 
            !(job->bj_Request.bdr_Given & BDRF_NOCACHE) && 
            !job->bj_WriteUncached)
        {
            BDMA_CacheAfter(SysBase, job->bj_WriteLo, job->bj_WriteHi - job->bj_WriteLo);
        }

        BDMA_ReplyJob(BDMABase, job);
    }

    return 0;
}
