/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _CACHE_H
#define _CACHE_H

#include <exec/types.h>

/*
    Cache maintenance around a transfer.
    Measured on a Pi 4B at 1.8 GHz (brcm-dma-cost), on a machine whose ROM is exec 47.13:
    CachePreDMA(), CachePostDMA() and CacheClearE() are all CPUSHA of the ROM, whatever the area (decoded from the
    ROM): Emu68 turns it into dc cisw on every level of the data cache, L1 and L2: a fixed 41 us, from 64 bytes to
    1 MB, and the whole data cache of every task is lost each time. CPUSHL (one line of 64 bytes) is dsb, dc civac,
    dsb and the end of the translated block: 0.3 us a line. CPUSHP (one page of 4 KB) is one native loop of
    dc civac and one dsb: 0.5 us for 4 KB, 1.2 us for 16 KB, 5.3 us for 64 KB, 15 us for 256 KB (on clean lines).
    So: up to BDMA_LINEWISE_MAX the lines one by one (a small area does not push the lines around it away), up to
    BDMA_PAGEWISE_MAX page by page (CPUSHP: the first and the last page push their neighbours too, which is
    harmless), above it the call of the system (a fixed cost, and it purges the whole cache). CPUSHL and CPUSHP
    write back a dirty line and drop it, CINVL drops it: a line shared with the data of somebody else is always
    pushed first, never dropped alone. After the DMA the pages are pushed, not invalidated: a line of the area that
    the CPU wrote during the transfer would be written back over the data, as a CPUSHL of the first and last line
    does.
*/
#define BDMA_CACHE_LINE     64
#define BDMA_CACHE_PAGE     4096
#define BDMA_LINEWISE_MAX   1024        /* the lines one by one up to here */
#define BDMA_PAGEWISE_MAX   524288      /* page by page (CPUSHP) up to here; above, the call of the system */
#define BDMA_INTERRUPT_MAX  131072      /* the maintenance after a job is done in the interrupt up to here (8 us), above in a software interrupt */

/* Write back what the CPU wrote, and drop the lines: before the DMA reads or writes the area */
static inline VOID BDMA_PushLines(ULONG address, ULONG bytes)
{
    ULONG p = address & ~(ULONG)(BDMA_CACHE_LINE - 1);
    ULONG end = (address + bytes + BDMA_CACHE_LINE - 1) & ~(ULONG)(BDMA_CACHE_LINE - 1);

    for (; p < end; p += BDMA_CACHE_LINE)
    {
        asm volatile("nop; cpushl dc,(%0)" :: "a"(p) : "memory");
    }
}

/* Drop the lines of an area that the DMA wrote, so that the CPU reads the memory: the first and the last line may hold data that is not part of the
   area, they are pushed (and dropped) instead */
static inline VOID BDMA_DropLines(ULONG address, ULONG bytes)
{
    ULONG first = address & ~(ULONG)(BDMA_CACHE_LINE - 1);
    ULONG end = (address + bytes + BDMA_CACHE_LINE - 1) & ~(ULONG)(BDMA_CACHE_LINE - 1);
    ULONG p;

    for (p = first; p < end; p += BDMA_CACHE_LINE)
    {
        if (p == first && address != first)
        {
            asm volatile("nop; cpushl dc,(%0)" :: "a"(p) : "memory");
        }
        else if (p + BDMA_CACHE_LINE == end && ((address + bytes) & (BDMA_CACHE_LINE - 1)))
        {
            asm volatile("nop; cpushl dc,(%0)" :: "a"(p) : "memory");
        }
        else
        {
            asm volatile("cinvl dc,(%0)" :: "a"(p) : "memory");
        }
    }
}

/* Write back what the CPU wrote and drop the pages, 4 KB an instruction (Emu68 runs the loop of dc civac itself) */
static inline VOID BDMA_PushPages(ULONG address, ULONG bytes)
{
    ULONG p = address & ~(ULONG)(BDMA_CACHE_PAGE - 1);
    ULONG end = (address + bytes + BDMA_CACHE_PAGE - 1) & ~(ULONG)(BDMA_CACHE_PAGE - 1);

    for (; p < end; p += BDMA_CACHE_PAGE)
    {
        asm volatile("nop; cpushp dc,(%0)" :: "a"(p) : "memory");
    }
}

/* Before the DMA: TRUE when the maintenance is done here (lines or pages), FALSE when the area is so big that the caller
   uses the call of the system (CachePreDMA(), or CacheClearE()) */
static inline BOOL BDMA_PushRange(ULONG address, ULONG bytes)
{
    if (bytes <= BDMA_LINEWISE_MAX)
    {
        BDMA_PushLines(address, bytes);
        return TRUE;
    }

    if (bytes <= BDMA_PAGEWISE_MAX)
    {
        BDMA_PushPages(address, bytes);
        return TRUE;
    }

    return FALSE;
}

/* After the DMA wrote the area: TRUE when done here, FALSE when the caller uses CachePostDMA() */
static inline BOOL BDMA_DropRange(ULONG address, ULONG bytes)
{
    if (bytes <= BDMA_LINEWISE_MAX)
    {
        BDMA_DropLines(address, bytes);
        return TRUE;
    }

    if (bytes <= BDMA_PAGEWISE_MAX)
    {
        BDMA_PushPages(address, bytes);
        return TRUE;
    }

    return FALSE;
}

#endif /* _CACHE_H */
