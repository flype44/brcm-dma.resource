/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    brcm-dma-cost -- where the fixed cost of a small job goes. Every phase is repeated N times (default 300) and printed as min / average / max
    in microseconds (the system timer, 1 us). Refused jobs stop at known points of BDMA_AddJobTagList(Client, ), so subtracting two phases gives the
    cost of the step between them:

      A  refused for a missing destination        the call, utility.library lookup, the parsing of the tags
      B  refused for a destination out of memory  A + the allocation of the job + the checks of the memory
      C  BDMA_QueryInfoTagList                        a call that parses tags and answers (no job)
      D  a 64 byte job, cache maintenance, BDMA_WaitJob (sleeps)        add = the return of AddJob, wait = from there to the end of the wait
      E  the same with BDJ_NoCache                                       D - E = the cache maintenance (before and after)
      F  BDJ_NoCache, BDMA_CheckJob polled until true, then BDMA_WaitJob  spin = the transfer and the interrupt, with no sleep; D - F = sleep and wake

      brcm-dma-cost [N n] [SIZE bytes]

    SIZE is the length of the jobs of the phases D, E and F and of the jobs started again (G, H); default 64. With 65536 or 1048576 the phase D
    minus the phase E is the cost, for the CPU, of the cache maintenance around a big job, and the building blocks are also measured at that size.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/brcm-dma.h>

#include <resources/brcm-dma.h>

#include "hw-vc6.h"

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
APTR BrcmDmaBase;
struct BDMAClient *Client;

/* run by Supervisor(): nothing but the return */
extern ULONG CostSuperNop(void);

__asm__(
    "       .text\n"
    "       .even\n"
    "       .globl  _CostSuperNop\n"
    "_CostSuperNop:\n"
    "       rte\n"
);

struct Acc {
    ULONG n, min, max, sum;
};

static void AccInit(struct Acc *a)
{
    a->n = a->max = a->sum = 0;
    a->min = 0xffffffffUL;
}

static void AccAdd(struct Acc *a, ULONG v)
{
    a->n++;
    a->sum += v;
    if (v < a->min)
        a->min = v;
    if (v > a->max)
        a->max = v;
}

static void Print(const char *what, struct Acc *a)
{
    Printf("  %-30s %6ld %6ld %6ld\n", (LONG)what, (LONG)(a->n ? a->min : 0), (LONG)(a->n ? a->sum / a->n : 0), (LONG)a->max);
}

/* the instructions of the 68040, one per line of 64 bytes (what brcm-emmc.device does): CPUSHL writes back the dirty line and drops it */
static void PushLines(UBYTE *a, ULONG len)
{
    UBYTE *p = (UBYTE *)((ULONG)a & ~63UL), *end = (UBYTE *)(((ULONG)a + len + 63) & ~63UL);

    for (; p < end; p += 64)
        asm volatile("nop; cpushl dc,(%0)" :: "a"(p) : "memory");
}

/* CPUSHP: a whole page of 4 KB per instruction (Emu68 does the loop of dc civac itself, one dsb at the end) */
static void PushPages(UBYTE *a, ULONG len)
{
    UBYTE *p = (UBYTE *)((ULONG)a & ~4095UL), *end = (UBYTE *)(((ULONG)a + len + 4095) & ~4095UL);

    for (; p < end; p += 4096)
        asm volatile("nop; cpushp dc,(%0)" :: "a"(p) : "memory");
}

static void InvLines(UBYTE *a, ULONG len)
{
    UBYTE *p = (UBYTE *)((ULONG)a & ~63UL), *end = (UBYTE *)(((ULONG)a + len + 63) & ~63UL);

    for (; p < end; p += 64)
        asm volatile("cinvl dc,(%0)" :: "a"(p) : "memory");
}

static ULONG Rng = 0x2545F491;

static ULONG Rnd(ULONG m)
{
    Rng ^= Rng << 13;
    Rng ^= Rng >> 17;
    Rng ^= Rng << 5;

    return Rng % m;
}

/* the cost per size, then the proof that the data is right when the maintenance is done by these instructions only */
static void LineTest(UBYTE *buf0, ULONG n)
{
    static const ULONG sizes[] = { 64, 256, 1024, 4096, 16384, 65536, 262144 };
    ULONG k, i, bad = 0, runs = 0, wrong_guard = 0;
    UBYTE *buf = (UBYTE *)(((ULONG)buf0 + 63) & ~63UL);

    Printf("per line instructions (64 byte lines) and per page (cpushp, 4 KB), nanoseconds for the whole range\n  bytes     cpushl     cinvl    cpushp  CacheClearE(us)\n");
    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
    {
        ULONG len = sizes[k], lines = len / 64, reps = 4096 / lines, t0, a, b, c, pp;

        if (reps < 3)
            reps = 3;
        if (reps > 200)
            reps = 200;
        t0 = timer_now(); for (i = 0; i < reps; i++) PushLines(buf, len); a = (timer_now() - t0) * 1000 / reps;
        t0 = timer_now(); for (i = 0; i < reps; i++) InvLines(buf, len); b = (timer_now() - t0) * 1000 / reps;
        t0 = timer_now(); for (i = 0; i < reps; i++) PushPages(buf, len); pp = (timer_now() - t0) * 1000 / reps;
        t0 = timer_now(); for (i = 0; i < reps; i++) CacheClearE(buf, len, CACRF_ClearD); c = (timer_now() - t0) / reps;
        Printf("  %6ld %9ld %9ld %9ld %9ld\n", (LONG)len, (LONG)a, (LONG)b, (LONG)pp, (LONG)c);
    }

    /* correctness: sources written by the CPU (dirty in the cache), destinations too, jobs with BDJ_NoCache, only the instructions above */
    for (i = 0; i < n; i++)
    {
        ULONG len = 4 * (1 + Rnd(16384)), so = 4 * Rnd(32), dof = 4 * Rnd(32), j;
        UBYTE *s = buf + 64 + so, *d = buf + 0x20000 + 64 + dof;
        ULONG *sw = (ULONG *)s, *dw = (ULONG *)d, seed = Rnd(0xffff) + 1;
        struct BDMAJob *job;

        for (j = 0; j < len / 4; j++)
            sw[j] = seed * 2654435761UL + j;
        for (j = 0; j < len / 4 + 32; j++)
            ((ULONG *)(d - 64))[j] = 0xA5A5A5A5UL;
        for (j = 0; j < 16; j++)
            ((ULONG *)(d + len))[j] = 0xA5A5A5A5UL;
        PushLines(s, len);
        PushLines(d - 64, len + 128);
        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, len, BDJ_NoCache, TRUE, TAG_DONE);
        if (job == NULL)
        {
            bad++;
            continue;
        }
        BDMA_WaitJob(job);
        InvLines(d - 64, len + 128);
        for (j = 0; j < len / 4; j++)
            if (dw[j] != seed * 2654435761UL + j)
            {
                bad++;
                break;
            }
        for (j = 0; j < 16; j++)
            if (((ULONG *)(d - 64))[j] != 0xA5A5A5A5UL || ((ULONG *)(d + len))[j] != 0xA5A5A5A5UL)
                wrong_guard++;
        runs++;
    }
    Printf("  %ld jobs with only cpushl / cinvl: %ld wrong, %ld guard words touched\n",(LONG)runs, (LONG)bad, (LONG)wrong_guard);
}

/* MASK: does the way of keeping the interrupts out really keep out the interrupt of the DMA? A job of SIZE bytes is on its channel; the
   interrupts are masked for twice its length; if the end of the job is seen inside the masked region the interrupt came through, if it is
   seen only after, the mask held. Disable() writes INTENA of the chipset, SetSR() the mask of the SR of the CPU. */
static void MaskTest(UBYTE *s, UBYTE *d, ULONG size)
{
    static const char *names[] = { "Disable() / Enable()", "SetSR mask 7" };
    ULONG m;

    for (m = 0; m < 2; m++)
    {
        struct BDMAJob *job;
        ULONG t0, t1, old = 0, inside, after;

        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, size, BDJ_NoCache, TRUE, TAG_DONE);
        if (job == NULL)
        {
            Printf("  no job\n");
            return;
        }

        if (m == 0)
            Disable();
        else
            old = SetSR(0x0700, 0x0700);

        t0 = timer_now();
        while (timer_now() - t0 < 2000)
            ;
        inside = BDMA_CheckJob(job);

        if (m == 0)
            Enable();
        else
            SetSR(old & 0x0700, 0x0700);

        t1 = timer_now();
        while (!BDMA_CheckJob(job) && timer_now() - t1 < 100000UL)
            ;
        after = BDMA_CheckJob(job);
        BDMA_WaitJob(job);

        Printf("  %-22s end of the job seen inside the masked region: %s; over after the unmask: %s\n",
            (LONG)names[m],
            (LONG)(inside ? "YES (the interrupt came through)" : "no (the mask held)"),
            (LONG)(after ? "yes" : "NO (lost)"));
    }
}

int main(int argc, struct WBStartup *wbmsg)
{
    struct RDArgs *rda;
    LONG args[3] = { 0, 0, 0 };
    BOOL mask = FALSE;
    ULONG n = 300, size = 64, i, t0, t1, t2, t3, available = 0;
    UBYTE *buf, *buf2, *s, *d;
    struct Acc A, B, C, Dadd, Dwait, Dtot, Eadd, Ewait, Etot, Fadd, Fspin, Fwait, Ftot;
    ULONG jobs;

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("N/K/N,SIZE/K/N,MASK/S", args, NULL);
    if (rda != NULL)
    {
        if (args[0])
            n = *(ULONG *)args[0];
        if (args[1] && *(ULONG *)args[1] != 0)
            size = *(ULONG *)args[1];
        mask = args[2] != 0;
        FreeArgs(rda);
    }

    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (BrcmDmaBase == NULL)
    {
        Printf("%s is not there\n", (LONG)BRCMDMANAME);
        return 10;
    }
    BDMA_QueryInfoTags(BDI_Available, (ULONG)&available, TAG_DONE);
    if (!available)
    {
        Printf("engine not available\n");
        return 10;
    }

    Client = BDMA_OpenClientTags(BDC_Name, (ULONG)"brcm-dma-cost", TAG_DONE);
    if (Client == NULL)
    {
        Printf("no client\n");
        return 10;
    }

    buf2 = AllocMem(0x40000 + 256, MEMF_ANY | MEMF_CLEAR);
    buf = AllocMem(8192, MEMF_ANY | MEMF_CLEAR);
    if (buf == NULL)
        return 20;

    /* the source and the destination of the jobs: the two halves of buf for the small jobs, blocks of their own for the big ones */
    if (size > 4096)
    {
        s = AllocMem(size, MEMF_PUBLIC | MEMF_FAST);
        d = AllocMem(size, MEMF_PUBLIC | MEMF_FAST);
        if (s == NULL || d == NULL)
        {
            Printf("no memory for SIZE %ld\n", (LONG)size);
            return 20;
        }
        for (i = 0; i < size; i += 4096)
            s[i] = (UBYTE)(i >> 12);
    }
    else
    {
        s = buf;
        d = buf + 4096;
    }
    Printf("jobs of %ld bytes\n", (LONG)size);

    if (mask)
    {
        if (size < 524288UL)
            Printf("MASK needs a SIZE of 524288 or more (the job must last longer than the masked region)\n");
        else
            MaskTest(s, d, size);
        BDMA_CloseClient(Client);
        CloseLibrary((struct Library *)DOSBase);
        return 0;
    }

    AccInit(&A); AccInit(&B); AccInit(&C);
    AccInit(&Dadd); AccInit(&Dwait); AccInit(&Dtot);
    AccInit(&Eadd); AccInit(&Ewait); AccInit(&Etot);
    AccInit(&Fadd); AccInit(&Fspin); AccInit(&Fwait); AccInit(&Ftot);

    /* a first job, so that nothing is measured while the first caller pays for the start of the engine */
    BDMA_WaitJob(BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, size, TAG_DONE));

    for (i = 0; i < n; i++)
    {
        struct BDMAJob *job;
        ULONG v;

        t0 = timer_now();
        BDMA_AddJobTags(Client, BDJ_Src, (ULONG)buf, BDJ_Length, 64, TAG_DONE);
        AccAdd(&A, timer_now() - t0);

        t0 = timer_now();
        BDMA_AddJobTags(Client, BDJ_Src, (ULONG)buf, BDJ_Dst, 0x7f000000, BDJ_Length, 64, TAG_DONE);
        AccAdd(&B, timer_now() - t0);

        t0 = timer_now();
        BDMA_QueryInfoTags(BDI_Jobs, (ULONG)&v, TAG_DONE);
        AccAdd(&C, timer_now() - t0);

        /* D: cache maintenance, sleeping wait */
        t0 = timer_now();
        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, size, TAG_DONE);
        t1 = timer_now();
        if (job)
            BDMA_WaitJob(job);
        t2 = timer_now();
        AccAdd(&Dadd, t1 - t0);
        AccAdd(&Dwait, t2 - t1);
        AccAdd(&Dtot, t2 - t0);

        /* E: no cache maintenance */
        t0 = timer_now();
        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, size, BDJ_NoCache, TRUE, TAG_DONE);
        t1 = timer_now();
        if (job)
            BDMA_WaitJob(job);
        t2 = timer_now();
        AccAdd(&Eadd, t1 - t0);
        AccAdd(&Ewait, t2 - t1);
        AccAdd(&Etot, t2 - t0);

        /* F: no cache maintenance, polled to the end, then the wait that returns at once */
        t0 = timer_now();
        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, size, BDJ_NoCache, TRUE, TAG_DONE);
        t1 = timer_now();
        if (job)
        {
            while (!BDMA_CheckJob(job) && timer_now() - t1 < 100000UL)
                ;
            t2 = timer_now();
            BDMA_WaitJob(job);
            t3 = timer_now();
            AccAdd(&Fadd, t1 - t0);
            AccAdd(&Fspin, t2 - t1);
            AccAdd(&Fwait, t3 - t2);
            AccAdd(&Ftot, t3 - t0);
        }
    }

    /* a job described once and started again: what the new model is for. G: BDMA_StartJob and BDMA_WaitJob only; H: with BDMA_SetJobTags before */
    {
        struct Acc Gs, Gw, Gt, Hs, Ht;
        struct BDMAJob *rj = BDMA_AllocJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, size, BDJ_NoCache, TRUE, TAG_DONE);

        AccInit(&Gs); AccInit(&Gw); AccInit(&Gt); AccInit(&Hs); AccInit(&Ht);
        if (rj != NULL)
        {
            for (i = 0; i < n; i++)
            {
                t0 = timer_now();
                BDMA_StartJob(rj);
                t1 = timer_now();
                BDMA_WaitJob(rj);
                t2 = timer_now();
                AccAdd(&Gs, t1 - t0);
                AccAdd(&Gw, t2 - t1);
                AccAdd(&Gt, t2 - t0);

                t0 = timer_now();
                BDMA_SetJobTags(rj, BDJ_Src, (ULONG)s + 4 * (i & 7), TAG_DONE);
                t1 = timer_now();
                BDMA_StartJob(rj);
                BDMA_WaitJob(rj);
                t2 = timer_now();
                AccAdd(&Hs, t1 - t0);
                AccAdd(&Ht, t2 - t0);
            }
            BDMA_FreeJob(rj);
        }
        Printf("a job described once (BDMA_AllocJobTags), started again; microseconds\n  phase                           min    avg    max\n");
        Print("G StartJob return", &Gs);
        Print("G WaitJob", &Gw);
        Print("G total (Start + Wait)", &Gt);
        Print("H SetJobTags alone", &Hs);
        Print("H total (Set + Start + Wait)", &Ht);
    }

    /* the building blocks, one call at a time */
    {
        struct Acc G1, G2, H0, H1, I0, J, K, L, M, Z0, Z1, Z2, Z3, S1, S2;
        ULONG l;
        BYTE bit = AllocSignal(-1);

        AccInit(&G1); AccInit(&G2); AccInit(&H0); AccInit(&H1); AccInit(&I0); AccInit(&J); AccInit(&K); AccInit(&L); AccInit(&M); AccInit(&Z0); AccInit(&Z1); AccInit(&Z2); AccInit(&Z3); AccInit(&S1); AccInit(&S2);
        for (i = 0; i < n; i++)
        {
            APTR p;

            t0 = timer_now(); CacheClearE(buf, 64, CACRF_ClearD); AccAdd(&G1, timer_now() - t0);
            t0 = timer_now(); CacheClearE(buf, 4096, CACRF_ClearD); AccAdd(&G2, timer_now() - t0);
            l = 64; t0 = timer_now(); CachePreDMA(buf, &l, DMA_ReadFromRAM); AccAdd(&H0, timer_now() - t0);
            l = 64; t0 = timer_now(); CachePreDMA(buf + 4096, &l, 0); AccAdd(&H1, timer_now() - t0);
            l = 64; t0 = timer_now(); CachePostDMA(buf + 4096, &l, 0); AccAdd(&I0, timer_now() - t0);
            t0 = timer_now(); { BYTE b = AllocSignal(-1); if (b >= 0) FreeSignal(b); } AccAdd(&J, timer_now() - t0);
            t0 = timer_now(); Disable(); Enable(); AccAdd(&K, timer_now() - t0);
            /* the other ways to keep the interrupts out: the trap of Supervisor() alone, and the mask of the SR (what Disable() does in the chipset) */
            t0 = timer_now(); Supervisor((ULONG (*)())CostSuperNop); AccAdd(&S1, timer_now() - t0);
            t0 = timer_now(); { ULONG old = SetSR(0x0700, 0x0700); SetSR(old & 0x0700, 0x0700); } AccAdd(&S2, timer_now() - t0);
            if (bit >= 0) { t0 = timer_now(); Signal(FindTask(NULL), 1UL << bit); AccAdd(&L, timer_now() - t0); SetSignal(0, 1UL << bit); }
            t0 = timer_now(); p = AllocVec(64, MEMF_PUBLIC | MEMF_CLEAR); FreeVec(p); AccAdd(&M, timer_now() - t0);
            /* the same calls at the size of the jobs */
            t0 = timer_now(); CacheClearE(s, size, CACRF_ClearD); AccAdd(&Z0, timer_now() - t0);
            l = size; t0 = timer_now(); CachePreDMA(s, &l, DMA_ReadFromRAM); AccAdd(&Z1, timer_now() - t0);
            l = size; t0 = timer_now(); CachePreDMA(d, &l, 0); AccAdd(&Z2, timer_now() - t0);
            l = size; t0 = timer_now(); CachePostDMA(d, &l, 0); AccAdd(&Z3, timer_now() - t0);
        }
        if (bit >= 0)
            FreeSignal(bit);
        Printf("building blocks, microseconds\n  call                           min    avg    max\n");
        Print("CacheClearE 64 bytes", &G1);
        Print("CacheClearE 4096 bytes", &G2);
        Print("CachePreDMA 64, ReadFromRAM", &H0);
        Print("CachePreDMA 64, write side", &H1);
        Print("CachePostDMA 64", &I0);
        Print("AllocSignal + FreeSignal", &J);
        Print("Disable + Enable", &K);
        Print("Supervisor() empty", &S1);
        Print("SetSR mask 7 + restore", &S2);
        Print("Signal() to self", &L);
        Print("AllocVec + FreeVec 64", &M);
        Print("CacheClearE SIZE", &Z0);
        Print("CachePreDMA SIZE, ReadFromRAM", &Z1);
        Print("CachePreDMA SIZE, write side", &Z2);
        Print("CachePostDMA SIZE", &Z3);
    }

    LineTest(buf2, n);

    jobs = n;
    Printf("%ld repetitions; microseconds\n  phase                           min    avg    max\n", (LONG)jobs);
    Print("A refused: no destination", &A);
    Print("B refused: out of memory", &B);
    Print("C BDMA_QueryInfoTagList", &C);
    Print("D cache: AddJob return", &Dadd);
    Print("D cache: wait (sleeping)", &Dwait);
    Print("D cache: total", &Dtot);
    Print("E nocache: AddJob return", &Eadd);
    Print("E nocache: wait (sleeping)", &Ewait);
    Print("E nocache: total", &Etot);
    Print("F nocache: AddJob return", &Fadd);
    Print("F nocache: spin to the end", &Fspin);
    Print("F nocache: WaitJob after", &Fwait);
    Print("F nocache: total", &Ftot);

    BDMA_CloseClient(Client);
    FreeMem(buf, 8192);
    CloseLibrary((struct Library *)DOSBase);

    return 0;
}
