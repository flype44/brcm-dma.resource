/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/


/*
    The common engine: the queue of the jobs, the scheduler (priorities, order of submission, footprints), the slices of big jobs,
    the watchdog and the end of a job. It knows jobs and channels, not registers: the hardware is behind the backend interface of
    brcm-dma.h (struct BDMABackend), one per SoC family: vc6.c for the BCM2711, vc4.c for the BCM2835 family (not written).

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

#include <common/compiler.h>

#include "brcm-dma.h"
#include "hw-vc4.h"     /* timer_now(): the free running system timer of the BCM2835 family, the clock of the timeouts */
#include "mbox.h"
#include "cache.h"

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

/* What an interrupt of a channel cost the CPU, in the cycles and the 68k instructions that the counters of Emu68 gave for it: kept as megacycles and
   kilo instructions with the rest, as the megabytes are */
VOID BDMA_AccountIrq(
    struct BDMABase *BDMABase,
    ULONG cycles,
    ULONG instructions)
{
    BDMABase->bdb_IrqCalls++;

    BDMABase->bdb_IrqCycles += cycles;
    BDMABase->bdb_IrqMCycles += BDMABase->bdb_IrqCycles >> 20;
    BDMABase->bdb_IrqCycles &= (1UL << 20) - 1;

    BDMABase->bdb_IrqInstr += instructions;
    BDMABase->bdb_IrqKInstr += BDMABase->bdb_IrqInstr >> 10;
    BDMABase->bdb_IrqInstr &= (1UL << 10) - 1;
}

/* The time a slice kept its channel busy: milliseconds and the microseconds that do not make one yet */
static VOID AccountBusy(
    struct BDMAChannel *channel)
{
    channel->bc_BusyUs += timer_now() - channel->bc_Start;

    if (channel->bc_BusyUs >= 1000)
    {
        channel->bc_BusyMs += channel->bc_BusyUs / 1000;
        channel->bc_BusyUs %= 1000;
    }
}

/* May a second slice of this job run while the first one does? Only when what it reads and what it writes are apart:
   with an overlap (a scroll inside a bitmap) the order of the rows is the contract.
*/
static BOOL Splittable(
    const struct BDMAJob *j)
{
    return !Meets(j->bj_ReadLo, j->bj_ReadHi, j->bj_WriteLo, j->bj_WriteHi);
}

/* Is a slice of this job being armed? (its starter builds the next chain, which advances the units: one at a time) */
static BOOL Arming(
    struct BDMABase *BDMABase,
    const struct BDMAJob *j)
{
    ULONG i;

    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        if (BDMABase->bdb_Channel[i].bc_Job == j && BDMABase->bdb_Channel[i].bc_Starting)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/* May this job go on a channel now? It conflicts with no job that runs on a channel (a slice of the job itself does not count)
   and with no OLDER job still in the queue (a job never overtakes an older one it conflicts with: the result is that of
   the jobs run one by one).
*/
static BOOL Free(
    struct BDMABase *BDMABase,
    struct BDMAJob *j)
{
    struct BDMAJob *o;
    ULONG i;

    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        struct BDMAJob *run = BDMABase->bdb_Channel[i].bc_Job;

        if (run != NULL && run != j && Conflict(j, run))
        {
            return FALSE;
        }
    }

    /* the queue is in the order of submission: the older jobs are in front of this one */
    for (o = BDMA_NODEJOB(BDMABase->bdb_Queue.mlh_Head); o != j; o = BDMA_NODEJOB(o->bj_Node.mln_Succ))
    {
        if (Conflict(j, o))
        {
            return FALSE;
        }
    }

    return TRUE;
}

/* Picks the slice that may start now on an idle channel and hands the channel to it.
   First choice: the job that has none on a channel, the best priority and then the order of submission. Only when no such job
   can start, a job that is already running and can be split gets a second slice on the idle channel (the opportunistic use of
   the second channel: nothing else waits for it).
   Called with the interrupts off (Disable(), or from the interrupt). The channel is taken; the caller then calls
   BDMA_StartOnChannel() with the interrupts on if it can.
*/
static struct BDMAChannel *Pick(
    struct BDMABase *BDMABase,
    struct BDMAJob **picked)
{
    struct BDMAChannel *channel = NULL;
    struct BDMAJob *best = NULL, *j;
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
        if (j->bj_Active != 0 || (best != NULL && j->bj_Request.bdr_Priority <= best->bj_Request.bdr_Priority))
        {
            continue;
        }

        if (Free(BDMABase, j))
        {
            best = j;
        }
        else if (!j->bj_Deferred)
        {
            j->bj_Deferred = 1;
            BDMABase->bdb_Deferred++;
        }
    }

    if (best == NULL)
    {
        for (j = BDMA_NODEJOB(BDMABase->bdb_Queue.mlh_Head);
            j->bj_Node.mln_Succ != NULL;
            j = BDMA_NODEJOB(j->bj_Node.mln_Succ))
        {
            if (j->bj_Active != 0 && j->bj_Active < BDMABase->bdb_Channels && j->bj_Stop == 0 &&
                j->bj_Unit < j->bj_Units && Splittable(j) && !Arming(BDMABase, j) && Free(BDMABase, j))
            {
                best = j;
                break;
            }
        }
    }

    if (best == NULL)
    {
        return NULL;
    }

    if (best->bj_Active == 0)
    {
        /* the first slice of this stay in the queue */
        BDMABase->bdb_Waiting--;

        if (timer_now() - best->bj_Queued > BDMABase->bdb_WaitMax)
        {
            BDMABase->bdb_WaitMax = timer_now() - best->bj_Queued;
        }

        best->bj_State = BJS_RUNNING;
    }

    best->bj_Active++;
    channel->bc_Starting = 1;    /* the time starts when the channel is armed, not here */
    channel->bc_Job = best;
    *picked = best;

    return channel;
}

/* The job ends early (abort, timeout, hardware error): no more slices go to a channel, the ones on a channel are stopped,
   except a slice that is being armed, whose starter ends the job. Called with the interrupts off. TRUE when nothing is left
   of the job on a channel: it is settled (state, error and counters), and the caller replies it.
*/
BOOL BDMA_KillJob(
    struct BDMABase *BDMABase,
    struct BDMAJob *job,
    LONG error)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    ULONG i;

    if (job->bj_Stop == 0)
    {
        job->bj_Stop = error;
    }

    if (job->bj_InQueue)
    {
        Remove(BDMA_JOBNODE(job));
        job->bj_InQueue = 0;

        if (job->bj_Active == 0)
        {
            BDMABase->bdb_Waiting--;
        }
    }

    for (i = 0; i < BDMABase->bdb_Channels; i++)
    {
        struct BDMAChannel *channel = &BDMABase->bdb_Channel[i];

        if (channel->bc_Job == job && !channel->bc_Starting)
        {
            AccountBusy(channel);
            BDMABase->bdb_Backend->Stop(channel);
            channel->bc_Job = NULL;
            job->bj_Active--;
        }
    }

    if (job->bj_Active != 0)
    {
        return FALSE;
    }

    job->bj_Error = job->bj_Stop;
    job->bj_Done = 0;

    if (job->bj_Stop == BDERR_ABORTED)
    {
        job->bj_State = BJS_ABORTED;
        BDMABase->bdb_Aborts++;
    }
    else
    {
        job->bj_State = BJS_FAILED;
        BDMABase->bdb_Failures++;

        if (job->bj_Stop == BDERR_TIMEOUT)
        {
            BDMABase->bdb_Timeouts++;
        }
    }

    return TRUE;
}

/* Builds the chain of the next slice of the job on its channel, then arms the channel. The channel has belonged to the job
   since Pick(), but the task that builds can be preempted meanwhile and the job stopped: the state is looked at and the
   channel started in one breath, with the interrupts off.
*/
VOID BDMA_StartOnChannel(
    struct BDMABase *BDMABase,
    struct BDMAChannel *channel,
    struct BDMAJob *job)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;

    BDMABase->bdb_Backend->Build(BDMABase, channel, job);

    Disable();
    channel->bc_Starting = 0;

    if (job->bj_Stop != 0)
    {
        BOOL settled;

        channel->bc_Job = NULL;
        job->bj_Active--;
        settled = BDMA_KillJob(BDMABase, job, job->bj_Stop);
        Enable();

        if (settled)
        {
            BDMA_ReplyJob(BDMABase, job);
        }

        return;
    }

    channel->bc_Start = timer_now();
    BDMABase->bdb_Backend->Arm(channel);

    /* every unit is on a channel: nothing is left to give */
    if (job->bj_Unit >= job->bj_Units && job->bj_InQueue)
    {
        Remove(BDMA_JOBNODE(job));
        job->bj_InQueue = 0;
    }

    Enable();
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

        BDMA_StartOnChannel(BDMABase, channel, job);
    }
}

/* The watchdog: a vertical blank server, 50 or 60 times a second.
   A slice that has been on its channel longer than the BDJ_Timeout
   of its job is given up: the job is stopped (its other slice too),
   replied with BDERR_TIMEOUT and what waited for the channel goes on.
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

        if (job != NULL && !channel->bc_Starting && !job->bj_Test && job->bj_Request.bdr_Timeout != 0 &&
            timer_now() - channel->bc_Start > job->bj_Request.bdr_Timeout)
        {
            BOOL settled = BDMA_KillJob(BDMABase, job, BDERR_TIMEOUT);

            Enable();

            if (settled)
            {
                BDMA_ReplyJob(BDMABase, job);
            }

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
    ULONG bytes;

    job->bj_Error = error;
    job->bj_State = error == BDERR_OK ? BJS_DONE : BJS_FAILED;

    if (!job->bj_Test)
    {
        BOOL cached;

        if (error == BDERR_OK)
        {
            BDMABase->bdb_Jobs++;
            bytes = r->bdr_Length * r->bdr_Rows;
            BDMABase->bdb_Size[bytes <= 64 ? 0 : bytes <= 4096 ? 1 : bytes <= 32768 ? 2 : bytes <= 1048576 ? 3 : 4]++;
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

/* The backend tells that the slice on the channel is over (error: BDERR_OK or BDERR_HW). Called by the interrupt of the channel.
   The job goes on if it has slices left (it waits its turn again when none of its slices is on a channel, so that a more urgent
   job may go first), or ends when the last one is over; then the next slices that can start do.
*/
VOID BDMA_ChannelEnded(
    struct BDMABase *BDMABase,
    struct BDMAChannel *channel,
    LONG error)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    struct BDMAJob *job = channel->bc_Job;

    channel->bc_Job = NULL;

    if (job != NULL)
    {
        AccountBusy(channel);
        job->bj_Active--;

        if (job->bj_Test)
        {
            Finish(BDMABase, job, error);
        }
        else if (error != BDERR_OK)
        {
            /* the other slice of the job, if any, is stopped with it */
            BOOL settled;

            Disable();
            settled = BDMA_KillJob(BDMABase, job, error);
            Enable();

            if (settled)
            {
                BDMA_ReplyJob(BDMABase, job);
            }
        }
        else if (job->bj_Unit < job->bj_Units)
        {
            BDMABase->bdb_Slices++;

            if (job->bj_Active == 0)
            {
                /* the job waits its turn again (it is still in the queue, in its place) */
                job->bj_State = BJS_QUEUED;
                BDMA_ENQUEUED(BDMABase, job);
            }
        }
        else if (job->bj_Active == 0)
        {
            Finish(BDMABase, job, BDERR_OK);
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

        BDMA_StartOnChannel(BDMABase, idle, next);
    }
}

/* Which backend drives the DMA of this machine, from the revision code of the board that the firmware gives
   (NOSQ uuWu FMMM CCCC PPPP TTTT TTTT RRRR: with the bit F set, PPPP names the SoC: 0 BCM2835, 1 BCM2836, 2 BCM2837, 3 BCM2711,
   4 BCM2712; without it the code is one of the first boards, all BCM2835). Sets bdb_Model. NULL: no backend for this SoC.
   The device tree is left to the backend, for the channels and the interrupts.
*/
static const struct BDMABackend *DetectBackend(
    ULONG revision,
    struct BDMABase *BDMABase)
{
    ULONG soc = (revision >> 12) & 15;

    if ((revision & (1UL << 23)) == 0 || soc <= 2)
    {
        BDMABase->bdb_Model = BDM_BCM2835;
        return &BDMA_VC4Backend;
    }

    if (soc == 3)
    {
        BDMABase->bdb_Model = BDM_BCM2711;
        return &BDMA_VC6Backend;
    }

    return NULL;
}

/* Starts the engine. Called by a task, under the semaphore of the base, at the first call that needs it.
   The hardware is the backend's: when it says no, the engine stays unavailable and the clients stay on the CPU.
*/
BOOL BDMA_StartEngine(
    struct BDMABase *BDMABase)
{
    struct ExecBase *SysBase = BDMABase->bdb_ExecBase;
    ULONG revision;

    if (BDMABase->bdb_State != BES_NOTSTARTED)
    {
        return BDMABase->bdb_State == BES_RUNNING;
    }

    BDMABase->bdb_State = BES_FAILED;

    /* NewList(): before the first interrupt of a channel can look at it */
    BDMABase->bdb_Queue.mlh_Head = (struct MinNode *)&BDMABase->bdb_Queue.mlh_Tail;
    BDMABase->bdb_Queue.mlh_Tail = NULL;
    BDMABase->bdb_Queue.mlh_TailPred = (struct MinNode *)&BDMABase->bdb_Queue.mlh_Head;

    BDMABase->bdb_MailboxBase = (struct Library *)OpenResource(MAILBOXNAME);
    if (BDMABase->bdb_MailboxBase == NULL)
    {
        BDMABase->bdb_Reason = BDW_RESOURCES;
        return FALSE;
    }

    revision = GetBoardRevision(BDMABase->bdb_MailboxBase);
    BDMABase->bdb_Backend = revision != 0 ? DetectBackend(revision, BDMABase) : NULL;
    if (BDMABase->bdb_Backend == NULL)
    {
        BDMABase->bdb_Reason = revision != 0 ? BDW_NOTIMPLEMENTED : BDW_RESOURCES;
        return FALSE;
    }

    if (!BDMABase->bdb_Backend->Start(BDMABase))
    {
        return FALSE;
    }

    BDMABase->bdb_State = BES_RUNNING;

    /* the watchdog of BDJ_Timeout */
    BDMABase->bdb_TickInt.is_Node.ln_Type = NT_INTERRUPT;
    BDMABase->bdb_TickInt.is_Node.ln_Pri = 0;
    BDMABase->bdb_TickInt.is_Node.ln_Name = (char *)"brcm-dma.watchdog";
    BDMABase->bdb_TickInt.is_Data = BDMABase;
    BDMABase->bdb_TickInt.is_Code = (void (*)())Tick;
    AddIntServer(INTB_VERTB, &BDMABase->bdb_TickInt);

    return TRUE;
}
