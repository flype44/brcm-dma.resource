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

#define BDMA_LINKJOB(node) ((struct BDMAJob *)((UBYTE *)(node) - __builtin_offsetof(struct BDMAJob, bj_Link)))

/* Takes a range back. BDERR_BUSY if a job of the client that is in flight is on it (wait for the job or abort it first). The jobs of the client
   that were checked against it and are not started are marked: BDMA_StartJob() refuses them (BDERR_RANGE) until BDMA_SetJobTagList() checks them again.
   The task that removes a range is the one that starts the jobs of the client: nothing starts a job on it during the call.
*/
LONG L_BDMA_RemRange(
    REGARG(struct BDMARange *range, "a0"),
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct BDMAClient *client;
    struct MinNode *node;

    if (range == NULL)
    {
        return BDERR_ARGS;
    }

    client = range->brg_Client;

    if (BDMA_RangeInUse(client, range))
    {
        return BDERR_BUSY;
    }

    for (node = client->bcl_Jobs.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct BDMAJob *job = BDMA_LINKJOB(node);

        if ((job->bj_ReadLo < range->brg_Hi && range->brg_Lo < job->bj_ReadHi) ||
            (job->bj_WriteLo < range->brg_Hi && range->brg_Lo < job->bj_WriteHi))
        {
            job->bj_Stale = 1;
        }
    }

    ObtainSemaphore(&BDMABase->bdb_Lock);
    Remove((struct Node *)&range->brg_Node);
    ReleaseSemaphore(&BDMABase->bdb_Lock);

    FreeVec(range);

    return BDERR_OK;
}
