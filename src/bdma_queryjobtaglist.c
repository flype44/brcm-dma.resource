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
#include <proto/utility.h>
#include <utility/tagitem.h>
#include <common/compiler.h>

#include "brcm-dma.h"

/* What a job can say about itself, as GetDTAttrs() does:
   each tag names a value, the ULONG * that goes with it receives it;
   the number of answers is returned. A tag that this version does not 
   know is skipped and its place left alone.
*/
ULONG L_BDMA_QueryJobTagList(
    REGARG(struct BDMAJob *job, "a0"), 
    REGARG(const struct TagItem *tagList, "a1"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct Library *UtilityBase = BDMA_OpenUtility(BDMABase);
    struct TagItem *t;
    const struct TagItem *list = tagList;
    ULONG answered = 0;

    if (job == NULL || UtilityBase == NULL)
    {
        return 0;
    }

    while ((t = NextTagItem((struct TagItem **)&list)) != NULL)
    {
        ULONG *to = (ULONG *)t->ti_Data;
        ULONG value;

        switch (t->ti_Tag)
        {
            case BDJI_State:     value = job->bj_State; break;
            case BDJI_Error:     value = job->bj_State < BJS_DONE ? BDERR_OK : (ULONG)job->bj_Error; break;
            case BDJI_BytesDone: value = job->bj_Done; break;
            default: continue;
        }

        if (to != NULL)
        {
            *to = value;
            answered++;
        }
    }

    return answered;
}
