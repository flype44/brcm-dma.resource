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
#include <common/compiler.h>

#include "brcm-dma.h"

/* Sets the statistics back to zero: everything that measures (jobs, bytes, failures, aborts, timeouts, refusals, slices,
   deferred jobs, the longest wait, the busy time of the channels, the sizes), and the largest queue, which starts again at
   what is waiting now. What is in flight (the queue, the jobs on the channels) is not touched. The counters are shared
   by every client: whoever resets them resets them for all.
*/
VOID L_BDMA_ResetStatistics(
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    ULONG i;

    Disable();

    BDMABase->bdb_Jobs = 0;
    BDMABase->bdb_MegaBytes = 0;
    BDMABase->bdb_ByteRemainder = 0;
    BDMABase->bdb_Failures = 0;
    BDMABase->bdb_QueueMax = BDMABase->bdb_Waiting;
    BDMABase->bdb_WaitMax = 0;
    BDMABase->bdb_Aborts = 0;
    BDMABase->bdb_Timeouts = 0;
    BDMABase->bdb_Busy = 0;
    BDMABase->bdb_Slices = 0;
    BDMABase->bdb_Deferred = 0;

    for (i = 0; i < BDMA_SIZE_BUCKETS; i++)
    {
        BDMABase->bdb_Size[i] = 0;
    }

    for (i = 0; i < BDMA_MAX_CHANNELS; i++)
    {
        BDMABase->bdb_Channel[i].bc_BusyUs = 0;
        BDMABase->bdb_Channel[i].bc_BusyMs = 0;
    }

    Enable();
}
