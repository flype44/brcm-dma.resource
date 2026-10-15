/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <common/compiler.h>

#include "brcm-dma.h"

/* The shortcut: BDMA_AllocJobTagList() and BDMA_StartJob() in one call, for a 
   job that is used once. The job is freed by the BDMA_WaitJob() or the BDMA_AbortJob() 
   that ends it. A job that BDJ_NoWait refused is replied at once with BDERR_BUSY: 
   BDMA_WaitJob() gives it. With BDJ_Detached nobody waits for it: BDMA_WaitClient() frees it
   and gives the first error.
*/
struct BDMAJob * L_BDMA_AddJobTagList(
    REGARG(struct BDMAClient *client, "a0"), 
    REGARG(const struct TagItem *tagList, "a1"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct BDMAJob *job;

    /* the jobs that were let go and are over: freed here, in a task context, so that a client that never calls BDMA_WaitClient() does not pile them up */
    if (client != NULL && client->bcl_Detached >= BDMA_REAP_AT)
    {
        BDMA_ReapDetached(BDMABase, client);
    }

    job = L_BDMA_AllocJobTagList(client, tagList, BDMABase);

    if (job != NULL)
    {
        job->bj_Auto = 1;

        if (job->bj_Request.bdr_Given & BDRF_DETACHED)
        {
            job->bj_Detached = 1;
            client->bcl_Detached++;
        }

        L_BDMA_StartJob(job, BDMABase);
    }

    return job;
}
