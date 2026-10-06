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
#include "hw-vc4.h"

#define BDMA_LINKJOB(node) ((struct BDMAJob *)((UBYTE *)(node) - __builtin_offsetof(struct BDMAJob, bj_Link)))

/* Ends the session: what is in flight is aborted, the jobs of the client are
   freed, and so is its port when the client made it. A job that is over but
   whose reply is still to come (the software interrupt has it) is waited for,
   a few milliseconds at most. The replies that the client has not taken from
   a port of its own are the client's to take before it closes.
*/
VOID L_BDMA_CloseClient(
    REGARG(struct BDMAClient *client, "a0"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct MinNode *node;
    ULONG t0;
    BOOL pending;

    if (client == NULL)
    {
        return;
    }

    for (node = client->bcl_Jobs.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        BDMA_AbortInternal(BDMABase, BDMA_LINKJOB(node));
    }

    t0 = timer_now();
    do
    {
        pending = FALSE;

        for (node = client->bcl_Jobs.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        {
            struct BDMAJob *job = BDMA_LINKJOB(node);

            if (job->bj_State != BJS_IDLE && job->bj_Msg.mn_Node.ln_Type == NT_MESSAGE)
            {
                pending = TRUE;
            }
        }
    }
    while (pending && timer_now() - t0 < 50000);

    /* the replies that wait on a port that the client made go first:
       they are inside the jobs, which are about to be freed */
    if (client->bcl_OwnPort)
    {
        while (GetMsg(client->bcl_Port) != NULL);
        DeleteMsgPort(client->bcl_Port);
    }

    while ((node = (struct MinNode *)RemHead((struct List *)&client->bcl_Jobs)) != NULL)
    {
        FreeVec(BDMA_LINKJOB(node));
    }

    ObtainSemaphore(&BDMABase->bdb_Lock);
    Remove(&client->bcl_Node);
    ReleaseSemaphore(&BDMABase->bdb_Lock);

    FreeVec(client);
}
