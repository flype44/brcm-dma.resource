/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <common/compiler.h>

#include "brcm-dma.h"

/* As WaitIO() does for a request:
   sleeps until the job is replied, and gives the result, BDERR_OK or the reason.
   The task is asleep, not looping. Any task may wait for any job: the owner of a 
   signal port sleeps on the signal of the port, any other task on a signal of its 
   own that the reply wakes. A signal port holds the reply until it is taken:
   it is taken here (use this call or GetMsg() on such a port, not both);
   a port that wakes an interrupt has its reply taken in that interrupt.
   A second call gives the same result again. A job from BDMA_AddJobTagList() is freed here.
*/
LONG L_BDMA_WaitJob(
    REGARG(struct BDMAJob *job, "a0"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    LONG error;

    if (job == NULL || job->bj_State == BJS_IDLE)
    {
        return BDERR_ARGS;
    }

    if (!job->bj_Taken)
    {
        struct MsgPort *port = job->bj_Msg.mn_ReplyPort;
        BOOL signalport = port != NULL && (port->mp_Flags & PF_ACTION) == PA_SIGNAL;

        if (job->bj_Msg.mn_Node.ln_Type != NT_REPLYMSG)
        {
            if (signalport && port->mp_SigTask == FindTask(NULL))
            {
                while (job->bj_Msg.mn_Node.ln_Type != NT_REPLYMSG)
                {
                    Wait(1UL << port->mp_SigBit);
                }
            }
            else
            {
                BYTE bit = AllocSignal(-1);

                if (bit < 0)
                {
                    return BDERR_BUSY;
                }

                Disable();
                if (job->bj_Msg.mn_Node.ln_Type != NT_REPLYMSG)
                {
                    SetSignal(0, 1UL << bit);
                    job->bj_WaitMask = 1UL << bit;
                    job->bj_Waiter = FindTask(NULL);
                }
                Enable();

                while (job->bj_Msg.mn_Node.ln_Type != NT_REPLYMSG)
                {
                    Wait(1UL << bit);
                }

                job->bj_Waiter = NULL;
                SetSignal(0, 1UL << bit);
                FreeSignal(bit);
            }
        }

        if (signalport)
        {
            Disable();
            Remove(&job->bj_Msg.mn_Node);
            Enable();
        }

        job->bj_Taken = TRUE;
    }

    error = job->bj_Error;

    if (job->bj_Auto)
    {
        BDMA_Release(BDMABase, job);
    }

    return error;
}
