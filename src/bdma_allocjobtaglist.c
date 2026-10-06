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
#include <utility/tagitem.h>
#include <common/compiler.h>

#include "brcm-dma.h"

/* Describes a job: the tags are parsed and checked once, here, in a task context.
   The job belongs to the client until BDMA_FreeJob(); BDMA_StartJob() queues it,
   as many times as wanted (BDMA_SetJobTagList() changes it between two).
   The engine starts here, at the first job (never in Init()).
*/
struct BDMAJob * L_BDMA_AllocJobTagList(
    REGARG(struct BDMAClient *client, "a0"), 
    REGARG(const struct TagItem *tagList, "a1"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct Library *UtilityBase = BDMA_OpenUtility(BDMABase);
    struct BDMARequest request;
    struct BDMAJob *job = NULL;
    LONG error = BDERR_UNAVAILABLE;

    request.bdr_ErrorCode = NULL;

    if (client != NULL && UtilityBase != NULL)
    {
        error = BDMA_ParseTags(UtilityBase, tagList, &request, FALSE, client->bcl_Priority);

        if (error == BDERR_OK)
        {
            BOOL available;

            ObtainSemaphore(&BDMABase->bdb_Lock);
            available = BDMA_StartEngine(BDMABase);
            ReleaseSemaphore(&BDMABase->bdb_Lock);

            error = available ? BDERR_OK : BDERR_UNAVAILABLE;
        }

        if (error == BDERR_OK)
        {
            job = AllocVec(sizeof(struct BDMAJob), MEMF_PUBLIC | MEMF_CLEAR);

            if (job == NULL)
            {
                error = BDERR_BUSY;
            }
            else
            {
                job->bj_Request = request;
                job->bj_Channel = -1;
                job->bj_State = BJS_IDLE;
                job->bj_Client = client;
                error = BDMA_CheckRequest(BDMABase, job);

                if (error != BDERR_OK)
                {
                    FreeVec(job);
                    job = NULL;
                }
                else
                {
                    Disable();
                    AddTail((struct List *)&client->bcl_Jobs, (struct Node *)&job->bj_Link);
                    Enable();
                }
            }
        }
    }

    if (request.bdr_ErrorCode != NULL)
    {
        *request.bdr_ErrorCode = error;
    }

    return job;
}
