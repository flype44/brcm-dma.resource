/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    copymemdma -- Exec CopyMem() and CopyMemQuick() through brcm-dma.resource (PiStorm / Emu68), and the way back.

      copymemdma CM/S CMQ/S THRESHOLD/N REMOVE/S

      CM          patch CopyMem()
      CMQ         patch CopyMemQuick()
      THRESHOLD   the smallest copy, in bytes, that goes to the DMA (at least 1, default 1048576). Smaller copies are done by the routine that
                  was there before: the patch costs them a comparison.
      REMOVE      take the patch off (the DMA copies in flight are waited for)

    At least one of CM and CMQ is needed to install. Started again while it is installed, the program says so and leaves (return code 5).
    From Workbench there are no arguments: both functions are patched, with the default threshold.

    The program detaches itself (the process keeps its own seglist, as a daemon does) and the shell comes back. The state is a public
    semaphore named "copymemdma" whose structure starts with the tag 'BDMA'; a second run, REMOVE included, finds the patch through it.

    A copy goes to the DMA only when it can: the size reaches the threshold, the caller is a task that may sleep (no Forbid, no Disable, not
    an interrupt: told by the stack pointer, which is then outside the stack of the task), the blocks do not overlap, and there is stack left
    for the calls into the resource.
    The memory is the business of the resource: it refuses a job in memory that the DMA of the Pi does not reach (the Chip RAM, the bus of
    the Amiga, the vector table...) and the copy is then done by the old routine. This program has no list of its own: it needs a
    brcm-dma.resource that does that, and must not be run with an older one.

    The removal puts the old vectors back only if they still point to this patch. If another program patched the function since, the vector
    cannot be taken out of the chain: the patch then stays resident, passing every call on, and REMOVE can be tried again later.

    Nothing here may call CopyMem() or CopyMemQuick() between the patch and the DMA job without going back to the old routine: the resource
    does not use them (its own calls are in a task context, with the stack of the caller).
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/dos.h>
#include <proto/brcm-dma.h>

#include <resources/brcm-dma.h>

#include "common/compiler.h"

#ifndef VERSION_STRING
#define VERSION_STRING      "$VER: copymemdma 0.1 (unknown date)"
#endif

#define CMD_NAME            "copymemdma"
#define CMD_TAG             0x42444D41UL        /* 'BDMA' */
#define CMD_VERSION         2UL                 /* the layout of struct State: raise it whenever a field is added or moved */

#define OFFSET_COPYMEM      (-624)              /* _LVOCopyMem */
#define OFFSET_COPYMEMQUICK (-630)              /* _LVOCopyMemQuick */

#define DEFAULT_THRESHOLD   1048576UL
#define MIN_THRESHOLD       16384UL             /* below this, THRESHOLD needs FORCE */
#define STACK_MARGIN        1024UL              /* bytes of stack that must be left below the caller; to be set from brcm-dma-stack */

#define INSIDE              8                   /* tasks shown by STATUS */

#define REQ_NONE            0
#define REQ_REMOVE          1

#define L_PENDING           0
#define L_OK                1
#define L_FAILED            2

#define WHY_CLIENT          1                   /* the resource gave no client */
#define WHY_SIGNAL          2                   /* no signal bit left */
#define WHY_EXISTS          3                   /* installed by someone else in the meantime */

struct State {
    struct SignalSemaphore  sem;                /* first: FindSemaphore() answers a pointer to the state */
    ULONG                   tag;
    ULONG                   version;
    struct Task            *task;               /* the daemon */
    LONG                    sigbit;
    ULONG                   sigmask;            /* wakes the daemon */
    volatile ULONG          request;            /* REQ_*: set by the one who removes, cleared by the daemon */
    volatile ULONG          stuck;              /* a vector could not be put back */
    ULONG                   threshold;          /* for the report; the stub reads CopyThreshold */
    volatile ULONG          cmPatched;          /* the vector is ours */
    volatile ULONG          cmqPatched;
    volatile ULONG          cmOn;               /* the patch may use the DMA */
    volatile ULONG          cmqOn;
    volatile LONG           inflight;           /* calls inside the DMA path */
    ULONG                   dma;                /* copies done by the DMA */
    ULONG                   fallback;           /* copies that were given back to the old routine after the job was refused or failed */
    ULONG                   skipped;            /* copies over the threshold that the CPU did, by the rules (context, memory): a statistic, not locked */
    struct Task            *insideTask[INSIDE]; /* who is in the DMA path now, to see who is stuck */
    ULONG                   insideSize[INSIDE];
    char                    insideName[INSIDE][32];
};

/* For the Version command: it looks for "$VER:" in the file */
static const char __attribute__((used)) Version[] = VERSION_STRING;

struct DosLibrary *DOSBase;
struct ExecBase *SysBase;
APTR BrcmDmaBase;
struct BDMAClient *Client;

/* The state lives in the segment of the program, which stays loaded as long as the daemon runs */
static struct State S;

/* What the program that is started gives to the one it detaches (the globals are shared: it is the same seglist) */
static ULONG LaunchCM, LaunchCMQ, LaunchThreshold;
static BOOL IsChild;
static volatile LONG LaunchStatus, LaunchWhy, LaunchAck;

/* What the assembler stubs below read, so they are plain variables: the threshold, and the routines that were there before (the ROM's
   or another patch). */
ULONG CopyThreshold;
APTR OldCopyMem;
APTR OldCopyMemQuick;

static APTR Vector(
    LONG offset)
{
    return *(APTR *)((UBYTE *)SysBase + offset + 2);          /* jmp abs.l: the address follows the opcode */
}

static void CopyName(
    char *to,
    const char *from)
{
    ULONG i;

    if (from == NULL)
        from = "?";
    for (i = 0; i < 31 && from[i] != 0; i++)
        to[i] = from[i];
    to[i] = 0;
}

/* The decision and the job, in C. TRUE: the copy is done. FALSE: the stub gives the copy to the routine that was there before; this
   function never calls it, so that no call of the old routine ever returns into this program (which may be unloaded meanwhile). */
ULONG __attribute__((used)) TryCopy(
    ULONG which,
    CONST APTR src,
    APTR dst,
    ULONG size)
{
    volatile ULONG *on = which ? &S.cmqOn : &S.cmOn;
    struct Task *task;
    ULONG here, s = (ULONG)src, d = (ULONG)dst, slot;
    BOOL ok, done = FALSE;
    struct BDMAJob *job;

    /* a task that may sleep, with stack left; the stack pointer of an interrupt is not in the stack of the task */
    task = SysBase->ThisTask;
    if (SysBase->IDNestCnt >= 0 || SysBase->TDNestCnt >= 0 ||
        (ULONG)&here < (ULONG)task->tc_SPLower + STACK_MARGIN || (ULONG)&here >= (ULONG)task->tc_SPUpper ||
        (s < d + size && d < s + size))
    {
        S.skipped++;
        return FALSE;
    }

    /* seen under Forbid(): once the patch is switched off nobody comes in, and the calls that did are counted */
    slot = INSIDE;
    Forbid();
    ok = *on != 0;
    if (ok)
    {
        S.inflight++;
        for (slot = 0; slot < INSIDE && S.insideTask[slot] != NULL; slot++)
            ;
        if (slot < INSIDE)
        {
            S.insideTask[slot] = task;
            S.insideSize[slot] = size;
            CopyName(S.insideName[slot], task->tc_Node.ln_Name);
        }
    }
    Permit();

    if (!ok)
        return FALSE;

    job = BDMA_AddJobTags(
        Client,
        BDJ_Src, s,
        BDJ_Dst, d,
        BDJ_Length, size,
        TAG_DONE);

    if (job != NULL)
        done = BDMA_WaitJob(job) == BDERR_OK;

    Forbid();
    S.inflight--;
    if (slot < INSIDE)
        S.insideTask[slot] = NULL;
    if (done)
        S.dma++;
    else
        S.fallback++;
    Permit();

    return done;
}

/* The two entries of the patch, as Exec calls them: A0 = source, A1 = destination, D0 = size. Under the threshold the stub is one
   comparison, then it jumps to the old routine by pushing its address and doing an RTS: a tail jump, so the old routine returns to
   the caller and never to this program. Above it, TryCopy() decides; when it says no, the same jump follows. D1 is scratch for Exec. */
extern void PatchCopyMem(void);
extern void PatchCopyMemQuick(void);

__asm__(
    "       .text\n"
    "       .even\n"
    "       .globl  _PatchCopyMem\n"
    "_PatchCopyMem:\n"
    "       cmpl    _CopyThreshold,d0\n"
    "       jcs     1f\n"
    "       movel   d0,sp@-\n"
    "       movel   a1,sp@-\n"
    "       movel   a0,sp@-\n"
    "       clrl    sp@-\n"
    "       jsr     _TryCopy\n"
    "       addql   #4,sp\n"
    "       movel   sp@+,a0\n"
    "       movel   sp@+,a1\n"
    "       movel   sp@+,d1\n"
    "       tstl    d0\n"
    "       jne     2f\n"
    "       movel   d1,d0\n"
    "1:     movel   _OldCopyMem,sp@-\n"
    "2:     rts\n"
    "       .even\n"
    "       .globl  _PatchCopyMemQuick\n"
    "_PatchCopyMemQuick:\n"
    "       cmpl    _CopyThreshold,d0\n"
    "       jcs     1f\n"
    "       movel   d0,sp@-\n"
    "       movel   a1,sp@-\n"
    "       movel   a0,sp@-\n"
    "       movel   #1,sp@-\n"
    "       jsr     _TryCopy\n"
    "       addql   #4,sp\n"
    "       movel   sp@+,a0\n"
    "       movel   sp@+,a1\n"
    "       movel   sp@+,d1\n"
    "       tstl    d0\n"
    "       jne     2f\n"
    "       movel   d1,d0\n"
    "1:     movel   _OldCopyMemQuick,sp@-\n"
    "2:     rts\n"
);

/* In the daemon: the state, the client of the resource, then the patch. 0 or a WHY_*. */
static LONG Install(void)
{
    LONG sig;

    Client = BDMA_OpenClientTags(
        BDC_Name, (ULONG)CMD_NAME,
        TAG_DONE);
    if (Client == NULL)
        return WHY_CLIENT;

    sig = AllocSignal(-1);
    if (sig < 0)
    {
        BDMA_CloseClient(Client);
        return WHY_SIGNAL;
    }

    InitSemaphore(&S.sem);
    S.sem.ss_Link.ln_Name = (char *)CMD_NAME;
    S.sem.ss_Link.ln_Pri = 0;
    S.tag = CMD_TAG;
    S.version = CMD_VERSION;
    S.task = FindTask(NULL);
    S.sigbit = sig;
    S.sigmask = 1UL << sig;
    S.request = REQ_NONE;
    S.stuck = 0;
    S.threshold = LaunchThreshold;
    CopyThreshold = LaunchThreshold;

    Forbid();
    if (FindSemaphore((STRPTR)CMD_NAME) != NULL)
    {
        Permit();
        FreeSignal(sig);
        BDMA_CloseClient(Client);
        return WHY_EXISTS;
    }
    AddSemaphore(&S.sem);
    Permit();

    /* the old routine is stored with the interrupts off: nobody may enter the patch before it is known */
    if (LaunchCM)
    {
        Disable();
        OldCopyMem = SetFunction((struct Library *)SysBase, OFFSET_COPYMEM, (APTR)PatchCopyMem);
        Enable();
        S.cmPatched = 1;
        S.cmOn = 1;
    }
    if (LaunchCMQ)
    {
        Disable();
        OldCopyMemQuick = SetFunction((struct Library *)SysBase, OFFSET_COPYMEMQUICK, (APTR)PatchCopyMemQuick);
        Enable();
        S.cmqPatched = 1;
        S.cmqOn = 1;
    }

    return 0;
}

/* Puts the vectors back and waits for the calls that are still inside. TRUE when the patch is gone for good. */
static BOOL Unpatch(void)
{
    BOOL clean = TRUE;
    LONG n;

    S.stuck = 0;
    S.cmOn = 0;
    S.cmqOn = 0;

    Forbid();
    if (S.cmPatched)
    {
        if (Vector(OFFSET_COPYMEM) == (APTR)PatchCopyMem)
        {
            SetFunction((struct Library *)SysBase, OFFSET_COPYMEM, OldCopyMem);
            S.cmPatched = 0;
        }
        else
            clean = FALSE;
    }
    if (S.cmqPatched)
    {
        if (Vector(OFFSET_COPYMEMQUICK) == (APTR)PatchCopyMemQuick)
        {
            SetFunction((struct Library *)SysBase, OFFSET_COPYMEMQUICK, OldCopyMemQuick);
            S.cmqPatched = 0;
        }
        else
            clean = FALSE;
    }
    Permit();

    /* the calls that are inside the DMA path: those that came in before the patch was switched off */
    do
    {
        Forbid();
        n = S.inflight;
        Permit();
        if (n != 0)
            Delay(2);
    }
    while (n != 0);

    /* a caller may have read the vector before it was put back and not run its first instruction yet */
    if (clean)
        Delay(50);

    return clean;
}

/* The daemon sleeps until it is asked to remove the patch (or Ctrl-C in the foreground), then goes away if it can. */
static void Serve(void)
{
    BOOL gone = FALSE;
    ULONG got;

    while (!gone)
    {
        got = Wait(S.sigmask | SIGBREAKF_CTRL_C);

        if (got & SIGBREAKF_CTRL_C)
            S.request = REQ_REMOVE;

        if (S.request == REQ_REMOVE)
        {
            gone = Unpatch();
            if (!gone)
                S.stuck = 1;
            S.request = REQ_NONE;
        }
    }

    BDMA_CloseClient(Client);
    FreeSignal(S.sigbit);
    Forbid();
    RemSemaphore(&S.sem);
    Permit();
}

/* The detached process */
static int Child(void)
{
    LONG why = Install();

    if (why != 0)
    {
        LaunchWhy = why;
        LaunchStatus = L_FAILED;

        /* the one who started us reads this, and is in the segment that is freed when we are over: wait for it to be done */
        {
            LONG t;

            for (t = 0; t < 100 && !LaunchAck; t++)
                Delay(1);
            Delay(10);
        }
        return RETURN_ERROR;
    }

    LaunchStatus = L_OK;
    Serve();

    return RETURN_OK;
}

static void WhyText(
    LONG why)
{
    const char *text = "?";

    if (why == WHY_CLIENT)
        text = "the resource gave no client";
    else if (why == WHY_SIGNAL)
        text = "no signal bit left";
    else if (why == WHY_EXISTS)
        text = "it was installed in the meantime";

    Printf("%s: %s\n", (LONG)CMD_NAME, (LONG)text);
}

static void Announce(
    ULONG cm,
    ULONG cmq,
    ULONG threshold,
    const char *what)
{
    Printf("%s %s: %s%s%s, from %ld bytes\n",
        (LONG)CMD_NAME,
        (LONG)what,
        (LONG)(cm ? "CopyMem" : ""),
        (LONG)(cm && cmq ? " and " : ""),
        (LONG)(cmq ? "CopyMemQuick" : ""),
        (LONG)threshold);
}

/* The version of the structure of a patch that another version of this program installed, 0 when there is none: set by Peek() */
static ULONG ForeignVersion;

/* The state of a patch that is installed, copied with the tasks stopped. FALSE when there is none, the semaphore is not ours, or the
   structure is the one of another version of the program (ForeignVersion says so): its fields are not where this program looks for them. */
static BOOL Peek(
    struct State *copy)
{
    struct State *st;
    BOOL found = FALSE;

    ForeignVersion = 0;

    Forbid();
    st = (struct State *)FindSemaphore((STRPTR)CMD_NAME);
    if (st != NULL && st->tag == CMD_TAG)
    {
        if (st->version == CMD_VERSION)
        {
            /* by hand: CopyMem() is the vector that this program patches */
            {
                const UBYTE *from = (const UBYTE *)st;
                UBYTE *to = (UBYTE *)copy;
                ULONG n;

                for (n = 0; n < sizeof(struct State); n++)
                    to[n] = from[n];
            }
            found = TRUE;
        }
        else
            ForeignVersion = st->version;
    }
    Permit();

    return found;
}

/* A patch of another version is installed: it can be taken off only by that version, or by a reboot */
static int Foreign(void)
{
    Printf("%s: a patch of another version (%ld) of this program is installed; reboot, or use that version\n",
        (LONG)CMD_NAME,
        (LONG)ForeignVersion);

    return RETURN_WARN;
}

static int ShowStatus(void)
{
    struct State st;
    ULONG i;

    if (!Peek(&st))
    {
        if (ForeignVersion != 0)
            return Foreign();
        Printf("%s is not installed\n", (LONG)CMD_NAME);
        return RETURN_WARN;
    }

    Announce(st.cmPatched, st.cmqPatched, st.threshold, "is installed");
    Printf("  copies by the DMA %ld, given back to the CPU after a job %ld, done by the CPU by the rules %ld\n",
        (LONG)st.dma,
        (LONG)st.fallback,
        (LONG)st.skipped);
    Printf("  in the DMA path now: %ld%s\n", (LONG)st.inflight, (LONG)(st.stuck ? " (a vector was patched since: removal pending)" : ""));

    for (i = 0; i < INSIDE; i++)
        if (st.insideTask[i] != NULL)
        {
            Printf("    %s, %ld bytes\n", (LONG)st.insideName[i], (LONG)st.insideSize[i]);
        }

    return RETURN_OK;
}

static int RemovePatch(void)
{
    struct State before, st;
    struct State *p;
    LONG waited;

    if (!Peek(&before))
    {
        if (ForeignVersion != 0)
            return Foreign();
        Printf("%s is not installed\n", (LONG)CMD_NAME);
        return RETURN_WARN;
    }

    Forbid();
    p = (struct State *)FindSemaphore((STRPTR)CMD_NAME);
    if (p != NULL && p->tag == CMD_TAG)
    {
        p->request = REQ_REMOVE;
        Signal(p->task, p->sigmask);
    }
    Permit();

    for (waited = 0; waited < 500; waited += 5)
    {
        Delay(5);

        if (!Peek(&st))
        {
            Printf("%s removed: %ld copies by the DMA, %ld given back to the CPU\n",
                (LONG)CMD_NAME,
                (LONG)before.dma,
                (LONG)before.fallback);
            return RETURN_OK;
        }
        if (st.request == REQ_NONE && st.stuck)
        {
            Printf("%s: the vector of %s%s%s was patched by another program since; the patch stays, passing the calls on. Try again later.\n",
                (LONG)CMD_NAME,
                (LONG)(st.cmPatched ? "CopyMem" : ""),
                (LONG)(st.cmPatched && st.cmqPatched ? " and " : ""),
                (LONG)(st.cmqPatched ? "CopyMemQuick" : ""));
            return RETURN_WARN;
        }
    }

    Printf("%s: still waiting for copies in flight; REMOVE is on its way\n", (LONG)CMD_NAME);
    return RETURN_WARN;
}

/* BENCH: how much of the CPU is left to the other tasks while the system copies, with the patch or without it. A task of low priority does
   nothing but count; the task of the benchmark copies blocks of one size for half a second. When the CPU copies, the benchmark task keeps
   it and the counter hardly moves. When the DMA copies, the benchmark task sleeps and the counter runs. The ratio of the speed of the
   counter during the copies to its speed alone is the share of the CPU that was left, and the table says how many of the copies went to
   the DMA (the counters of the patch, when there is one). It costs the patch nothing: nobody else runs this code. The counter is an
   estimate: the cycles that the cache and the interrupt of the end of a job take from the counter are not seen in it. */
#define BENCH_MS            500UL               /* the length of each phase */
#define BENCH_TICKS         25L                 /* the same, in ticks of 1/50 s, for the phase of the counter alone */

struct Device *TimerBase;
static struct MsgPort *TimerPort;
static struct timerequest *TimerReq;
static ULONG TimerFreq;                         /* ticks of the E-clock in a second */
static struct State BenchState;
static volatile ULONG BenchCount, BenchStop, BenchDone;

static BOOL OpenTimer(void)
{
    TimerPort = CreateMsgPort();
    if (TimerPort == NULL)
        return FALSE;

    TimerReq = (struct timerequest *)CreateIORequest(TimerPort, sizeof(struct timerequest));
    if (TimerReq == NULL)
    {
        DeleteMsgPort(TimerPort);
        return FALSE;
    }

    if (OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK, (struct IORequest *)TimerReq, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)TimerReq);
        DeleteMsgPort(TimerPort);
        return FALSE;
    }

    TimerBase = TimerReq->tr_node.io_Device;

    return TRUE;
}

static void CloseTimer(void)
{
    CloseDevice((struct IORequest *)TimerReq);
    DeleteIORequest((struct IORequest *)TimerReq);
    DeleteMsgPort(TimerPort);
}

/* the low 32 bits of the E-clock: the difference of two readings is right when it wrapped once */
static ULONG Ticks(void)
{
    struct EClockVal ev;

    TimerFreq = ReadEClock(&ev);

    return ev.ev_lo;
}

static ULONG ElapsedMs(
    ULONG t0,
    ULONG t1)
{
    ULONG ms = (t1 - t0) / (TimerFreq / 1000UL);

    return ms == 0 ? 1 : ms;
}

/* The task of low priority */
static void CounterEntry(void)
{
    while (!BenchStop)
        BenchCount++;

    BenchDone = 1;
}

static BOOL BenchSize(
    ULONG size,
    BOOL quick)
{
    UBYTE *src, *dst;
    ULONG t0, t1, c0, c1, i, every, copies = 0, msBase, msCopy, rateBase, rateCopy, dma = 0;
    BOOL patched;

    src = AllocMem(size, MEMF_PUBLIC | MEMF_FAST);
    dst = AllocMem(size, MEMF_PUBLIC | MEMF_FAST);
    if (src == NULL || dst == NULL)
    {
        Printf("| %8ld | (no memory: skipped)\n", (LONG)size);
        if (src != NULL)
            FreeMem(src, size);
        if (dst != NULL)
            FreeMem(dst, size);
        return TRUE;
    }

    for (i = 0; i < size; i++)
        src[i] = (UBYTE)i;

    /* the counter alone */
    c0 = BenchCount;
    t0 = Ticks();
    Delay(BENCH_TICKS);
    t1 = Ticks();
    c1 = BenchCount;
    msBase = ElapsedMs(t0, t1);
    rateBase = (c1 - c0) / msBase;

    /* the copies; the clock is read every few copies when they are short */
    patched = Peek(&BenchState);
    if (patched)
        dma = BenchState.dma;
    every = size >= 65536UL ? 1 : 65536UL / size;
    c0 = BenchCount;
    t0 = Ticks();
    do
    {
        for (i = 0; i < every; i++)
        {
            if (quick)
                CopyMemQuick(src, dst, size);
            else
                CopyMem(src, dst, size);
        }
        copies += every;
        t1 = Ticks();
        msCopy = ElapsedMs(t0, t1);
    }
    while (msCopy < BENCH_MS);
    c1 = BenchCount;
    rateCopy = (c1 - c0) / msCopy;

    if (patched && Peek(&BenchState))
        dma = BenchState.dma - dma;

    Printf("| %8ld | %8ld | %9ld | %4ld.%ld %% | %9ld |\n",
        (LONG)size,
        (LONG)copies,
        (LONG)(((copies * (size >> 10)) * 1000UL / msCopy) / 1024UL),
        (LONG)(rateBase ? rateCopy * 1000UL / rateBase / 10 : 0),
        (LONG)(rateBase ? (rateCopy * 1000UL / rateBase) % 10 : 0),
        (LONG)dma);

    FreeMem(src, size);
    FreeMem(dst, size);

    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) == 0;
}

static int RunBench(
    BOOL quick,
    ULONG only)
{
    static const ULONG sizes[] = { 4096, 16384, 65536, 262144, 1048576, 4194304 };
    struct State seen;
    struct Process *counter;
    ULONG i;
    int rc = RETURN_OK;

    if (!OpenTimer())
    {
        Printf("%s: no timer.device\n", (LONG)CMD_NAME);
        return RETURN_ERROR;
    }

    BenchStop = 0;
    BenchDone = 0;
    counter = CreateNewProcTags(
        NP_Entry, (ULONG)CounterEntry,
        NP_Name, (ULONG)"copymemdma counter",
        NP_Priority, (ULONG)-5,
        NP_StackSize, 2048,
        TAG_DONE);
    if (counter == NULL)
    {
        Printf("%s: cannot create the counter task\n", (LONG)CMD_NAME);
        CloseTimer();
        return RETURN_ERROR;
    }

    Printf("%s\n", (LONG)(Version + 6));
    if (Peek(&seen))
        Announce(seen.cmPatched, seen.cmqPatched, seen.threshold, "is installed");
    else
        Printf("%s: no patch installed: the CPU does the copies\n", (LONG)CMD_NAME);
    Printf("Copies with %s, %ld ms for each size. CPU free = share of the CPU left to a task of lower priority.\n\n",
        (LONG)(quick ? "CopyMemQuick()" : "CopyMem()"),
        (LONG)BENCH_MS);
    Printf("|     size |   copies |      MB/s | CPU free |  by DMA   |\n");
    Printf("+----------+----------+-----------+----------+-----------+\n");

    if (only != 0)
        BenchSize(only, quick);
    else
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if (!BenchSize(sizes[i], quick))
            {
                Printf("(stopped)\n");
                rc = RETURN_WARN;
                break;
            }

    /* the counter task runs code of this program: wait for it to be over before leaving */
    BenchStop = 1;
    while (!BenchDone)
        Delay(1);
    Delay(5);
    CloseTimer();

    return rc;
}

int main(int argc, struct WBStartup *wbmsg)
{
    LONG args[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    struct State seen;
    struct CommandLineInterface *cli;
    struct Process *proc;
    ULONG threshold = DEFAULT_THRESHOLD;
    LONG waited;
    int rc = RETURN_OK;
    BOOL cm = FALSE, cmq = FALSE, takeoff = FALSE, status = FALSE, force = FALSE, bench = FALSE, quick = FALSE;
    ULONG benchsize = 0;
    LONG available = 0;

    (void)argc;

    SysBase = *(struct ExecBase **)4;

    /* the detached process: the parent opened everything, and does not close it */
    if (IsChild)
        return Child();

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return RETURN_FAIL;

    if (wbmsg != NULL)
    {
        cm = TRUE;
        cmq = TRUE;
    }
    else
    {
        rda = ReadArgs("CM/S,CMQ/S,THRESHOLD/N,REMOVE/S,STATUS/S,FORCE/S,BENCH/S,QUICK/S,SIZE/N", args, NULL);
        if (rda == NULL)
        {
            PrintFault(IoErr(), (STRPTR)CMD_NAME);
            CloseLibrary((struct Library *)DOSBase);
            return RETURN_ERROR;
        }
        cm = args[0] != 0;
        cmq = args[1] != 0;
        if (args[2] != 0)
            threshold = *(ULONG *)args[2] > 0 ? *(ULONG *)args[2] : 1;
        takeoff = args[3] != 0;
        status = args[4] != 0;
        force = args[5] != 0;
        bench = args[6] != 0;
        quick = args[7] != 0;
        if (args[8] != 0)
            benchsize = *(ULONG *)args[8];
        FreeArgs(rda);
    }

    if (bench)
    {
        rc = RunBench(quick, benchsize);
        CloseLibrary((struct Library *)DOSBase);
        return rc;
    }

    if (status)
    {
        rc = ShowStatus();
        CloseLibrary((struct Library *)DOSBase);
        return rc;
    }

    if (takeoff)
    {
        rc = RemovePatch();
        CloseLibrary((struct Library *)DOSBase);
        return rc;
    }

    if (!cm && !cmq)
    {
        Printf("%s\n", (LONG)(Version + 6));
        Printf("usage: %s CM/S CMQ/S THRESHOLD/N REMOVE/S STATUS/S FORCE/S BENCH/S QUICK/S SIZE/N  (CM and/or CMQ to install)\n", (LONG)CMD_NAME);
        CloseLibrary((struct Library *)DOSBase);
        return RETURN_ERROR;
    }

    /* A DMA job costs about 150 us whatever its size: a small threshold makes every program that copies a lot of small blocks crawl */
    if (threshold < MIN_THRESHOLD && !force)
    {
        Printf("%s: THRESHOLD under %ld bytes makes the system slow (a DMA job costs about 150 us); add FORCE to do it anyway\n",
            (LONG)CMD_NAME,
            (LONG)MIN_THRESHOLD);
        CloseLibrary((struct Library *)DOSBase);
        return RETURN_ERROR;
    }

    if (Peek(&seen))
    {
        Announce(seen.cmPatched, seen.cmqPatched, seen.threshold, "is already installed");
        CloseLibrary((struct Library *)DOSBase);
        return RETURN_WARN;
    }
    if (ForeignVersion != 0)
    {
        rc = Foreign();
        CloseLibrary((struct Library *)DOSBase);
        return rc;
    }

    /* the resource: present, and with an engine that works */
    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (BrcmDmaBase == NULL)
    {
        Printf("%s: %s is not there\n", (LONG)CMD_NAME, (LONG)BRCMDMANAME);
        CloseLibrary((struct Library *)DOSBase);
        return RETURN_ERROR;
    }
    BDMA_QueryInfoTags(
        BDI_Available, (ULONG)&available,
        TAG_DONE);
    if (!available)
    {
        Printf("%s: the engine of %s is not available\n", (LONG)CMD_NAME, (LONG)BRCMDMANAME);
        CloseLibrary((struct Library *)DOSBase);
        return RETURN_ERROR;
    }

    LaunchCM = cm;
    LaunchCMQ = cmq;
    LaunchThreshold = threshold;

    /* from Workbench this process is the daemon */
    if (wbmsg != NULL)
    {
        if (Install() != 0)
            rc = RETURN_ERROR;
        else
            Serve();
        CloseLibrary((struct Library *)DOSBase);
        return rc;
    }

    /* from a shell: the seglist goes to a new process, and the shell gets the prompt back */
    cli = Cli();
    if (cli == NULL || cli->cli_Module == 0)
    {
        Printf("%s: cannot detach, staying in the foreground (Ctrl-C takes the patch off)\n", (LONG)CMD_NAME);
        if (Install() != 0)
        {
            CloseLibrary((struct Library *)DOSBase);
            return RETURN_ERROR;
        }
        Announce(cm, cmq, threshold, "installed");
        Serve();
        return RETURN_OK;
    }

    IsChild = TRUE;
    LaunchStatus = L_PENDING;
    LaunchAck = 0;

    proc = CreateNewProcTags(
        NP_Seglist, cli->cli_Module,
        NP_FreeSeglist, TRUE,
        NP_Cli, TRUE,
        NP_Name, (ULONG)CMD_NAME,
        NP_StackSize, 8192,
        TAG_DONE);
    if (proc == NULL)
    {
        IsChild = FALSE;
        Printf("%s: cannot create the process\n", (LONG)CMD_NAME);
        CloseLibrary((struct Library *)DOSBase);
        return RETURN_ERROR;
    }

    /* the new process owns the seglist now: the shell must not unload it when we are over */
    cli->cli_Module = 0;

    for (waited = 0; waited < 250 && LaunchStatus == L_PENDING; waited += 2)
        Delay(2);

    if (LaunchStatus == L_OK)
    {
        Announce(cm, cmq, threshold, "installed");
        return RETURN_OK;                  /* DOS is not closed: the daemon uses it */
    }

    if (LaunchStatus == L_FAILED)
    {
        WhyText(LaunchWhy);
        LaunchAck = 1;                     /* the very last access to the segment */
        return RETURN_ERROR;
    }

    Printf("%s: the new process did not answer\n", (LONG)CMD_NAME);
    return RETURN_ERROR;
}
