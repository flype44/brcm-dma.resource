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
#include <exec/execbase.h>
#include <proto/exec.h>

/*
    Cache maintenance around a transfer: CachePreDMA() and CachePostDMA() of exec, always.
    With the Emu68 that has the private data-cache range opcodes (property /emu68 dcache-range-ops), 68040.library 2.0
    does the area in one native loop on the real lines of 64 bytes and one dsb: 0 us for 64 bytes, 9 us for 256 KB,
    37 us for 1 MB, 146 us for 4 MB (brcm-dma-cost), and only the lines of the area are touched. The lines at the two
    ends are pushed, never dropped alone, and a source that the DMA only reads is written back and kept
    (DMA_ReadFromRAM).
    On an older Emu68 the same calls are the CPUSHA of the ROM: correct, but a fixed 41 us whatever the area, and the
    whole data cache of every task is lost each time.
*/
#define BDMA_INTERRUPT_MAX  262144      /* the maintenance after a job is done in the interrupt up to here (9 us), above in a software interrupt */

/* Before the DMA: write back what the CPU wrote, and drop the lines when the DMA writes the area. CachePreDMA() gives back
   the length of the contiguous piece it handled: loop until the area is done */
static inline VOID BDMA_CacheBefore(struct ExecBase *SysBase, ULONG address, ULONG bytes, ULONG flags)
{
    ULONG first = 1;

    while (bytes)
    {
        LONG l = bytes;

        CachePreDMA((APTR)address, &l, flags | (first ? 0 : DMA_Continue));
        first = 0;

        if (l <= 0 || (ULONG)l > bytes)
        {
            break;
        }

        address += l;
        bytes -= l;
    }
}

/* After the DMA wrote the area: the CPU must read the memory, not the lines */
static inline VOID BDMA_CacheAfter(struct ExecBase *SysBase, ULONG address, ULONG bytes)
{
    ULONG first = 1;

    while (bytes)
    {
        LONG l = bytes;

        CachePostDMA((APTR)address, &l, first ? 0 : DMA_Continue);
        first = 0;

        if (l <= 0 || (ULONG)l > bytes)
        {
            break;
        }

        address += l;
        bytes -= l;
    }
}

#endif /* _CACHE_H */
