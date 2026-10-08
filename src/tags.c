/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <exec/types.h>
#include <utility/tagitem.h>
#include <proto/utility.h>
#include <common/compiler.h>

#include "brcm-dma.h"

LONG BDMA_ParseTags(
    struct Library *UtilityBase, 
    const struct TagItem *tags, 
    struct BDMARequest *r, 
    BOOL keep, 
    ULONG defprio)
{
    struct TagItem *t;
    const struct TagItem *list = tags;
    ULONG *error_code = NULL;

    if (!keep)
    {
        r->bdr_Given     = 0;
        r->bdr_Src       = 0;
        r->bdr_Dst       = 0;
        r->bdr_Fill      = 0;
        r->bdr_Length    = 0;
        r->bdr_Rows      = 1;
        r->bdr_SrcPitch  = 0;
        r->bdr_DstPitch  = 0;
        r->bdr_Priority  = defprio;
        r->bdr_ReplyPort = NULL;
        r->bdr_Classes   = BDCLASS_ALL;
        r->bdr_Timeout   = BDMA_DEFAULT_TIMEOUT;
    }
    r->bdr_ErrorCode = NULL;
    r->bdr_Unknown = 0;

    while ((t = NextTagItem((struct TagItem **)&list)) != NULL)
    {
        switch (t->ti_Tag)
        {
            case BDJ_Src:           r->bdr_Src = t->ti_Data;         r->bdr_Given |= BDRF_SRC; break;
            case BDJ_Dst:           r->bdr_Dst = t->ti_Data;         r->bdr_Given |= BDRF_DST; break;
            case BDJ_FillValue:     r->bdr_Fill = t->ti_Data;        r->bdr_Given |= BDRF_FILL; break;
            case BDJ_Length:        r->bdr_Length = t->ti_Data;      r->bdr_Given |= BDRF_LENGTH; break;
            case BDJ_Rows:          r->bdr_Rows = t->ti_Data;        r->bdr_Given |= BDRF_ROWS; break;
            case BDJ_SrcPitch:      r->bdr_SrcPitch = t->ti_Data;    r->bdr_Given |= BDRF_SRCPITCH; break;
            case BDJ_DstPitch:      r->bdr_DstPitch = t->ti_Data;    r->bdr_Given |= BDRF_DSTPITCH; break;
            case BDJ_Move:          if (t->ti_Data) r->bdr_Given |= BDRF_MOVE; break;
            case BDJ_NoCache:       if (t->ti_Data) r->bdr_Given |= BDRF_NOCACHE; break;
            case BDJ_NoWait:        if (t->ti_Data) r->bdr_Given |= BDRF_NOWAIT; break;
            case BDJ_Strict:        if (t->ti_Data) r->bdr_Given |= BDRF_STRICT; break;
            case BDJ_Priority:      r->bdr_Priority = t->ti_Data; break;
            case BDJ_ReplyPort:     r->bdr_ReplyPort = (struct MsgPort *)t->ti_Data; break;
            case BDJ_Classes:       r->bdr_Classes = t->ti_Data;     r->bdr_Given |= BDRF_CLASSES; break;
            case BDJ_Timeout:       r->bdr_Timeout = t->ti_Data; break;
            case BDJ_ErrorCode:     error_code = (ULONG *)t->ti_Data; break;
            default:                r->bdr_Unknown++; break;
        }
    }

    r->bdr_ErrorCode = error_code;

    if (!(r->bdr_Given & BDRF_DST) || !(r->bdr_Given & BDRF_LENGTH))
    {
        return BDERR_ARGS;
    }

    if (((r->bdr_Given & BDRF_SRC) != 0) == ((r->bdr_Given & BDRF_FILL) != 0))
    {
        return BDERR_ARGS;
    }

    if ((r->bdr_Length == 0) || 
        (r->bdr_Length & 3) || 
        (r->bdr_Dst & 3) || 
        ((r->bdr_Given & BDRF_SRC) && (r->bdr_Src & 3)))
    {
        return BDERR_ARGS;
    }

    if (r->bdr_Rows == 0)
    {
        return BDERR_ARGS;
    }

    if (r->bdr_Rows > BDMA_MAX_ROWS)
    {
        return BDERR_TOOBIG;
    }

    if (!(r->bdr_Given & BDRF_SRCPITCH))
    {
        r->bdr_SrcPitch = r->bdr_Length;
    }
    
    if (!(r->bdr_Given & BDRF_DSTPITCH))
    {
        r->bdr_DstPitch = r->bdr_Length;
    }

    if ((r->bdr_Given & BDRF_FILL) && (r->bdr_Given & BDRF_SRCPITCH))
    {
        return BDERR_ARGS;
    }

    if ((r->bdr_Rows > 1) && (
        (r->bdr_DstPitch < r->bdr_Length) || 
        ((r->bdr_Given & BDRF_SRC) && r->bdr_SrcPitch < r->bdr_Length) || 
        (r->bdr_DstPitch & 3) || (r->bdr_SrcPitch & 3)))
    {
        return BDERR_ARGS;
    }

    if ((r->bdr_Given & BDRF_MOVE) && (!(r->bdr_Given & BDRF_SRC) || (r->bdr_SrcPitch != r->bdr_DstPitch)))
    {
        return BDERR_ARGS;
    }

    if (r->bdr_Priority > BDPRI_HIGH)
    {
        return BDERR_ARGS;
    }

    if (r->bdr_Classes == 0 || (r->bdr_Classes & ~(ULONG)BDCLASS_ALL))
    {
        return BDERR_ARGS;
    }

    if ((r->bdr_Given & BDRF_STRICT) && r->bdr_Unknown)
    {
        return BDERR_ARGS;
    }

    return BDERR_OK;
}

ULONG BDMA_FillQuery(
    struct Library *UtilityBase, 
    const struct TagItem *tags, 
    const struct BDMAStatus *s)
{
    struct TagItem *t;
    const struct TagItem *list = tags;
    ULONG answered = 0;

    while ((t = NextTagItem((struct TagItem **)&list)) != NULL)
    {
        ULONG *to = (ULONG *)t->ti_Data;
        ULONG value;

        switch (t->ti_Tag)
        {
            case BDI_Available: value = s->bds_Available; break;
            case BDI_Version:   value = s->bds_Version; break;
            case BDI_Interrupt: value = s->bds_Interrupt; break;
            case BDI_Model:     value = s->bds_Model; break;
            case BDI_Features:  value = s->bds_Features; break;
            case BDI_Channels:  value = s->bds_Channels; break;
            case BDI_MaxRows:   value = s->bds_MaxRows; break;
            case BDI_Jobs:      value = s->bds_Jobs; break;
            case BDI_MegaBytes: value = s->bds_MegaBytes; break;
            case BDI_Failures:  value = s->bds_Failures; break;
            case BDI_Classes:   value = s->bds_Classes; break;
            case BDI_VideoBase: value = s->bds_VideoBase; break;
            case BDI_VideoSize: value = s->bds_VideoSize; break;
            case BDI_QueueMax:  value = s->bds_QueueMax; break;
            case BDI_WaitMaxUs: value = s->bds_WaitMaxUs; break;
            case BDI_Aborts:    value = s->bds_Aborts; break;
            case BDI_Timeouts:  value = s->bds_Timeouts; break;
            case BDI_Busy:      value = s->bds_Busy; break;
            case BDI_Slices:    value = s->bds_Slices; break;
            case BDI_Deferred:  value = s->bds_Deferred; break;
            case BDI_BusyMs0:   value = s->bds_BusyMs[0]; break;
            case BDI_BusyMs1:   value = s->bds_BusyMs[1]; break;
            case BDI_Size64:    value = s->bds_Size[0]; break;
            case BDI_Size4K:    value = s->bds_Size[1]; break;
            case BDI_Size32K:   value = s->bds_Size[2]; break;
            case BDI_Size1M:    value = s->bds_Size[3]; break;
            case BDI_SizeBig:   value = s->bds_Size[4]; break;
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
