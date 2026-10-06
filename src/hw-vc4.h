/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _HW_VC4_H
#define _HW_VC4_H

#include <exec/types.h>

/*
    VideoCore IV:
	the DMA engine of the BCM2835 family (Pi Zero, 1, 2, 3, Zero 2),
	which the BCM2711 keeps for its channels 0 to 10: the "normal" channels
    and the "lite" ones (bit LITE of their DEBUG register, a length of 16 bits).
	What the generations share is here; the 40 bit channels of the BCM2711 are
    in hw-vc6.h, which includes this file.
    Emu68 maps the peripherals of the Pi at 0xf2000000:
	the bus address 0x7e000000 + n is at 0xf2000000 + n.
	The registers are little endian: the 68k reads and writes them swapped.
*/

#define LE32(x)                 __builtin_bswap32(x)

#define ARM_IO_BASE             0xf2000000UL

/* System timer: the low word of the counter, 1 MHz */
#define SYSTEM_TIMER_BASE       (ARM_IO_BASE + 0x003000UL)
#define SYSTEM_TIMER_CLOCK      (SYSTEM_TIMER_BASE + 0x04)

/* DMA: the channel n is at DMA_BASE + 0x100 * n (the 40 bit channels 11 to 14 are the node /scb/dma@7e007b00 of the device tree, see hw-vc6.h) */
#define DMA_BASE                (ARM_IO_BASE + 0x007000UL)
#define DMA_CHANNEL_SIZE        0x100
#define DMA_INT_STATUS          (ARM_IO_BASE + 0x007fe0UL)  /* one bit per channel */
#define DMA_ENABLE              (ARM_IO_BASE + 0x007ff0UL)  /* one bit per channel */

/* registers of a channel, the same offsets on every class */
#define DMA_CS                  0x00        /* control and status */
#define DMA_CB                  0x04        /* the control block: its bus address for a legacy channel, its address >> 5 for a 40 bit one; 0 = idle */

/* the DEBUG register of a legacy channel (the 40 bit channels have it at 0x0c) */
#define LDEBUG                  0x20
#define LDEBUG_LITE             (1UL << 28)

/* CS of a legacy channel */
#define L_ACTIVE                (1UL << 0)
#define L_END                   (1UL << 1)
#define L_INT                   (1UL << 2)
#define L_ERROR                 (1UL << 8)
#define L_WAITWRITES            (1UL << 28) /* wait for the outstanding writes */
#define L_ABORT                 (1UL << 30)
#define L_RESET                 (1UL << 31)

/* TI of a legacy control block (from the BCM2835 manual and the Linux driver, measured by test/lab.c) */
#define L_TI_INTEN              (1UL << 0)
#define L_TI_WAITRESP           (1UL << 3)
#define L_TI_DINC               (1UL << 4)
#define L_TI_DWIDTH             (1UL << 5)
#define L_TI_SINC               (1UL << 8)
#define L_TI_SWIDTH             (1UL << 9)
#define L_TI_BURST(n)           ((ULONG)(n) << 12)

/* the longest block of a lite channel */
#define L_LITE_MAX_LEN          65532UL

static inline ULONG dma_rd(ULONG channel, ULONG reg)
{
    return LE32(*(volatile ULONG *)(DMA_BASE + channel * DMA_CHANNEL_SIZE + reg));
}

static inline void dma_wr(ULONG channel, ULONG reg, ULONG value)
{
    *(volatile ULONG *)(DMA_BASE + channel * DMA_CHANNEL_SIZE + reg) = LE32(value);
}

/* microseconds, wrapping every 71 minutes: for differences only */
static inline ULONG timer_now(void)
{
    return LE32(*(volatile ULONG *)SYSTEM_TIMER_CLOCK);
}

#endif /* _HW_VC4_H */
