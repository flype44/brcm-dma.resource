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

/*
    As AbortIO() does for a request: stops the job, in the queue or on its channel, and replies it to its port with BDERR_ABORTED. A job that is over is left
    alone. A job from BDMA_AddJobTagList() that was replied is taken off the port and freed here.
*/
VOID L_BDMA_AbortJob(REGARG(struct BDMAJob *job, "a0"), REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;

    if (job == NULL)
        return;

    BDMA_AbortInternal(BDMABase, job);

    if (job->bj_Auto && job->bj_Msg.mn_Node.ln_Type == NT_REPLYMSG)
    {
        struct MsgPort *port = job->bj_Msg.mn_ReplyPort;

        if (port != NULL && (port->mp_Flags & PF_ACTION) == PA_SIGNAL && !job->bj_Taken)
        {
            Disable();
            Remove(&job->bj_Msg.mn_Node);
            Enable();
        }

        BDMA_Release(BDMABase, job);
    }
}
