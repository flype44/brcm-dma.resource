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
#include <proto/expansion.h>

#include <libraries/configregs.h>
#include <libraries/configvars.h>

#include <common/compiler.h>

#include "brcm-dma.h"

extern UBYTE rom_end;
extern const char deviceName[];
extern const char deviceIdString[];

APTR Init(REGARG(struct ExecBase *SysBase, "a6"))
{
    struct BDMABase *BDMABase = NULL;
    struct ExpansionBase *ExpansionBase = NULL;
    struct CurrentBinding binding;

    APTR base_pointer = NULL;

    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    GetCurrentBinding(&binding, sizeof(binding));

    base_pointer = AllocMem(
        BASE_NEG_SIZE + BASE_POS_SIZE, 
        MEMF_PUBLIC | MEMF_CLEAR);

    bug("[brcm-dma] Init\n");

    if (base_pointer)
    {
        ULONG relFuncTable[NUMBER_OF_FUNCTIONS + 1];

        relFuncTable[0] = (ULONG)&L_BDMA_AbortJob;
        relFuncTable[1] = (ULONG)&L_BDMA_AddJobTagList;
        relFuncTable[2] = (ULONG)&L_BDMA_AllocJobTagList;
        relFuncTable[3] = (ULONG)&L_BDMA_CheckJob;
        relFuncTable[4] = (ULONG)&L_BDMA_CloseClient;
        relFuncTable[5] = (ULONG)&L_BDMA_FreeJob;
        relFuncTable[6] = (ULONG)&L_BDMA_OpenClientTagList;
        relFuncTable[7] = (ULONG)&L_BDMA_QueryInfoTagList;
        relFuncTable[8] = (ULONG)&L_BDMA_QueryJobTagList;
        relFuncTable[9] = (ULONG)&L_BDMA_SetJobTagList;
        relFuncTable[10] = (ULONG)&L_BDMA_StartJob;
        relFuncTable[11] = (ULONG)&L_BDMA_WaitJob;
        relFuncTable[12] = (ULONG)&L_BDMA_ResetStatistics;
        relFuncTable[NUMBER_OF_FUNCTIONS] = (ULONG)-1;

        BDMABase = (struct BDMABase *)((UBYTE *)base_pointer + BASE_NEG_SIZE);
        
        MakeFunctions(BDMABase, relFuncTable, 0);

        BDMABase->bdb_Node.lib_Node.ln_Type = NT_RESOURCE;
        BDMABase->bdb_Node.lib_Node.ln_Pri = BDMA_PRIORITY;
        BDMABase->bdb_Node.lib_Node.ln_Name = (STRPTR)deviceName;

        BDMABase->bdb_Node.lib_NegSize = BASE_NEG_SIZE;
        BDMABase->bdb_Node.lib_PosSize = BASE_POS_SIZE;
        BDMABase->bdb_Node.lib_Version = BDMA_VERSION;
        BDMABase->bdb_Node.lib_Revision = BDMA_REVISION;
        BDMABase->bdb_Node.lib_IdString = (STRPTR)deviceIdString;

        BDMABase->bdb_ExecBase = SysBase;

        InitSemaphore(&BDMABase->bdb_Lock);

        /* NewList() */
        BDMABase->bdb_Clients.mlh_Head = (struct MinNode *)&BDMABase->bdb_Clients.mlh_Tail;
        BDMABase->bdb_Clients.mlh_Tail = NULL;
        BDMABase->bdb_Clients.mlh_TailPred = (struct MinNode *)&BDMABase->bdb_Clients.mlh_Head;
        BDMABase->bdb_Done.mlh_Head = (struct MinNode *)&BDMABase->bdb_Done.mlh_Tail;
        BDMABase->bdb_Done.mlh_Tail = NULL;
        BDMABase->bdb_Done.mlh_TailPred = (struct MinNode *)&BDMABase->bdb_Done.mlh_Head;

        /* the software interrupt that does the cache and the reply of a job that is over */
        BDMABase->bdb_DoneInt.is_Node.ln_Type = NT_INTERRUPT;
        BDMABase->bdb_DoneInt.is_Node.ln_Pri = 0;
        BDMABase->bdb_DoneInt.is_Node.ln_Name = (char *)"brcm-dma.done";
        BDMABase->bdb_DoneInt.is_Data = BDMABase;
        BDMABase->bdb_DoneInt.is_Code = (void (*)())BDMA_DoneCode;

        if (OpenResource("devicetree.resource") != NULL)
        {
            SumLibrary((struct Library*)BDMABase);
            AddResource(BDMABase);

            if (binding.cb_ConfigDev != NULL)
            {
                binding.cb_ConfigDev->cd_Flags &= ~CDF_CONFIGME;
                binding.cb_ConfigDev->cd_Driver = BDMABase;
            }
        }
        else
        {
            FreeMem(base_pointer, BASE_NEG_SIZE + BASE_POS_SIZE);
            BDMABase = NULL;
        }
    }

    CloseLibrary((struct Library*)ExpansionBase);
    return BDMABase;
}

static void putch(REGARG(UBYTE data, "d0"), REGARG(APTR ignore, "a3"))
{
    (void)ignore;
    *(UBYTE*)0xdeadbeef = data;
}

void kprintf(REGARG(const char * msg, "a0"), REGARG(void * args, "a1"))
{
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
    RawDoFmt(msg, args, (APTR)putch, NULL);
}
