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

/* Takes the job out of the list of its client and frees it.
   For the calls that know that it is over and that its reply was taken.
*/
VOID BDMA_Release(
    struct BDMABase *BDMABase, 
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;

    Disable();
    Remove((struct Node *)&job->bj_Link);
    Enable();

    FreeVec(job);
}

/* Gives the job back. A job in flight, or over whose reply has not come yet,
   is left alone: wait for it (BDMA_WaitJob()) or abort it first.
*/
VOID L_BDMA_FreeJob(
    REGARG(struct BDMAJob *job, "a0"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    if (job == NULL)
    {
        return;
    }

    if (job->bj_State != BJS_IDLE && (job->bj_State < BJS_DONE || job->bj_Msg.mn_Node.ln_Type == NT_MESSAGE))
    {
        return;
    }

    BDMA_Release(BDMABase, job);
}
