/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <exec/types.h>
#include <exec/nodes.h>
#include <common/compiler.h>

#include "brcm-dma.h"

/* As CheckIO() does for a request: TRUE when the job was replied to its port (or was never started), FALSE while it is in flight */
BOOL L_BDMA_CheckJob(REGARG(struct BDMAJob *job, "a0"), REGARG(struct BDMABase *BDMABase, "a6"))
{
    return job != NULL && (job->bj_State == BJS_IDLE || job->bj_Msg.mn_Node.ln_Type == NT_REPLYMSG);
}
