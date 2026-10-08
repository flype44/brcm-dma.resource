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

/* The data cache is not coherent with the DMA:
   write back what the CPU wrote and drop the lines before the transfer
   (the software interrupt that replies drops them again after it).
   A small area line by line, a big one by the call of the system (CachePreDMA() 
   gives back the length of the contiguous piece it handled: loop until the area is done).
*/
static VOID CachePre(
    struct ExecBase *SysBase, 
    ULONG address, 
    ULONG bytes, 
    ULONG flags)
{
    ULONG first = 1;

    if (bytes <= BDMA_LINEWISE_MAX)
    {
        BDMA_PushLines(address, bytes);
        return;
    }

    while (bytes)
    {
        LONG l = bytes;

        CachePreDMA((APTR)address, &l, flags | (first ? 0 : DMA_Continue));
        first = 0;

        if (l <= 0 || (ULONG)l > bytes)
        {
            break;
        }

        address += l;
        bytes -= l;
    }
}

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

    /* every channel busy and the caller does not want to wait */
    if (r->bdr_Given & BDRF_NOWAIT)
    {
        ULONG i;
        BOOL idle = FALSE;

        Disable();
        for (i = 0; i < BDMABase->bdb_Channels; i++)
        {
            if (BDMABase->bdb_Channel[i].bc_Job == NULL && BDMABase->bdb_Channel[i].bc_Registered)
            {
                idle = TRUE;
            }
        }
        Enable();

        if (!idle)
        {
            BDMABase->bdb_Busy++;
            job->bj_State = BJS_FAILED;
            job->bj_Error = BDERR_BUSY;
            BDMA_ReplyJob(BDMABase, job);
            return;
        }
    }

    if (!(r->bdr_Given & BDRF_NOCACHE))
    {
        /* the memory of the RTG board is not cached by the CPU */
        if (!(r->bdr_Given & BDRF_FILL) && !BDMA_InVideoMemory(BDMABase, job->bj_ReadLo, job->bj_ReadHi))
        {
            CachePre(SysBase, job->bj_ReadLo, job->bj_ReadHi - job->bj_ReadLo, DMA_ReadFromRAM);
        }

        if (!BDMA_InVideoMemory(BDMABase, job->bj_WriteLo, job->bj_WriteHi))
        {
            CachePre(SysBase, job->bj_WriteLo, job->bj_WriteHi - job->bj_WriteLo, 0);
        }
    }

    Disable();
    job->bj_Sequence = ++BDMABase->bdb_Sequence;
    job->bj_State = BJS_QUEUED;
    AddTail((struct List *)&BDMABase->bdb_Queue, BDMA_JOBNODE(job));
    job->bj_InQueue = 1;
    BDMA_ENQUEUED(BDMABase, job);
    Enable();

    BDMA_Run(BDMABase);
}
