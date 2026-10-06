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
    Cache maintenance around a transfer. Measured on a Pi 4B at 1.8 GHz (brcm-dma-cost): one call of CachePreDMA(), CachePostDMA() or CacheClearE() costs a
    fixed 41 us, from 64 bytes to 256 KB; the instruction of the 68040 on every line of 64 bytes (what brcm-emmc.device does) costs 0.4 us for 64 bytes and
    about 1 us a KB. The two cross near 40 KB: below BDMA_LINEWISE_MAX the lines are done one by one, above it the call of the system is used.
    CPUSHL writes back a dirty line and drops it, CINVL drops it: a line shared with the data of somebody else is always pushed first, never dropped alone.
*/
#define BDMA_CACHE_LINE     64
#define BDMA_LINEWISE_MAX   32768

/* Write back what the CPU wrote, and drop the lines: before the DMA reads or writes the area */
static inline void BDMA_PushLines(ULONG address, ULONG bytes)
{
    ULONG p = address & ~(ULONG)(BDMA_CACHE_LINE - 1);
    ULONG end = (address + bytes + BDMA_CACHE_LINE - 1) & ~(ULONG)(BDMA_CACHE_LINE - 1);

    for (; p < end; p += BDMA_CACHE_LINE)
        asm volatile("nop; cpushl dc,(%0)" :: "a"(p) : "memory");
}

/* Drop the lines of an area that the DMA wrote, so that the CPU reads the memory: the first and the last line may hold data that is not part of the
   area, they are pushed (and dropped) instead */
static inline void BDMA_DropLines(ULONG address, ULONG bytes)
{
    ULONG first = address & ~(ULONG)(BDMA_CACHE_LINE - 1);
    ULONG end = (address + bytes + BDMA_CACHE_LINE - 1) & ~(ULONG)(BDMA_CACHE_LINE - 1);
    ULONG p;

    for (p = first; p < end; p += BDMA_CACHE_LINE)
    {
        if (p == first && address != first)
            asm volatile("nop; cpushl dc,(%0)" :: "a"(p) : "memory");
        else if (p + BDMA_CACHE_LINE == end && ((address + bytes) & (BDMA_CACHE_LINE - 1)))
            asm volatile("nop; cpushl dc,(%0)" :: "a"(p) : "memory");
        else
            asm volatile("cinvl dc,(%0)" :: "a"(p) : "memory");
    }
}

#endif /* _CACHE_H */
