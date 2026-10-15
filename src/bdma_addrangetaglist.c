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
#include <proto/utility.h>
#include <utility/tagitem.h>
#include <common/compiler.h>

#include "brcm-dma.h"

#define RANGE_ALIGN     0x1000UL        /* a page: the memory of the firmware is given and taken by pages */
#define CONSTANT_BYTES  256UL           /* what a fill reads (bc_Constant) */

static BOOL Meets(
    ULONG lo1,
    ULONG hi1,
    ULONG lo2,
    ULONG hi2)
{
    return lo1 < hi2 && lo2 < hi1;
}

/* Does [lo, hi) meet the memory that the resource keeps for itself: the control blocks, the buffer of the moves and the constant of the fills of its channels? */
static BOOL MeetsEngine(
    const struct BDMABase *BDMABase,
    ULONG lo,
    ULONG hi)
{
    ULONG i;

    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        const struct BDMAChannel *c = &BDMABase->bdb_Channel[i];

        if ((c->bc_Chain != NULL && Meets(lo, hi, (ULONG)c->bc_Chain, (ULONG)c->bc_Chain + BDMA_CHAIN_SIZE)) ||
            (c->bc_Bounce != NULL && Meets(lo, hi, (ULONG)c->bc_Bounce, (ULONG)c->bc_Bounce + BDMA_BOUNCE_SIZE)) ||
            (c->bc_Constant != NULL && Meets(lo, hi, (ULONG)c->bc_Constant, (ULONG)c->bc_Constant + CONSTANT_BYTES)))
        {
            return TRUE;
        }
    }

    return FALSE;
}

/* Does [lo, hi) meet a range of any client? The caller has the semaphore of the base. */
static BOOL MeetsRanges(
    const struct BDMABase *BDMABase,
    ULONG lo,
    ULONG hi)
{
    const struct Node *cn;

    for (cn = (const struct Node *)BDMABase->bdb_Clients.mlh_Head; cn->ln_Succ != NULL; cn = cn->ln_Succ)
    {
        const struct BDMAClient *client = (const struct BDMAClient *)cn;
        const struct MinNode *node;

        for (node = client->bcl_Ranges.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        {
            const struct BDMARange *range = (const struct BDMARange *)node;

            if (Meets(lo, hi, range->brg_Lo, range->brg_Hi))
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* Declares a range of memory for the jobs of the client. The memory must be in the window of the GPU (what the device tree leaves out of /memory, below
   1 GB: where the firmware puts its framebuffers), page aligned, clear of the RTG memory, of the ranges of the other clients and of the memory of the
   engine itself. Nothing else is opened: the RAM of exec and the RTG memory are accepted without it. A task context.
*/
struct BDMARange * L_BDMA_AddRangeTagList(
    REGARG(struct BDMAClient *client, "a0"),
    REGARG(ULONG address, "d0"),
    REGARG(ULONG size, "d1"),
    REGARG(const struct TagItem *tagList, "a1"),
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct Library *UtilityBase = BDMA_OpenUtility(BDMABase);
    struct BDMARange *range = NULL;
    ULONG *error_code = NULL;
    LONG error = BDERR_UNAVAILABLE;

    if (UtilityBase != NULL)
    {
        struct TagItem *t;
        const struct TagItem *list = tagList;
        CONST_STRPTR name = NULL;
        ULONG flags = 0, unknown = 0, hi = address + size;
        BOOL strict = FALSE;

        while ((t = NextTagItem((struct TagItem **)&list)) != NULL)
        {
            switch (t->ti_Tag)
            {
                case BDR_Cached:        if (t->ti_Data) flags |= BRF_CACHED; break;
                case BDR_ReadOnly:      if (t->ti_Data) flags |= BRF_READONLY; break;
                case BDR_WriteOnly:     if (t->ti_Data) flags |= BRF_WRITEONLY; break;
                case BDR_Name:          name = (CONST_STRPTR)t->ti_Data; break;
                case BDJ_Strict:        strict = t->ti_Data != 0; break;
                case BDJ_ErrorCode:     error_code = (ULONG *)t->ti_Data; break;
                default:                unknown++; break;
            }
        }

        if (client == NULL || size == 0 || hi <= address || ((address | size) & (RANGE_ALIGN - 1)) ||
            (flags & BRF_READONLY && flags & BRF_WRITEONLY) || (strict && unknown))
        {
            error = BDERR_ARGS;
        }
        else
        {
            BOOL available;

            ObtainSemaphore(&BDMABase->bdb_Lock);
            available = BDMA_StartEngine(BDMABase);
            ReleaseSemaphore(&BDMABase->bdb_Lock);

            error = available ? BDERR_OK : BDERR_UNAVAILABLE;
        }

        if (error == BDERR_OK && (BDMABase->bdb_GpuHi == 0 || address < BDMABase->bdb_GpuLo || hi > BDMABase->bdb_GpuHi))
        {
            error = BDERR_RANGE;
        }

        if (error == BDERR_OK && (Meets(address, hi, BDMABase->bdb_VCBase, BDMABase->bdb_VCBase + BDMABase->bdb_VCSize) ||
                                  MeetsEngine(BDMABase, address, hi)))
        {
            error = BDERR_RANGE;
        }

        if (error == BDERR_OK)
        {
            range = AllocVec(sizeof(struct BDMARange), MEMF_PUBLIC | MEMF_CLEAR);
            if (range == NULL)
            {
                error = BDERR_BUSY;
            }
        }

        if (error == BDERR_OK)
        {
            ObtainSemaphore(&BDMABase->bdb_Lock);

            if (MeetsRanges(BDMABase, address, hi))
            {
                error = BDERR_OVERLAP;
            }
            else
            {
                ULONG i;

                range->brg_Client = client;
                range->brg_Lo = address;
                range->brg_Hi = hi;
                range->brg_Flags = flags;

                if (name != NULL)
                {
                    for (i = 0; i < sizeof(range->brg_Name) - 1 && name[i] != 0; i++)
                    {
                        range->brg_Name[i] = name[i];
                    }
                }

                AddTail((struct List *)&client->bcl_Ranges, (struct Node *)&range->brg_Node);
            }

            ReleaseSemaphore(&BDMABase->bdb_Lock);

            if (error != BDERR_OK)
            {
                FreeVec(range);
                range = NULL;
            }
        }
    }

    if (error_code != NULL)
    {
        *error_code = error;
    }

    return range;
}
