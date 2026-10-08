/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    The engine of the BCM2711: the two 40 bit DMA channels that the ARM may use (12 and 13), run as one pool.

    A job is a chain of control blocks, up to three a row: a head and a tail with 32 bit accesses, which bring the destination to a
    16 byte boundary (a 128 bit write to an unaligned destination froze the machine in the video memory), and a body with 128 bit
    accesses and bursts of 16 (0.82 ns a byte, as the VPU). The last block raises an interrupt through gic400.library.

    Everything here comes from the laboratory (reference/dma40.c), with these differences: two channels, a queue with priorities and the
    order between jobs, the interrupt of a channel starts the next job, the task that waits is any task.

    NOTHING of this runs in Init(): the engine starts at the first call that needs it (BDMA_StartEngine, from a task, under the
    semaphore of the base). A resource that touches the hardware before the system is up, in the ROM of a machine that has no serial
    line, can make the machine unbootable; a failure here only leaves the engine unavailable.
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
void BDMA_StopChannel(
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

/* Is (address, address + bytes) memory that a transfer may touch:
   known to exec, or the RTG memory (/emu68 vc4-mem)?
*/
BOOL BDMA_InMemory(
    struct BDMABase *BDMABase, 
    ULONG address, 
    ULONG bytes)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    ULONG last = address + bytes - 1;

    if (bytes == 0 || last < address)
    {
        return FALSE;
    }

    if (address >= BDMABase->bdb_VCBase && last - BDMABase->bdb_VCBase < BDMABase->bdb_VCSize)
    {
        return TRUE;
    }

    return TypeOfMem((APTR)address) != 0 && TypeOfMem((APTR)last) != 0;
}

/* Is (lo, hi) all in the memory of the RTG board?
   The CPU does not cache it: no cache maintenance there
   (what VideoCore.card knew, now the resource's)
*/
BOOL BDMA_InVideoMemory(
    struct BDMABase *BDMABase, 
    ULONG lo, 
    ULONG hi)
{
    return (
        (BDMABase->bdb_VCSize != 0) && (hi > lo) && 
        (lo >= BDMABase->bdb_VCBase) && 
        (hi - BDMABase->bdb_VCBase <= BDMABase->bdb_VCSize));
}

static BOOL Meets(
    ULONG lo1, 
    ULONG hi1, 
    ULONG lo2, 
    ULONG hi2)
{
    return lo1 < hi1 && lo2 < hi2 && lo1 < hi2 && lo2 < hi1;
}

/* Two jobs conflict when one writes what the other reads or writes:
   the later one has to wait for the earlier one
*/
static BOOL Conflict(
    const struct BDMAJob *a, 
    const struct BDMAJob *b)
{
    return Meets(a->bj_WriteLo, a->bj_WriteHi, b->bj_WriteLo, b->bj_WriteHi) ||
           Meets(a->bj_WriteLo, a->bj_WriteHi, b->bj_ReadLo, b->bj_ReadHi) ||
           Meets(a->bj_ReadLo, a->bj_ReadHi, b->bj_WriteLo, b->bj_WriteHi);
}

/* Builds the chain of the job on the channel and starts it.
   src and dst are physical (the CPU address of RAM and of the RTG memory under Emu68).
   With `reverse` (a move whose destination is above the source) the rows, and the blocks
   of a row, go from the last to the first, or a block would overwrite source bytes that 
   a later one still has to read (the overlap inside a row was refused).
*/
static VOID StartChain(
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

    job->bj_Units = height * pieces;

    if (fill)
    {
        /* The constant: 256 bytes of the value, which is what a burst of 16 beats of 128 bits reads (a burst reads its beats one after
           the other even when the source does not increment: with only 16 bytes the rest of the row got what lay behind them). */
        for (i = 0; i < CONSTANT_SIZE / 4; i++)
        {
            channel->bc_Constant[i] = r->bdr_Fill;
        }
        
        CacheClearE((APTR)channel->bc_Constant, CONSTANT_SIZE, CACRF_ClearD);
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

    /* Arming: the chain is built in the buffer of the channel, which the job holds since Pick(), but a task can have been preempted
       meanwhile and the job aborted. The state is looked at and the channel started in one breath, with the interrupts off. */
    Disable();
    job->bj_Starting = 0;

    if (job->bj_AbortReq)
    {
        channel->bc_Job = NULL;
        job->bj_State = BJS_ABORTED;
        job->bj_Error = BDERR_ABORTED;
        job->bj_Done = 0;
        Enable();

        BDMA_ReplyJob(BDMABase, job);
        return;
    }

    job->bj_Start = timer_now();
    dma_wr(channel->bc_Number, DMA_CS, CS_END | CS_PROT);
    dma_wr(channel->bc_Number, DMA_CB, chain >> 5);
    dma_wr(channel->bc_Number, DMA_CS, CS_WAIT_FOR_WRITES | CS_ACTIVE | CS_PROT);
    Enable();
}

/* Picks the job that may start now on an idle channel and hands it to that channel:
   the best priority, then the order of submission, among the jobs that conflict with
   no running job and with no OLDER job still waiting (a job never overtakes an older
   one it conflicts with: the result is that of the jobs run one by one).
   Called with the interrupts off (Disable(), or from the interrupt). The job is marked 
   running, the channel is taken; the caller then calls StartChain() with the interrupts on if it can.
*/
static struct BDMAChannel *Pick(
    struct BDMABase *BDMABase, 
    struct BDMAJob **picked)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct BDMAChannel *channel = NULL;
    struct BDMAJob *best = NULL, *j, *o;
    ULONG i;

    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        if (BDMABase->bdb_Channel[i].bc_Job == NULL && BDMABase->bdb_Channel[i].bc_Registered)
        {
            channel = &BDMABase->bdb_Channel[i];
            break;
        }
    }

    if (channel == NULL)
    {
        return NULL;
    }

    for (j = BDMA_NODEJOB(BDMABase->bdb_Queue.mlh_Head); 
        j->bj_Node.mln_Succ != NULL; 
        j = BDMA_NODEJOB(j->bj_Node.mln_Succ))
    {
        BOOL free = TRUE;

        if (best != NULL && j->bj_Request.bdr_Priority <= best->bj_Request.bdr_Priority)
        {
            continue;
        }

        for (i = 0; i < BDMABase->bdb_Channels && free; i++)
        {
            struct BDMAJob *run = BDMABase->bdb_Channel[i].bc_Job;

            if (run != NULL && Conflict(j, run))
            {
                free = FALSE;
            }
        }

        /* the queue is in the order of submission: the older jobs are in front of this one */
        for (o = BDMA_NODEJOB(BDMABase->bdb_Queue.mlh_Head); 
             o != j && free; 
             o = BDMA_NODEJOB(o->bj_Node.mln_Succ))
        {
            if (Conflict(j, o))
            {
                free = FALSE;
            }
        }

        if (free)
        {
            best = j;
        }
    }

    if (best == NULL)
    {
        return NULL;
    }

    Remove(BDMA_JOBNODE(best));
    best->bj_State = BJS_RUNNING;   /* the time starts when StartChain() arms the channel, not here: until then the job is bj_Starting */
    best->bj_Starting = 1;
    best->bj_AbortReq = 0;
    best->bj_Channel = channel - BDMABase->bdb_Channel;
    channel->bc_Job = best;
    *picked = best;

    return channel;
}

/* Starts what can start on the idle channels.
   For a task: the chains are built with the interrupts on.
*/
void BDMA_Run(
    struct BDMABase *BDMABase)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;

    for (;;)
    {
        struct BDMAChannel *channel;
        struct BDMAJob *job = NULL;

        Disable();
        channel = Pick(BDMABase, &job);
        Enable();

        if (channel == NULL)
        {
            return;
        }

        StartChain(BDMABase, channel, job);
    }
}

/* The watchdog: a vertical blank server, 50 or 60 times a second.
   A slice that has been on its channel longer than the BDJ_Timeout
   of its job is given up: the channel is stopped, the job is replied
   with BDERR_TIMEOUT and what waited for the channel goes on.
   The resolution is a tick. (The time is read with the interrupts off,
   so that a slice that starts meanwhile is not taken for a late one.)
*/
static ULONG Tick(
    REGARG(struct BDMABase *BDMABase, "a1"))
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    BOOL again = FALSE;
    ULONG i;

    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        struct BDMAChannel *channel = &BDMABase->bdb_Channel[i];
        struct BDMAJob *job;

        Disable();
        job = channel->bc_Job;

        if (job != NULL && job->bj_State == BJS_RUNNING && !job->bj_Starting &&
            !job->bj_Test && job->bj_Request.bdr_Timeout != 0 &&
            timer_now() - job->bj_Start > job->bj_Request.bdr_Timeout)
        {
            BDMA_StopChannel(channel);
            channel->bc_Job = NULL;
            job->bj_State = BJS_FAILED;
            job->bj_Error = BDERR_TIMEOUT;
            job->bj_Done = 0;
            BDMABase->bdb_Failures++;
            Enable();

            BDMA_ReplyJob(BDMABase, job);
            again = TRUE;
        }
        else
        {
            Enable();
        }
    }

    if (again)
    {
        BDMA_Run(BDMABase);
    }

    return 0;
}

/* A job that has slices left goes back to the queue where its order of submission puts it:
   the queue is in that order, which is what lets a job see the older ones that it must wait
   for (footprints). Called by the interrupt.
*/
static VOID Requeue(
    struct BDMABase *BDMABase, 
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct BDMAJob *o;

    job->bj_State = BJS_QUEUED;

    for (o = BDMA_NODEJOB(BDMABase->bdb_Queue.mlh_Head); 
         o->bj_Node.mln_Succ != NULL; 
         o = BDMA_NODEJOB(o->bj_Node.mln_Succ))
    {
        if (o->bj_Sequence > job->bj_Sequence)
        {
            break;
        }
    }

    Insert((struct List *)&BDMABase->bdb_Queue, 
        BDMA_JOBNODE(job), (struct Node *)o->bj_Node.mln_Pred);
}

/* The end of a job: statistics,
   then the software interrupt takes over (cache, reply to the port)
*/
static VOID Finish(
    struct BDMABase *BDMABase, 
    struct BDMAJob *job, 
    LONG error)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    const struct BDMARequest *r = &job->bj_Request;

    job->bj_Error = error;
    job->bj_State = error == BDERR_OK ? BJS_DONE : BJS_FAILED;

    if (!job->bj_Test)
    {
        BOOL cached;

        if (error == BDERR_OK)
        {
            BDMABase->bdb_Jobs++;
            BDMABase->bdb_ByteRemainder += r->bdr_Length * r->bdr_Rows;
            BDMABase->bdb_MegaBytes += BDMABase->bdb_ByteRemainder >> 20;
            BDMABase->bdb_ByteRemainder &= (1UL << 20) - 1;
        }
        else
        {
            BDMABase->bdb_Failures++;
        }

        job->bj_Done = error == BDERR_OK ? r->bdr_Length * r->bdr_Rows : 0;
        cached = error == BDERR_OK && !(r->bdr_Given & BDRF_NOCACHE) && !BDMA_InVideoMemory(BDMABase, job->bj_WriteLo, job->bj_WriteHi);

        if (cached && job->bj_WriteHi - job->bj_WriteLo > BDMA_LINEWISE_MAX)
        {
            /* a big job: the call of the system for the cache is not for an interrupt,
               a software interrupt does it and replies. (It is not used for a small job:
               Cause() costs about 60 us under Emu68, the lines of a small area less than one.) */
            AddTail((struct List *)&BDMABase->bdb_Done, BDMA_JOBNODE(job));
            Cause(&BDMABase->bdb_DoneInt);
        }
        else
        {
            if (cached)
            {
                BDMA_DropLines(job->bj_WriteLo, job->bj_WriteHi - job->bj_WriteLo);
            }

            BDMA_ReplyJob(BDMABase, job);
        }
    }
}

/* Called by gic400.library at the interrupt of a channel, as a subroutine, with the 
   channel in a1 (the is_Data of the interrupt): it acknowledges (level triggered: an 
   interrupt that is not acknowledged is a storm), ends the job, and starts the next ones.
*/
static ULONG Interrupt(
    REGARG(struct BDMAChannel *channel, "a1"))
{
    struct BDMABase *BDMABase = channel->bc_Base;
    struct BDMAJob *job;
    ULONG cs = dma_rd(channel->bc_Number, DMA_CS);

    channel->bc_Calls++;

    if ((cs & CS_INT) == 0)
    {
        return 0;
    }

    /* writing 1 clears them */
    dma_wr(channel->bc_Number, DMA_CS, CS_INT | CS_END | CS_PROT);

    job = channel->bc_Job;
    channel->bc_Job = NULL;

    if (job != NULL)
    {
        if (!(cs & CS_ERROR) && job->bj_Unit < job->bj_Units)
        {
            /* another slice: the job waits its turn again, a more urgent one may go first */
            Requeue(BDMABase, job);
        }
        else
        {
            Finish(BDMABase, job, (cs & CS_ERROR) ? BDERR_HW : BDERR_OK);
        }
    }

    /* a job that waited for this one, or for the channel */
    for (;;)
    {
        struct BDMAChannel *idle;
        struct BDMAJob *next = NULL;

        idle = Pick(BDMABase, &next);

        if (idle == NULL)
        {
            break;
        }

        StartChain(BDMABase, idle, next);
    }

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
    job.bj_State = BJS_RUNNING;
    Enable();

    StartChain(BDMABase, channel, &job);

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

        BDMA_StopChannel(channel);
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

/* What the device tree says about the DMA of the machine: the model, 
   the channels and the interrupts. FALSE: no engine here.
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
            BDMABase->bdb_Model = BDM_BCM2711;

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
    else
    {
        /* the family of the Zero, Pi 2 and Pi 3: known, but no backend yet */
        key = DT_OpenKey("/soc/dma-controller@7e007000");
        if (key != NULL)
        {
            BDMABase->bdb_Model = BDM_BCM2835;
            DT_CloseKey(key);
        }
    }

    return found;
}

/* Starts the engine. Called by a task, under the semaphore of the base, at the first call that needs it.
   Everything is checked and the engine stays unavailable (the clients stay on the CPU) when something is 
   missing: the model, a free channel that is not the firmware's, its interrupt, gic400.library 1.3 or later, 
   memory for the control blocks, and a 4 KB copy that must arrive intact and raise exactly the interrupt of its channel.
*/
BOOL BDMA_StartEngine(
    struct BDMABase *BDMABase)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    APTR DeviceTreeBase;
    struct Library *MailboxBase;
    struct Library *GIC400_Base;
    ULONG interrupts[LAST_40BIT_CHANNEL - FIRST_40BIT_CHANNEL + 1] = { 0, 0, 0, 0 };
    ULONG mask, size, bus, phys, firmware, number, i;
    UBYTE *scratch;

    if (BDMABase->bdb_State != BES_NOTSTARTED)
    {
        return BDMABase->bdb_State == BES_RUNNING;
    }

    BDMABase->bdb_State = BES_FAILED;

    BDMABase->bdb_DeviceTreeBase = OpenResource("devicetree.resource");
    BDMABase->bdb_MailboxBase = (struct Library *)OpenResource(MAILBOXNAME);
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
    size = BDMABase->bdb_Channels * (BDMA_CHAIN_SIZE + CONSTANT_SIZE) + 2 * SELFTEST_SIZE;
    
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

    /* NewList() */
    BDMABase->bdb_Queue.mlh_Head = (struct MinNode *)&BDMABase->bdb_Queue.mlh_Tail;
    BDMABase->bdb_Queue.mlh_Tail = NULL;
    BDMABase->bdb_Queue.mlh_TailPred = (struct MinNode *)&BDMABase->bdb_Queue.mlh_Head;

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

        BDMA_StopChannel(channel);

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
    BDMABase->bdb_State = BES_RUNNING;
    BDMABase->bdb_Classes = BDCLASS_DMA40; /* every channel it manages is a 40 bit one: the other classes come with their backend */

    /* the watchdog of BDJ_Timeout */
    BDMABase->bdb_TickInt.is_Node.ln_Type = NT_INTERRUPT;
    BDMABase->bdb_TickInt.is_Node.ln_Pri = 0;
    BDMABase->bdb_TickInt.is_Node.ln_Name = (char *)"brcm-dma.watchdog";
    BDMABase->bdb_TickInt.is_Data = BDMABase;
    BDMABase->bdb_TickInt.is_Code = (void (*)())Tick;
    AddIntServer(INTB_VERTB, &BDMABase->bdb_TickInt);

    return TRUE;
}
