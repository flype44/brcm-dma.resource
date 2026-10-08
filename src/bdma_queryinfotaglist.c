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

ULONG L_BDMA_QueryInfoTagList(
    REGARG(const struct TagItem *tagList, "a0"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct Library *UtilityBase = BDMA_OpenUtility(BDMABase);
    struct BDMAStatus status;
    BOOL available;
    ULONG i;

    if (UtilityBase == NULL)
    {
        return 0;
    }

    /* the engine starts here, at the first call that needs to know (never in Init()) */
    ObtainSemaphore(&BDMABase->bdb_Lock);
    available = BDMA_StartEngine(BDMABase);
    ReleaseSemaphore(&BDMABase->bdb_Lock);

    status.bds_Available = available;
    status.bds_Version   = (BDMABase->bdb_Node.lib_Version << 16) | BDMABase->bdb_Node.lib_Revision;
    status.bds_Interrupt = available;
    status.bds_Model     = BDMABase->bdb_Model;
    status.bds_Features  = available ? BDMABase->bdb_Features : 0;
    status.bds_Channels  = 0;
    status.bds_MaxRows   = available ? BDMA_MAX_ROWS : 0;
    status.bds_Classes   = available ? BDMABase->bdb_Classes : 0;
    status.bds_VideoBase = BDMABase->bdb_VCBase;
    status.bds_VideoSize = BDMABase->bdb_VCSize;
    status.bds_Jobs      = BDMABase->bdb_Jobs;
    status.bds_MegaBytes = BDMABase->bdb_MegaBytes;
    status.bds_Failures  = BDMABase->bdb_Failures;
    status.bds_QueueMax  = BDMABase->bdb_QueueMax;
    status.bds_WaitMaxUs = BDMABase->bdb_WaitMax;
    status.bds_Aborts    = BDMABase->bdb_Aborts;
    status.bds_Timeouts  = BDMABase->bdb_Timeouts;
    status.bds_Busy      = BDMABase->bdb_Busy;
    status.bds_Slices    = BDMABase->bdb_Slices;
    status.bds_Deferred  = BDMABase->bdb_Deferred;
    status.bds_IrqCalls  = BDMABase->bdb_IrqCalls;
    status.bds_IrqMCycles = BDMABase->bdb_IrqMCycles;
    status.bds_IrqKInstr = BDMABase->bdb_IrqKInstr;

    for (i = 0; i < BDMA_MAX_CHANNELS; i++)
    {
        status.bds_BusyMs[i] = BDMABase->bdb_Channel[i].bc_BusyMs;
    }

    for (i = 0; i < BDMA_SIZE_BUCKETS; i++)
    {
        status.bds_Size[i] = BDMABase->bdb_Size[i];
    }

    if (available)
    {
        for (i = 0; i < BDMABase->bdb_Channels; i++)
        {
            if (BDMABase->bdb_Channel[i].bc_Registered)
            {
                status.bds_Channels |= 1UL << BDMABase->bdb_Channel[i].bc_Number;
            }
        }
    }

    return BDMA_FillQuery(UtilityBase, tagList, &status);
}
