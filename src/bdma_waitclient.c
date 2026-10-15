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

#define BDMA_LINKJOB(node) ((struct BDMAJob *)((UBYTE *)(node) - __builtin_offsetof(struct BDMAJob, bj_Link)))

/* In flight: started and not replied yet. A job that is described and not started is not. */
#define BDMA_INFLIGHT(job) \
    ((job)->bj_State != BJS_IDLE && ((job)->bj_State < BJS_DONE || (job)->bj_Msg.mn_Node.ln_Type == NT_MESSAGE))

/* Frees the detached jobs of the client that are over (their reply taken from the port that holds it), and keeps the first error
   of them for BDMA_WaitClient(). The jobs that are still in flight stay. A task context.
*/
VOID BDMA_ReapDetached(
    struct BDMABase *BDMABase,
    struct BDMAClient *client)
{
    struct MinNode *node, *next;

    for (node = client->bcl_Jobs.mlh_Head; (next = node->mln_Succ) != NULL; node = next)
    {
        struct BDMAJob *job = BDMA_LINKJOB(node);

        if (job->bj_Detached && job->bj_State != BJS_IDLE && !BDMA_INFLIGHT(job))
        {
            BDMA_TakeReply(BDMABase, job);

            if (job->bj_Error != BDERR_OK && client->bcl_Error == BDERR_OK)
            {
                client->bcl_Error = job->bj_Error;
            }

            BDMA_Release(BDMABase, job);
        }
    }
}

/* Sleeps until none of the jobs of the client is in flight, then frees the detached ones and gives the first error among them since
   the last call (BDERR_OK if none). The jobs that the client holds itself (BDMA_AllocJobTagList(), BDMA_AddJobTagList() without
   BDJ_Detached) are waited for and left alone: their replies, results and freeing are still the client's, by BDMA_WaitJob().
   For the task that owns the client: another task freeing its jobs meanwhile is not looked after.
*/
LONG L_BDMA_WaitClient(
    REGARG(struct BDMAClient *client, "a0"),
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    LONG error;

    if (client == NULL)
    {
        return BDERR_ARGS;
    }

    for (;;)
    {
        struct BDMAJob *flying = NULL;
        struct MinNode *node;
        ULONG ipl;

        ipl = BDMA_Lock();
        for (node = client->bcl_Jobs.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        {
            if (BDMA_INFLIGHT(BDMA_LINKJOB(node)))
            {
                flying = BDMA_LINKJOB(node);
                break;
            }
        }
        BDMA_Unlock(ipl);

        if (flying == NULL)
        {
            break;
        }

        error = BDMA_WaitReply(BDMABase, flying);

        if (error != BDERR_OK)
        {
            BDMA_ReapDetached(BDMABase, client);

            return error;
        }
    }

    BDMA_ReapDetached(BDMABase, client);

    error = client->bcl_Error;
    client->bcl_Error = BDERR_OK;

    return error;
}
