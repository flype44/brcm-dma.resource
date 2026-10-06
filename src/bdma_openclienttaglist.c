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
#include <exec/ports.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <utility/tagitem.h>
#include <common/compiler.h>

#include "brcm-dma.h"

/* A session: it names the program for the tools, keeps the default reply port
   and priority of its jobs, and gives them back when it is closed.
   Without BDC_ReplyPort the client makes a port of its own:
   it belongs to the task that opens the client, which is the only one that can
   sleep in BDMA_WaitJob() for the jobs that use it. A task context
   (utility.library is opened here, and the port needs a signal).
*/
struct BDMAClient * L_BDMA_OpenClientTagList(
    REGARG(const struct TagItem *tagList, "a0"), 
    REGARG(struct BDMABase *BDMABase, "a6"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct Library *UtilityBase = BDMA_OpenUtility(BDMABase);
    struct BDMAClient *client = NULL;
    CONST_STRPTR name = NULL;
    struct MsgPort *port = NULL;
    ULONG priority = BDPRI_NORMAL;
    ULONG *error_code = NULL;
    LONG error = BDERR_UNAVAILABLE;

    if (UtilityBase != NULL)
    {
        struct TagItem *t;
        const struct TagItem *list = tagList;

        while ((t = NextTagItem((struct TagItem **)&list)) != NULL)
        {
            switch (t->ti_Tag)
            {
                case BDC_Name:      name = (CONST_STRPTR)t->ti_Data; break;
                case BDC_ReplyPort: port = (struct MsgPort *)t->ti_Data; break;
                case BDC_Priority:  priority = t->ti_Data; break;
                case BDJ_ErrorCode: error_code = (ULONG *)t->ti_Data; break;
                default: break;
            }
        }

        if (priority > BDPRI_HIGH)
        {
            error = BDERR_ARGS;
        }
        else if ((client = AllocVec(sizeof(struct BDMAClient), MEMF_PUBLIC | MEMF_CLEAR)) == NULL)
        {
            error = BDERR_BUSY;
        }
        else
        {
            ULONG i;

            client->bcl_Base = BDMABase;
            client->bcl_Priority = priority;
            client->bcl_Jobs.mlh_Head = (struct MinNode *)&client->bcl_Jobs.mlh_Tail;
            client->bcl_Jobs.mlh_Tail = NULL;
            client->bcl_Jobs.mlh_TailPred = (struct MinNode *)&client->bcl_Jobs.mlh_Head;

            if (name != NULL)
            {
                for (i = 0; i < sizeof(client->bcl_Name) - 1 && name[i] != 0; i++)
                {
                    client->bcl_Name[i] = name[i];
                }
            }
            client->bcl_Node.ln_Name = client->bcl_Name;

            if (port == NULL)
            {
                port = CreateMsgPort();
                client->bcl_OwnPort = TRUE;
            }

            if (port == NULL)
            {
                FreeVec(client);
                client = NULL;
                error = BDERR_BUSY;
            }
            else
            {
                client->bcl_Port = port;

                ObtainSemaphore(&BDMABase->bdb_Lock);
                AddTail((struct List *)&BDMABase->bdb_Clients, &client->bcl_Node);
                ReleaseSemaphore(&BDMABase->bdb_Lock);

                error = BDERR_OK;
            }
        }
    }

    if (error_code != NULL)
    {
        *error_code = error;
    }

    return client;
}
