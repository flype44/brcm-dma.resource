/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _HW_VC6_H
#define _HW_VC6_H

#include "hw-vc4.h"

/*
    VideoCore VI:
	What the BCM2711 (Pi 4B, 400, CM4) adds to the DMA engine of hw-vc4.h:
	the 40 bit channels 11 to 14 (DMA40), at the same DMA_BASE + 0x100 * n
	(the node /scb/dma@7e007b00 of the device tree is the channel 11).
	The registers are named here and nowhere else.
*/

/* the DEBUG register of a 40 bit channel */
#define DMA_DEBUG               0x0c

/* DMA_CS of a 40 bit channel */
#define CS_ACTIVE               (1UL << 0)
#define CS_END                  (1UL << 1)
#define CS_INT                  (1UL << 2)
#define CS_ERROR                (1UL << 10) /* of the 40 bit channels (the bit 8 of the legacy ones is part of CS_PROT here and always reads 1) */
#define CS_PROT                 0x300UL     /* part of every write to CS */
#define CS_TRANSACTIONS         (1UL << 25)
#define CS_WAIT_FOR_WRITES      (1UL << 28)
#define CS_DISDEBUG             (1UL << 29) /* set on the channels of the firmware */

/* DMA_DEBUG */
#define DEBUG_RESET             (1UL << 23)

/* a control block of a 40 bit channel: 8 longwords, 32 byte aligned, its address in physical memory (divided by 32 in DMA_CB) */
#define DMA_CB_BYTES            32
#define DMA_CB_TI               0           /* transfer information */
#define DMA_CB_SRC              1           /* source address */
#define DMA_CB_SRCI             2           /* upper address bits | source information */
#define DMA_CB_DST              3
#define DMA_CB_DSTI             4
#define DMA_CB_LEN              5           /* bytes to transfer */
#define DMA_CB_NEXT             6           /* the next control block >> 5, 0 = the last one */
#define DMA_CB_WORDS            8

/* DMA_CB_TI */
#define TI_INTEN                (1UL << 0)  /* interrupt when this block is done */
#define TI_WAIT_RESP            (1UL << 2)  /* wait for the response of the writes of this block before the next one starts */

/* DMA_CB_SRCI, DMA_CB_DSTI */
#define INFO_INC                (1UL << 12) /* the address increments */
#define INFO_128_BURST16        ((2UL << 13) | (15UL << 8))     /* 128 bit accesses, bursts of 16 */

#endif /* _HW_VC6_H */
