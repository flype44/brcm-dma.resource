/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/


/*
    The BCM2711 backend: the two 40 bit DMA channels that the ARM may use (12 and 13), and their interrupts through gic400.library.

    A job is a chain of control blocks, up to three a row: a head and a tail with 32 bit accesses, which bring the destination to a
    16 byte boundary (a 128 bit write to an unaligned destination froze the machine in the video memory), and a body with 128 bit
    accesses and bursts of 16 (0.82 ns a byte, as the VPU). The last block raises an interrupt.

    The interface with the common engine (engine.c) is the table BDMA_VC6Backend at the end of this file; this file calls back
    BDMA_ChannelEnded() and BDMA_StartOnChannel(). Nothing here knows about queues, priorities or replies.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <proto/exec.h>
#include <proto/devicetree.h>
#include <proto/utility.h>
#include <proto/gic400.h>

#include <libraries/gic400.h>
#include <common/compiler.h>

#include "brcm-dma.h"
#include "hw-vc6.h"
#include "mbox.h"
#include "cache.h"

#define FIRST_40BIT_CHANNEL 11 /* the registers of the node /scb/dma@7e007b00 start at the channel 11 */
#define LAST_40BIT_CHANNEL  14

#define CONSTANT_SIZE       256
#define SELFTEST_SIZE       4096

#define GIC_MIN_VERSION     1
#define GIC_MIN_REVISION    3

/* Gives the GPU memory back (the engine did not start) */
static VOID ReleaseGPU(
    struct Library *MailboxBase, 
    struct BDMABase *BDMABase)
{
    UnlockMemory(MailboxBase, BDMABase->bdb_Handle); /* the sequence of the firmware: lock, unlock, release (it may not have been locked: it does not matter) */
    ReleaseMemory(MailboxBase, BDMABase->bdb_Handle);
    BDMABase->bdb_Handle = 0;
}

/* The way the Linux driver stops a channel: pause, wait for the transactions, default CS, reset.
   Also used by the sweep of the start (a channel that an earlier session left running) and by 
   BDMA_AbortJob(). Nothing waits long: the wait is bounded.
*/
static VOID StopChannel(
    struct BDMAChannel *channel)
{
    ULONG chan = channel->bc_Number;
    ULONG timeout = 100;

    if (dma_rd(chan, DMA_CB) != 0)
    {
        dma_wr(chan, DMA_CS, dma_rd(chan, DMA_CS) & ~CS_ACTIVE);

        while ((dma_rd(chan, DMA_CS) & CS_TRANSACTIONS) && --timeout);

        dma_wr(chan, DMA_CS, CS_PROT);
        dma_wr(chan, DMA_DEBUG, dma_rd(chan, DMA_DEBUG) | DEBUG_RESET);
    }

    /* END and INT of the session before, if any (it can have left them set),
       or the interrupt would be raised at once */
    dma_wr(chan, DMA_CS, CS_INT | CS_END | CS_PROT);
}

/* The chain of a strip of rows of a move that overlaps inside the rows: the rows of the strip are copied into the buffer of the channel (the
   blocks wait for the response of their writes), then out of it to the destination. All the reads of the strip are made before its first
   write; the strips follow each other in the order of the rows (the last first when the destination is above the source). A row is a unit.
   The buffer has the rows at multiples of 16 bytes, so its writes are aligned. */
static VOID BuildBounce(
    struct BDMABase *BDMABase,
    struct BDMAChannel *channel,
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    const struct BDMARequest *r = &job->bj_Request;
    ULONG *cb = channel->bc_Chain;
    ULONG chain = (ULONG)channel->bc_Chain;
    ULONG width = r->bdr_Length, height = r->bdr_Rows;
    ULONG aw = (width + 15) & ~15UL;
    ULONG first = job->bj_Unit;
    ULONG n = BDMA_BOUNCE_SIZE / aw, k = 0, i, part;

    job->bj_Units = height;

    if (n > BDMA_BOUNCE_ROWS)
    {
        n = BDMA_BOUNCE_ROWS;
    }

    if (n > height - first)
    {
        n = height - first;
    }

    /* out of the source into the buffer */
    for (i = 0; i < n; i++)
    {
        ULONG row = first + i;
        ULONG ry = job->bj_Reverse ? height - 1 - row : row;
        ULONG s = r->bdr_Src + ry * r->bdr_SrcPitch;
        ULONG d = (ULONG)channel->bc_Bounce + i * aw;
        ULONG body = width & ~15UL, tail = width - body;

        for (part = 0; part < 2; part++)
        {
            ULONG *c = cb + k * DMA_CB_WORDS;
            ULONG off = part == 0 ? 0 : body;
            ULONG len = part == 0 ? body : tail;
            ULONG info = INFO_INC | (part == 0 ? INFO_128_BURST16 : 0);

            if (len == 0)
            {
                continue;
            }

            c[DMA_CB_TI]   = LE32(TI_WAIT_RESP);
            c[DMA_CB_SRC]  = LE32(s + off);
            c[DMA_CB_SRCI] = LE32(info);
            c[DMA_CB_DST]  = LE32(d + off);
            c[DMA_CB_DSTI] = LE32(info);
            c[DMA_CB_LEN]  = LE32(len);
            c[DMA_CB_NEXT] = LE32((chain + (k + 1) * DMA_CB_BYTES) >> 5);
            c[7] = 0;
            k++;
        }
    }

    /* out of the buffer into the destination: head and tail with 32 bit accesses around a 128 bit body, as for any other job */
    for (i = 0; i < n; i++)
    {
        ULONG row = first + i;
        ULONG ry = job->bj_Reverse ? height - 1 - row : row;
        ULONG s = (ULONG)channel->bc_Bounce + i * aw;
        ULONG t = r->bdr_Dst + ry * r->bdr_DstPitch;
        ULONG head = (0 - t) & 15, body, tail, off[3], len[3];

        if (head > width)
        {
            head = width;
        }

        body = (width - head) & ~15UL;
        tail = width - head - body;

        off[0] = 0;
        off[1] = head;
        off[2] = head + body;

        len[0] = head;
        len[1] = body;
        len[2] = tail;

        for (part = 0; part < 3; part++)
        {
            ULONG *c = cb + k * DMA_CB_WORDS;
            ULONG info = INFO_INC | (part == 1 ? INFO_128_BURST16 : 0);

            if (len[part] == 0)
            {
                continue;
            }

            c[DMA_CB_TI]   = LE32(0);
            c[DMA_CB_SRC]  = LE32(s + off[part]);
            c[DMA_CB_SRCI] = LE32(info);
            c[DMA_CB_DST]  = LE32(t + off[part]);
            c[DMA_CB_DSTI] = LE32(info);
            c[DMA_CB_LEN]  = LE32(len[part]);
            c[DMA_CB_NEXT] = LE32((chain + (k + 1) * DMA_CB_BYTES) >> 5);
            c[7] = 0;
            k++;
        }
    }

    job->bj_Unit = first + n;

    cb[(k - 1) * DMA_CB_WORDS + DMA_CB_NEXT] = 0;
    cb[(k - 1) * DMA_CB_WORDS + DMA_CB_TI] |= LE32(TI_INTEN);

    if (DMA_CB_BYTES * k <= BDMA_LINEWISE_MAX)
    {
        BDMA_PushLines((ULONG)cb, DMA_CB_BYTES * k);
    }
    else
    {
        CacheClearE(cb, DMA_CB_BYTES * k, CACRF_ClearD);
    }
}

/* Builds, in the buffer of the channel, the chain of the next slice of the job (the channel is started by Arm()).
   src and dst are physical (the CPU address of RAM and of the RTG memory under Emu68).
   With `reverse` (a move whose destination is above the source) the rows, and the blocks
   of a row, go from the last to the first, or a block would overwrite source bytes that 
   a later one still has to read (the overlap inside a row was refused).
*/
static VOID Build(
    struct BDMABase *BDMABase, 
    struct BDMAChannel *channel, 
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    const struct BDMARequest *r = &job->bj_Request;
    BOOL fill = (r->bdr_Given & BDRF_FILL) != 0;
    ULONG *cb = channel->bc_Chain;
    ULONG chain = (ULONG)channel->bc_Chain;
    ULONG width = r->bdr_Length, height = r->bdr_Rows;
    ULONG src = fill ? (ULONG)channel->bc_Constant : r->bdr_Src;
    ULONG srcpitch = fill ? 0 : r->bdr_SrcPitch;
    ULONG k = 0, part, i;
    ULONG pieces = (width + BDMA_SLICE_BYTES - 1) / BDMA_SLICE_BYTES;     /* units of a row */
    ULONG u, bytes = 0;

    if (job->bj_Bounce)
    {
        BuildBounce(BDMABase, channel, job);
        return;
    }

    job->bj_Units = height * pieces;

    if (fill)
    {
        /* The constant: 256 bytes of the value, which is what a burst of 16 beats of 128 bits reads (a burst reads its beats one after
           the other even when the source does not increment: with only 16 bytes the rest of the row got what lay behind them). */
        /* The same value as the last fill of this channel (the usual case: one colour, many rectangles) needs nothing: the constant is in place, and
           the cache is not touched again (a call of the system for the cache costs about 41 us, by line a few). */
        if (!channel->bc_FillSet || channel->bc_Fill != r->bdr_Fill)
        {
            for (i = 0; i < CONSTANT_SIZE / 4; i++)
            {
                channel->bc_Constant[i] = r->bdr_Fill;
            }

            BDMA_PushLines((ULONG)channel->bc_Constant, CONSTANT_SIZE);
            channel->bc_Fill = r->bdr_Fill;
            channel->bc_FillSet = TRUE;
        }
    }

    /* One slice: the units from bj_Unit on, until about a megabyte or the capacity of the chain */
    for (u = job->bj_Unit; u < job->bj_Units && bytes < BDMA_SLICE_BYTES && u - job->bj_Unit < BDMA_SLICE_UNITS; u++)
    {
        ULONG row = pieces == 1 ? u : u / pieces;
        ULONG col = pieces == 1 ? 0 : (u - row * pieces) * BDMA_SLICE_BYTES;
        ULONG w = width - col > BDMA_SLICE_BYTES ? BDMA_SLICE_BYTES : width - col;
        ULONG ry = job->bj_Reverse ? height - 1 - row : row;
        ULONG s = src + ry * srcpitch + col;
        ULONG t = r->bdr_Dst + ry * r->bdr_DstPitch + col;
        ULONG head = (0 - t) & 15, body, tail, off[3], len[3];

        if (head > w)
        {
            head = w;
        }
        
        body = (w - head) & ~15UL;
        tail = w - head - body;

        off[0] = 0;
        off[1] = head;
        off[2] = head + body;
        
        len[0] = head;
        len[1] = body;
        len[2] = tail;
        
        for (part = 0; part < 3; part++)
        {
            ULONG p = job->bj_Reverse ? 2 - part : part;
            ULONG *c = cb + k * DMA_CB_WORDS;
            ULONG info = INFO_INC | (p == 1 ? INFO_128_BURST16 : 0);
            ULONG sinfo = fill ? info & ~INFO_INC : info; /* a fill reads the same constant over and over */

            if (len[p] == 0)
            {
                continue;
            }

            c[DMA_CB_TI]   = LE32(0); /* the interrupt of the last block is set below */
            c[DMA_CB_SRC]  = LE32(fill ? src : s + off[p]);
            c[DMA_CB_SRCI] = LE32(sinfo);
            c[DMA_CB_DST]  = LE32(t + off[p]);
            c[DMA_CB_DSTI] = LE32(info);
            c[DMA_CB_LEN]  = LE32(len[p]);
            c[DMA_CB_NEXT] = LE32((chain + (k + 1) * DMA_CB_BYTES) >> 5);
            c[7] = 0; /* reserved */
            k++;
        }

        bytes += w;
    }

    job->bj_Unit = u;

    cb[(k - 1) * DMA_CB_WORDS + DMA_CB_NEXT] = 0;
    cb[(k - 1) * DMA_CB_WORDS + DMA_CB_TI] |= LE32(TI_INTEN);
    
    if (DMA_CB_BYTES * k <= BDMA_LINEWISE_MAX)
    {
        BDMA_PushLines((ULONG)cb, DMA_CB_BYTES * k);
    }
    else
    {
        CacheClearE(cb, DMA_CB_BYTES * k, CACRF_ClearD);
    }
}

/* Starts the channel on the chain that Build() made */
static VOID Arm(
    struct BDMAChannel *channel)
{
    dma_wr(channel->bc_Number, DMA_CS, CS_END | CS_PROT);
    dma_wr(channel->bc_Number, DMA_CB, (ULONG)channel->bc_Chain >> 5);
    dma_wr(channel->bc_Number, DMA_CS, CS_WAIT_FOR_WRITES | CS_ACTIVE | CS_PROT);
}

/* Called by gic400.library at the interrupt of a channel, as a subroutine, with the
   channel in a1 (the is_Data of the interrupt): it acknowledges (level triggered: an
   interrupt that is not acknowledged is a storm) and tells the engine how the slice ended.
*/
static ULONG Interrupt(
    REGARG(struct BDMAChannel *channel, "a1"))
{
    ULONG cs = dma_rd(channel->bc_Number, DMA_CS);

    channel->bc_Calls++;

    if ((cs & CS_INT) == 0)
    {
        return 0;
    }

    /* writing 1 clears them */
    dma_wr(channel->bc_Number, DMA_CS, CS_INT | CS_END | CS_PROT);

    BDMA_ChannelEnded(channel->bc_Base, channel, (cs & CS_ERROR) ? BDERR_HW : BDERR_OK);

    return 0;
}

/* The self test of a channel: 4 KB through the real path, polled, 
   must arrive intact and raise the interrupt of the channel
*/
static ULONG SelfTest(
    struct BDMABase *BDMABase, 
    struct BDMAChannel *channel, 
    UBYTE *scratch)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct BDMAJob job;
    ULONG i, t0;
    ULONG why = BCW_OK;

    for (i = 0; i < SELFTEST_SIZE; i++)
    {
        scratch[i] = (UBYTE)(i * 7 + 3);
        scratch[SELFTEST_SIZE + i] = 0;
    }

    CacheClearE(scratch, 2 * SELFTEST_SIZE, CACRF_ClearD);

    for (i = 0; i < sizeof(job); i++)
    {
        ((UBYTE *)&job)[i] = 0;
    }

    job.bj_Test = 1;
    job.bj_Request.bdr_Given = BDRF_SRC | BDRF_DST | BDRF_LENGTH;
    job.bj_Request.bdr_Src = (ULONG)scratch;
    job.bj_Request.bdr_Dst = (ULONG)scratch + SELFTEST_SIZE;
    job.bj_Request.bdr_Length = SELFTEST_SIZE;
    job.bj_Request.bdr_Rows = 1;
    job.bj_Request.bdr_SrcPitch = SELFTEST_SIZE;
    job.bj_Request.bdr_DstPitch = SELFTEST_SIZE;

    Disable();
    channel->bc_Job = &job;
    channel->bc_Starting = 1;
    job.bj_Active = 1;
    job.bj_State = BJS_RUNNING;
    Enable();

    BDMA_StartOnChannel(BDMABase, channel, &job);

    t0 = timer_now();
    while (job.bj_State < BJS_DONE && timer_now() - t0 < 50000);

    if (job.bj_State != BJS_DONE)
    {
        why = BCW_NOINTERRUPT;

        /* what the channel says, and whether the data arrived anyway (the interrupt is what is missing, or the whole transfer) */
        channel->bc_TestCS = dma_rd(channel->bc_Number, DMA_CS);
        channel->bc_TestCB = dma_rd(channel->bc_Number, DMA_CB);
        CacheClearE(scratch, 2 * SELFTEST_SIZE, CACRF_ClearD);
        channel->bc_TestCopied = scratch[SELFTEST_SIZE] == scratch[0] && scratch[2 * SELFTEST_SIZE - 1] == (UBYTE)((SELFTEST_SIZE - 1) * 7 + 3);

        StopChannel(channel);
        Disable();
        channel->bc_Job = NULL;
        Enable();
    }
    else
    {
        CacheClearE(scratch, 2 * SELFTEST_SIZE, CACRF_ClearD);

        for (i = 0; i < SELFTEST_SIZE; i++)
        {
            if (scratch[SELFTEST_SIZE + i] != (UBYTE)(i * 7 + 3))
            {
                why = BCW_DATA;
                break;
            }
        }
    }

    return why;
}

/* What the device tree says about the 40 bit DMA of the machine: the channels and the interrupts.
   FALSE: no such node.
*/
static BOOL ReadDeviceTree(
    struct BDMABase *BDMABase, 
    ULONG *interrupts)
{
    APTR DeviceTreeBase = BDMABase->bdb_DeviceTreeBase;
    struct Library *UtilityBase = BDMABase->bdb_UtilityBase;
    BOOL found = FALSE;
    APTR key;

    /* the RTG memory, which exec does not know */
    key = DT_OpenKey("/emu68");
    if (key != NULL)
    {
        APTR prop = DT_FindProperty(key, "vc4-mem");

        if (prop != NULL && DT_GetPropLen(prop) >= 2 * sizeof(ULONG))
        {
            const ULONG *cells = DT_GetPropValue(prop);

            BDMABase->bdb_VCBase = cells[0];
            BDMABase->bdb_VCSize = cells[1];
        }

        DT_CloseKey(key);
    }

    key = DT_OpenKey("/scb/dma@7e007b00");
    if (key != NULL)
    {
        APTR prop = DT_FindProperty(key, "compatible");

        if (prop != NULL && Stricmp((CONST_STRPTR)DT_GetPropValue(prop), "brcm,bcm2711-dma") == 0)
        {

            prop = DT_FindProperty(key, "brcm,dma-channel-mask");
            if (prop != NULL && DT_GetPropLen(prop) >= sizeof(ULONG))
            {
                BDMABase->bdb_ChannelMask = *(const ULONG *)DT_GetPropValue(prop);
            }

            /* the interrupts of the channels 11 ...: three cells each (type, number, flags); a SPI (type 0) has the GIC id number + 32 */
            prop = DT_FindProperty(key, "interrupts");
            if (prop != NULL)
            {
                const ULONG *cells = DT_GetPropValue(prop);
                ULONG count = DT_GetPropLen(prop) / (3 * sizeof(ULONG)), i;

                for (i = 0; i < count && i <= LAST_40BIT_CHANNEL - FIRST_40BIT_CHANNEL; i++)
                {
                    if (cells[3 * i] == 0)
                    {
                        interrupts[i] = cells[3 * i + 1] + 32;
                    }
                }
            }

            found = TRUE;
        }

        DT_CloseKey(key);
    }

    return found;
}

/* The hardware side of BDMA_StartEngine(). Called by a task, under the semaphore of the base.
   Everything is checked and the engine stays unavailable (the clients stay on the CPU) when something is 
   missing: the model, a free channel that is not the firmware's, its interrupt, gic400.library 1.3 or later, 
   memory for the control blocks, and a 4 KB copy that must arrive intact and raise exactly the interrupt of its channel.
*/
static BOOL Start(
    struct BDMABase *BDMABase)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    APTR DeviceTreeBase;
    struct Library *MailboxBase;
    struct Library *GIC400_Base;
    ULONG interrupts[LAST_40BIT_CHANNEL - FIRST_40BIT_CHANNEL + 1] = { 0, 0, 0, 0 };
    ULONG mask, size, bus, phys, firmware, number, i;
    UBYTE *scratch;

    BDMABase->bdb_DeviceTreeBase = OpenResource("devicetree.resource");
    DeviceTreeBase = BDMABase->bdb_DeviceTreeBase;
    MailboxBase = BDMABase->bdb_MailboxBase;

    if (DeviceTreeBase == NULL || MailboxBase == NULL)
    {
        BDMABase->bdb_Reason = BDW_RESOURCES;
        return FALSE;
    }

    if (!ReadDeviceTree(BDMABase, interrupts))
    {
        BDMABase->bdb_Reason = BDW_DEVICETREE;
        return FALSE;
    }

    /* the channels: the device tree mask, as far as the firmware agrees, 
       40 bit ones only, never the firmware's own (DISDEBUG) */
    mask = BDMABase->bdb_ChannelMask;
    firmware = GetDMAChannels(MailboxBase);
    BDMABase->bdb_Firmware = firmware;
    
    if (firmware != 0 && firmware != 0xffffffffUL)
    {
        mask &= firmware;
    }

    BDMABase->bdb_Channels = 0;
    
    for (number = FIRST_40BIT_CHANNEL; 
        number <= LAST_40BIT_CHANNEL && BDMABase->bdb_Channels < BDMA_MAX_CHANNELS; 
        number++)
    {
        ULONG irq = interrupts[number - FIRST_40BIT_CHANNEL];
        ULONG cs;

        if ((mask & (1UL << number)) == 0 || irq == 0)
        {
            continue;
        }

        cs = dma_rd(number, DMA_CS);
        if (cs & CS_DISDEBUG)
        {
            continue;
        }

        BDMABase->bdb_Channel[BDMABase->bdb_Channels].bc_Number = number;
        BDMABase->bdb_Channel[BDMABase->bdb_Channels].bc_Interruptid = irq;
        BDMABase->bdb_Channels++;
    }

    if (BDMABase->bdb_Channels == 0)
    {
        BDMABase->bdb_Reason = BDW_NOCHANNEL;
        return FALSE;
    }

    BDMABase->bdb_GicBase = OpenLibrary("gic400.library", 0);
    GIC400_Base = BDMABase->bdb_GicBase;
    if (GIC400_Base == NULL)
    {
        BDMABase->bdb_Reason = BDW_NOGIC;
        return FALSE;
    }

    if (BDMABase->bdb_GicBase->lib_Version < GIC_MIN_VERSION ||
        (BDMABase->bdb_GicBase->lib_Version == GIC_MIN_VERSION && 
        BDMABase->bdb_GicBase->lib_Revision < GIC_MIN_REVISION))
    {
        BDMABase->bdb_Reason = BDW_GICOLD;
        return FALSE;
    }

    /* The control blocks of each channel, the 256 byte constant of each, 
       and 2 x 4 KB for the self test: coherent memory of the GPU, 32 byte aligned */
    size = BDMABase->bdb_Channels * (BDMA_CHAIN_SIZE + CONSTANT_SIZE + BDMA_BOUNCE_SIZE) + 2 * SELFTEST_SIZE;
    
    BDMABase->bdb_Handle = AllocateMemory(MailboxBase, size, 32, 
        MEM_FLAG_COHERENT | 
        MEM_FLAG_DIRECT | 
        MEM_FLAG_HINT_PERMALOCK);
    
    if (BDMABase->bdb_Handle == 0 || BDMABase->bdb_Handle == 0xffffffffUL)
    {
        BDMABase->bdb_Handle = 0;
        BDMABase->bdb_Reason = BDW_ALLOC;
        return FALSE;
    }

    bus = LockMemory(MailboxBase, BDMABase->bdb_Handle);
    if (bus == 0 || (bus & 31))
    {
        ReleaseGPU(MailboxBase, BDMABase);
        BDMABase->bdb_Reason = BDW_LOCK;
        return FALSE;
    }

    phys = bus & 0x3fffffffUL;
    scratch = (UBYTE *)(phys + BDMABase->bdb_Channels * (BDMA_CHAIN_SIZE + CONSTANT_SIZE));

    /* The sweep (the OS restarted, the Pi did not:
       a channel can still run what the session before gave it) and the interrupts.
       An id left enabled by that session is not a user: AddIntServerEx() refuses 
       when a handler is registered, so its state is not looked at. */
    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        struct BDMAChannel *channel = &BDMABase->bdb_Channel[i];

        channel->bc_Base = BDMABase;
        channel->bc_Job = NULL;
        channel->bc_Chain = (ULONG *)(phys + i * BDMA_CHAIN_SIZE);
        channel->bc_Constant = (ULONG *)(phys + BDMABase->bdb_Channels * BDMA_CHAIN_SIZE + i * CONSTANT_SIZE);
        channel->bc_Bounce = (ULONG *)(phys + BDMABase->bdb_Channels * (BDMA_CHAIN_SIZE + CONSTANT_SIZE) + 2 * SELFTEST_SIZE + i * BDMA_BOUNCE_SIZE);

        StopChannel(channel);

        channel->bc_Interrupt.is_Node.ln_Type = NT_INTERRUPT;
        channel->bc_Interrupt.is_Node.ln_Pri = 0;
        channel->bc_Interrupt.is_Node.ln_Name = (char *)BRCMDMANAME;
        channel->bc_Interrupt.is_Data = channel;
        channel->bc_Interrupt.is_Code = (void (*)())Interrupt;

        channel->bc_Error = AddIntServerEx(channel->bc_Interruptid, 0, FALSE, &channel->bc_Interrupt);
        
        if (channel->bc_Error == 0)
        {
            channel->bc_Registered = TRUE;
            channel->bc_Why = SelfTest(BDMABase, channel, scratch);

            if (channel->bc_Why != BCW_OK)
            {
                RemIntServerEx(channel->bc_Interruptid, &channel->bc_Interrupt);
                channel->bc_Registered = FALSE;
            }
        }
        else
        {
            channel->bc_Why = BCW_ADDINT;
        }
    }

    /* the channels that did not prove themselves are not used;
       the others stay, in the order of the table */
    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        if (BDMABase->bdb_Channel[i].bc_Registered)
        {
            break;
        }
    }

    if (i == BDMABase->bdb_Channels)
    {
        ReleaseGPU(MailboxBase, BDMABase);
        BDMABase->bdb_Reason = BDW_CHANNELS;
        return FALSE;
    }

    BDMABase->bdb_Features = BDFF_RECT | BDFF_MOVE | BDFF_WIDE | BDFF_FILL;
    BDMABase->bdb_Classes = BDCLASS_DMA40; /* every channel it manages is a 40 bit one: the other classes come with their backend */

    return TRUE;
}

/* The backend of the BCM2711, chosen by the engine when the device tree has its DMA node */
const struct BDMABackend BDMA_VC6Backend =
{
    Start,
    Build,
    Arm,
    StopChannel
};
