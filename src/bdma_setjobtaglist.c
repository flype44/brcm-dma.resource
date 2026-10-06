/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <exec/types.h>
#include <utility/tagitem.h>
#include <common/compiler.h>

#include "brcm-dma.h"

/* Changes a job that is not in flight: the tags name what changes (an address, 
   the length, the rows, a pitch, a priority), what they leave out stays.
   The kind of the job does not change (a copy stays a copy, a fill a fill).
   The result is BDERR_OK, or the reason, and then the job is as it was.
*/
LONG L_BDMA_SetJobTagList(
    REGARG(struct BDMAJob *job, "a0"), 
    REGARG(const struct TagItem *tagList, "a1"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct Library *UtilityBase = BDMA_OpenUtility(BDMABase);
    struct BDMARequest saved;
    ULONG read_lo, read_hi, write_lo, write_hi;
    UBYTE reverse;
    ULONG *error_code;
    LONG error;

    if (job == NULL || UtilityBase == NULL)
    {
        return BDERR_ARGS;
    }

    /* in flight: the resource is reading it */
    if (job->bj_State < BJS_DONE)
    {
        return BDERR_BUSY;
    }

    saved    = job->bj_Request;
    read_lo  = job->bj_ReadLo;
    read_hi  = job->bj_ReadHi;
    write_lo = job->bj_WriteLo;
    write_hi = job->bj_WriteHi;
    reverse  = job->bj_Reverse;

    error = BDMA_ParseTags(UtilityBase, tagList, &job->bj_Request, TRUE, job->bj_Client->bcl_Priority);
    error_code = job->bj_Request.bdr_ErrorCode;

    if (error == BDERR_OK)
    {
        error = BDMA_CheckRequest(BDMABase, job);
    }

    if (error != BDERR_OK)
    {
        job->bj_Request = saved;
        job->bj_ReadLo = read_lo;
        job->bj_ReadHi = read_hi;
        job->bj_WriteLo = write_lo;
        job->bj_WriteHi = write_hi;
        job->bj_Reverse = reverse;
    }

    if (error_code != NULL)
    {
        *error_code = error;
    }

    return error;
}
