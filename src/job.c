/*
    Copyright @ 2025 Michal Schulz <michal.schulz@gmx.de>
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

    if (!BDMA_InMemory(BDMABase, r->bdr_Dst, dstbytes))
    {
        return BDERR_RANGE;
    }

    if (!fill)
    {
        srcbytes = (rows - 1) * r->bdr_SrcPitch + length;
        job->bj_ReadLo = r->bdr_Src;
        job->bj_ReadHi = r->bdr_Src + srcbytes;

        if (!BDMA_InMemory(BDMABase, r->bdr_Src, srcbytes))
        {
            return BDERR_RANGE;
        }

        if (r->bdr_Given & BDRF_MOVE)
        {
            /* by whole rows only:
               a shift inside a row (less than the width) is refused, 
               a scroll never does it */
            LONG gap = (LONG)r->bdr_Dst - (LONG)r->bdr_Src;

            if (gap > -(LONG)length && gap < (LONG)length)
            {
                return BDERR_OVERLAP;
            }

            job->bj_Reverse = r->bdr_Dst > r->bdr_Src;
        }
        else if (job->bj_ReadLo < job->bj_WriteHi && job->bj_WriteLo < job->bj_ReadHi)
        {
            return BDERR_OVERLAP;
        }
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

/* Stops the job, in the queue or on its channel,
   and replies it with BDERR_ABORTED;
   a job that is over already is left alone
*/
void BDMA_AbortInternal(
    struct BDMABase *BDMABase, 
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    BOOL aborted = FALSE;

    Disable();

    if (job->bj_State == BJS_QUEUED)
    {
        Remove(BDMA_JOBNODE(job));
        BDMABase->bdb_Waiting--;
        aborted = TRUE;
    }
    else if (job->bj_State == BJS_RUNNING && job->bj_Starting)
    {
        /* picked, its chain not armed yet (the starter was preempted, or is in the interrupt): the channel is not touched
           and the job not replied here; StartChain() sees the request, gives the channel back and replies */
        job->bj_AbortReq = 1;
    }
    else if (job->bj_State == BJS_RUNNING)
    {
        struct BDMAChannel *channel = &BDMABase->bdb_Channel[job->bj_Channel];

        BDMABase->bdb_Backend->Stop(channel);
        channel->bc_Job = NULL;
        aborted = TRUE;
    }

    if (aborted)
    {
        BDMABase->bdb_Aborts++;
        job->bj_State = BJS_ABORTED;
        job->bj_Error = BDERR_ABORTED;
        job->bj_Done = 0;
    }

    Enable();

    if (aborted)
    {
        BDMA_ReplyJob(BDMABase, job);

        /* the channel it held, and what it was holding back */
        BDMA_Run(BDMABase);
    }
}

/* The software interrupt that Finish() causes:
   for each job that is over, the cache (what the DMA wrote is in the memory,
   not in the data cache; nothing to do for the source, which the DMA only read),
   then the reply. A job of a few KB costs lines, a big one the call of the system:
   that is why it is not done by the interrupt of the channel.
*/
ULONG BDMA_DoneCode(
    REGARG(struct BDMABase *BDMABase, "a1"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;

    for (;;)
    {
        struct BDMAJob *job;
        struct Node *node;

        Disable();
        node = (struct Node *)RemHead((struct List *)&BDMABase->bdb_Done);
        Enable();

        if (node == NULL)
            break;

        job = BDMA_NODEJOB(node);

        if (job->bj_State == BJS_DONE && 
            !(job->bj_Request.bdr_Given & BDRF_NOCACHE) && 
            !BDMA_InVideoMemory(BDMABase, job->bj_WriteLo, job->bj_WriteHi))
        {
            ULONG bytes = job->bj_WriteHi - job->bj_WriteLo;

            if (bytes <= BDMA_LINEWISE_MAX)
            {
                BDMA_DropLines(job->bj_WriteLo, bytes);
            }
            else
            {
                LONG l = bytes;
                CachePostDMA((APTR)job->bj_WriteLo, &l, 0);
            }
        }

        BDMA_ReplyJob(BDMABase, job);
    }

    return 0;
}
