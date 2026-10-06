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

/*
    utility.library is in the ROM of the Amiga, but its RomTag (priority 103 on the machine of the author, V47.3) comes AFTER ours (117):
    it is not there when Init() runs. It is opened here, the first time a call needs it, by the task that calls (never from an interrupt:
    only BDMA_CheckJob() may be called from one, and it needs no tags). The resource lives until the next reset: it is never closed.
*/
struct Library *BDMA_OpenUtility(struct BDMABase *BDMABase)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;

    if (BDMABase->bdb_UtilityBase == NULL)
    {
        ObtainSemaphore(&BDMABase->bdb_Lock);

        if (BDMABase->bdb_UtilityBase == NULL)
            BDMABase->bdb_UtilityBase = OpenLibrary("utility.library", 36);

        ReleaseSemaphore(&BDMABase->bdb_Lock);
    }

    return BDMABase->bdb_UtilityBase;
}
