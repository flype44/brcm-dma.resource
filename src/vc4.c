/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/


/*
    The backend of the BCM2835 family (Zero, 2, 3): the normal and lite channels and the legacy interrupt controller.
    NOT WRITTEN. It exists so that the engine has a second backend to choose and a precise answer for these machines:
    the model is recognised, the engine is unavailable with the reason BDW_NOTIMPLEMENTED, and the clients stay on the CPU.
*/

#include <exec/types.h>

#include "brcm-dma.h"

static BOOL Start(
    struct BDMABase *BDMABase)
{
    BDMABase->bdb_Reason = BDW_NOTIMPLEMENTED;

    return FALSE;
}

/* Never called: the engine does not run, so no job reaches a channel */
static VOID Build(
    struct BDMABase *BDMABase,
    struct BDMAChannel *channel,
    struct BDMAJob *job)
{
}

static VOID Arm(
    struct BDMAChannel *channel)
{
}

static VOID StopChannel(
    struct BDMAChannel *channel)
{
}

const struct BDMABackend BDMA_VC4Backend =
{
    Start,
    Build,
    Arm,
    StopChannel
};
