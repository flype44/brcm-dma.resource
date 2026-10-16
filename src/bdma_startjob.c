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
#include <exec/ports.h>
#include <proto/exec.h>
#include <common/compiler.h>

#include "brcm-dma.h"
#include "hw-vc4.h"     /* timer_now() */
#include "cache.h"

/* Queues the job, which was described by BDMA_AllocJobTagList():
   no allocation and no parsing here, so a software interrupt may call it (the next block
   of a transfer, for instance). The job is replied to its port when it is over. With BDJ_NoWait and every channel busy it is replied at once, with BDERR_BUSY.
   A job that is in flight is left alone.
*/
VOID L_BDMA_StartJob(
    REGARG(struct BDMAJob *job, "a0"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    const struct BDMARequest *r;
    struct MsgPort *port;
    ULONG ipl;

    if (job == NULL || job->bj_State < BJS_DONE)
    {
        return;
    }

    r = &job->bj_Request;
    port = r->bdr_ReplyPort != NULL ? r->bdr_ReplyPort : job->bj_Client->bcl_Port;

    job->bj_Msg.mn_Node.ln_Type = NT_MESSAGE;
    job->bj_Msg.mn_ReplyPort = port;
    job->bj_Msg.mn_Length = sizeof(struct BDMAJob);
    job->bj_Error = BDERR_OK;
    job->bj_Done = 0;
    job->bj_Active = 0;
    job->bj_Stop = 0;
    job->bj_Unit = 0;
    job->bj_Units = 0;
    job->bj_Taken = 0;
    job->bj_Waiter = NULL;

    /* a range that the job was checked against was removed since: the memory may be the firmware's again */
    if (job->bj_Stale)
    {
        job->bj_State = BJS_FAILED;
        job->bj_Error = BDERR_RANGE;
        BDMA_ReplyJob(BDMABase, job);
        return;
    }

    if (!(r->bdr_Given & BDRF_NOCACHE))
    {
        /* The data cache is not coherent with the DMA: write back what the CPU wrote and drop the lines before the transfer
           (the interrupt of the channel, or the software interrupt that replies, drops them again after it).
           The memory of the RTG board, and the ranges that the client says are not cached, are not cached by the CPU */
        if (!(r->bdr_Given & BDRF_FILL) && !job->bj_ReadUncached)
        {
            BDMA_CacheBefore(SysBase,job->bj_ReadLo, job->bj_ReadHi - job->bj_ReadLo, DMA_ReadFromRAM);
        }

        if (!job->bj_WriteUncached)
        {
            BDMA_CacheBefore(SysBase, job->bj_WriteLo, job->bj_WriteHi - job->bj_WriteLo, 0);
        }
    }

    ipl = BDMA_Lock();

    /* the caller does not want to wait: no idle channel, or a conflict with a job that runs or is queued */
    if ((r->bdr_Given & BDRF_NOWAIT) && BDMA_WouldWait(BDMABase, job))
    {
        BDMABase->bdb_Busy++;
        job->bj_State = BJS_FAILED;
        job->bj_Error = BDERR_BUSY;
        BDMA_Unlock(ipl);
        BDMA_ReplyJob(BDMABase, job);
        return;
    }

    job->bj_Sequence = ++BDMABase->bdb_Sequence;
    job->bj_State = BJS_QUEUED;
    AddTail((struct List *)&BDMABase->bdb_Queue, BDMA_JOBNODE(job));
    job->bj_InQueue = 1;
    BDMA_ENQUEUED(BDMABase, job);
    BDMA_Unlock(ipl);

    BDMA_Run(BDMABase);

    /* only a job that still waits is dated; the state is read again with the interrupts off, a job that went on a channel costs nothing here */
    if (job->bj_State == BJS_QUEUED)
    {
        ipl = BDMA_Lock();
        BDMA_Stamp(job);
        BDMA_Unlock(ipl);
    }
}
