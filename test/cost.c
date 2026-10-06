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

      brcm-dma-cost [N n]
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

    Printf("per line instructions (64 byte lines), nanoseconds for the whole range\n  bytes     cpushl     cinvl  CacheClearE(us)\n");
    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
    {
        ULONG len = sizes[k], lines = len / 64, reps = 4096 / lines, t0, a, b, c;

        if (reps < 3)
            reps = 3;
        if (reps > 200)
            reps = 200;
        t0 = timer_now(); for (i = 0; i < reps; i++) PushLines(buf, len); a = (timer_now() - t0) * 1000 / reps;
        t0 = timer_now(); for (i = 0; i < reps; i++) InvLines(buf, len); b = (timer_now() - t0) * 1000 / reps;
        t0 = timer_now(); for (i = 0; i < reps; i++) CacheClearE(buf, len, CACRF_ClearD); c = (timer_now() - t0) / reps;
        Printf("  %6ld %9ld %9ld %9ld\n",(LONG)len, (LONG)a, (LONG)b, (LONG)c);
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

int main(int argc, struct WBStartup *wbmsg)
{
    struct RDArgs *rda;
    LONG args[1] = { 0 };
    ULONG n = 300, i, t0, t1, t2, t3, available = 0;
    UBYTE *buf, *buf2;
    struct Acc A, B, C, Dadd, Dwait, Dtot, Eadd, Ewait, Etot, Fadd, Fspin, Fwait, Ftot;
    ULONG jobs;

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("N/K/N", args, NULL);
    if (rda != NULL)
    {
        if (args[0])
            n = *(ULONG *)args[0];
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

    AccInit(&A); AccInit(&B); AccInit(&C);
    AccInit(&Dadd); AccInit(&Dwait); AccInit(&Dtot);
    AccInit(&Eadd); AccInit(&Ewait); AccInit(&Etot);
    AccInit(&Fadd); AccInit(&Fspin); AccInit(&Fwait); AccInit(&Ftot);

    /* a first job, so that nothing is measured while the first caller pays for the start of the engine */
    BDMA_WaitJob(BDMA_AddJobTags(Client, BDJ_Src, (ULONG)buf, BDJ_Dst, (ULONG)buf + 4096, BDJ_Length, 64, TAG_DONE));

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
        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)buf, BDJ_Dst, (ULONG)buf + 4096, BDJ_Length, 64, TAG_DONE);
        t1 = timer_now();
        if (job)
            BDMA_WaitJob(job);
        t2 = timer_now();
        AccAdd(&Dadd, t1 - t0);
        AccAdd(&Dwait, t2 - t1);
        AccAdd(&Dtot, t2 - t0);

        /* E: no cache maintenance */
        t0 = timer_now();
        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)buf, BDJ_Dst, (ULONG)buf + 4096, BDJ_Length, 64, BDJ_NoCache, TRUE, TAG_DONE);
        t1 = timer_now();
        if (job)
            BDMA_WaitJob(job);
        t2 = timer_now();
        AccAdd(&Eadd, t1 - t0);
        AccAdd(&Ewait, t2 - t1);
        AccAdd(&Etot, t2 - t0);

        /* F: no cache maintenance, polled to the end, then the wait that returns at once */
        t0 = timer_now();
        job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)buf, BDJ_Dst, (ULONG)buf + 4096, BDJ_Length, 64, BDJ_NoCache, TRUE, TAG_DONE);
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
        struct BDMAJob *rj = BDMA_AllocJobTags(Client, BDJ_Src, (ULONG)buf, BDJ_Dst, (ULONG)buf + 4096, BDJ_Length, 64, BDJ_NoCache, TRUE, TAG_DONE);

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
                BDMA_SetJobTags(rj, BDJ_Src, (ULONG)buf + 4 * (i & 7), TAG_DONE);
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
        struct Acc G1, G2, H0, H1, I0, J, K, L, M;
        ULONG l;
        BYTE bit = AllocSignal(-1);

        AccInit(&G1); AccInit(&G2); AccInit(&H0); AccInit(&H1); AccInit(&I0); AccInit(&J); AccInit(&K); AccInit(&L); AccInit(&M);
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
            if (bit >= 0) { t0 = timer_now(); Signal(FindTask(NULL), 1UL << bit); AccAdd(&L, timer_now() - t0); SetSignal(0, 1UL << bit); }
            t0 = timer_now(); p = AllocVec(64, MEMF_PUBLIC | MEMF_CLEAR); FreeVec(p); AccAdd(&M, timer_now() - t0);
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
        Print("Signal() to self", &L);
        Print("AllocVec + FreeVec 64", &M);
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
