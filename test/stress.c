/*
    brcm-dma-stress -- walks through every path of brcm-dma.resource, stresses the machine with it and prints statistics.

      brcm-dma-stress [PATHS] [QUEUE] [BIG] [SOAK] [ALL] [SECONDS n] [MB n] [TEMPMAX n] [SEED n]

      PATHS    every combination of kind (copy, fill, move down, move up), alignment of the source and of the destination (head / body / tail
               of the control blocks), length, number of rows, pitches, with and without cache maintenance: the memory is compared word by
               word, the guard words around and between the rows must stay untouched.
      QUEUE    the scheduler: batches of jobs submitted at once (priorities, BDJ_NoWait, notification by signal, BDMA_CheckJob, abort of a job
               that waits) and the order of conflicting jobs (read after write, write after write, write after read).
      BIG      very big transfers: 1 MB doubling up to MB megabytes (default 64) as one job, as a rectangle of 1280 rows, as a scroll, with
               a fill; compared with CopyMem() and a CPU loop. This one holds the memory bus for tens of milliseconds a job.
      SOAK     random jobs of random sizes for SECONDS seconds (default 60), one to three in flight, every result verified, the temperature
               of the SoC read between batches (the work stops while it is above TEMPMAX, in degrees, default 80; the firmware's limit is 85).
      ALL      PATHS QUEUE BIG SOAK.   Without argument: PATHS and QUEUE.

    Ctrl-C stops the current phase cleanly. The statistics are printed after each phase and the totals at the end, with the numbers that the
    resource counted itself. Exit code 0 when nothing failed. BIG and SOAK can freeze a machine whose DMA is not right: start them on purpose,
    one at a time, and write down the clock of the Pi and its temperature.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/dos.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/brcm-dma.h>
#include <proto/mailbox.h>

#include <resources/brcm-dma.h>
#include "mbox.h"

#include "hw-vc6.h"

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
APTR BrcmDmaBase;
struct BDMAClient *Client;
struct MsgPort *NotifyPort;      /* the port of the jobs that ask for a notification: a second reply port */
APTR MailboxBase;

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* The basics                                                                                                                           */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

#define SENT  0xA5A5A5A5UL          /* the guard word: what the DMA must not touch */
#define PRE   64                    /* bytes of guard before the first row (the destination starts at PRE + a few bytes) */
#define POST  64                    /* ... and after the last one */

static ULONG rng = 0x12345678;
static ULONG failed;                /* checks that went wrong, all phases */
static ULONG printed;               /* failures already described */
static BOOL stopped;                /* Ctrl-C */

static ULONG Rnd(ULONG n)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;

    return rng % n;
}

static ULONG Pattern(ULONG i, ULONG seed)
{
    return (i * 2654435761UL) ^ (seed * 0x01010101UL) ^ 0x5a5a1234UL;
}

static BOOL Break(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
    {
        SetSignal(0, SIGBREAKF_CTRL_C);
        stopped = TRUE;
    }

    return stopped;
}

static void Fail(void)
{
    failed++;
}

/* A block of memory aligned on 16 bytes, from the fast memory when there is some */
struct Mem {
    APTR raw;
    ULONG size;
    UBYTE *p;
};

static BOOL MemGet(struct Mem *m, ULONG size)
{
    m->size = size + 16;
    m->raw = AllocMem(m->size, MEMF_FAST | MEMF_PUBLIC);
    if (m->raw == NULL)
        m->raw = AllocMem(m->size, MEMF_ANY);
    m->p = (UBYTE *)(((ULONG)m->raw + 15) & ~15UL);

    return m->raw != NULL;
}

static void MemPut(struct Mem *m)
{
    if (m->raw != NULL)
        FreeMem(m->raw, m->size);
    m->raw = NULL;
}

/* Statistics: a count, the shortest, the longest and the total of the times (microseconds) and the bytes (in blocks of 256) */
struct Stat {
    ULONG n, min, max, us, blocks, rem;
};

static void StatInit(struct Stat *s)
{
    s->n = s->max = s->us = s->blocks = s->rem = 0;
    s->min = 0xffffffffUL;
}

static void StatAdd(struct Stat *s, ULONG bytes, ULONG us)
{
    s->n++;
    if (us < s->min)
        s->min = us;
    if (us > s->max)
        s->max = us;
    s->us += us;
    s->rem += bytes & 255;
    s->blocks += (bytes >> 8) + (s->rem >> 8);
    s->rem &= 255;
}

/* Megabytes per second times ten (a byte per microsecond is a megabyte per second), without 64 bit arithmetic */
static ULONG Rate10(ULONG blocks, ULONG us)
{
    if (us == 0)
        us = 1;
    while (blocks > 1600000UL)
    {
        blocks >>= 1;
        us >>= 1;
        if (us == 0)
            us = 1;
    }

    return blocks * 2560 / us;
}

/* The temperature of the SoC in millidegrees, 0 when it cannot be read. Never called during a job. */
static ULONG ReadTemp(void)
{
    ULONG b[8];

    if (MailboxBase == NULL)
        return 0;

    b[0] = sizeof(b);
    b[1] = 0;
    b[2] = 0x00030006;      /* GET_TEMPERATURE */
    b[3] = 8;
    b[4] = 0;
    b[5] = 0;               /* the sensor: the SoC */
    b[6] = 0;
    b[7] = 0;

    MB_RawCommand(b);
    if (b[0] == 0xffffffffUL || b[1] != MB_SUCCESS)
        return 0;

    return b[6];
}

static void PrintTemp(ULONG t)
{
    if (t == 0)
        Printf("n/a");
    else
        Printf("%ld.%ld C", (LONG)(t / 1000), (LONG)((t % 1000) / 100));
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* A job, as the stress tool describes it                                                                                               */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

#define K_COPY 0
#define K_FILL 1
#define K_DOWN 2        /* a move whose destination is below the source (the rows go backwards) */
#define K_UP   3

static const char * const kindname[] = { "copy", "fill", "down", "up" };

struct Op {
    ULONG kind;
    ULONG smod, dmod;           /* bytes after the guard: where the source and the destination start */
    ULONG len, rows, sp, dp;    /* bytes of a row, rows, pitches (for a move sp == dp) */
    ULONG val;                  /* the value of a fill */
    BOOL nocache, nowait, notify;
    ULONG prio;
    ULONG seed;
    /* made by Prep() */
    UBYTE *src, *dst;
    UBYTE *dwin;                /* the start of the window of the destination, guard included */
    ULONG wbytes;               /* its size */
    ULONG pre;                  /* offset of the first row in the window */
    ULONG signals;              /* BDJ_NotifySignals */
    /* answers */
    struct BDMAJob *job;
    ULONG error;
};

/* Writes the source, the destination and the guards of the window of an operation */
static void Prep(struct Op *o, UBYTE *S, UBYTE *D, ULONG seed)
{
    ULONG *w;
    ULONG k, n;

    o->seed = seed;
    o->dwin = D;
    o->pre = PRE + o->dmod;

    if (o->kind == K_COPY || o->kind == K_FILL)
    {
        o->dst = D + o->pre;
        o->wbytes = o->pre + (o->rows - 1) * o->dp + o->len + POST;

        w = (ULONG *)D;
        for (k = 0; k < o->wbytes / 4; k++)
            w[k] = SENT;

        if (o->kind == K_COPY)
        {
            o->src = S + PRE + o->smod;
            n = ((o->rows - 1) * o->sp + o->len) / 4;
            w = (ULONG *)o->src;
            for (k = 0; k < n; k++)
                w[k] = Pattern(k, seed);
        }
        else
            o->src = NULL;
    }
    else
    {
        /* a move: one area of rows + 1 rows holds both the source and the destination */
        ULONG total = (o->rows + 1) * o->dp;
        UBYTE *base = D + o->pre;

        o->wbytes = o->pre + total + POST;
        w = (ULONG *)D;
        for (k = 0; k < o->wbytes / 4; k++)
            w[k] = SENT;
        w = (ULONG *)base;
        for (k = 0; k < total / 4; k++)
            w[k] = Pattern(k, seed);

        if (o->kind == K_DOWN)
        {
            o->src = base;
            o->dst = base + o->dp;
        }
        else
        {
            o->dst = base;
            o->src = base + o->dp;
        }
    }

    if (o->nocache)
    {
        /* what BDJ_NoCache leaves to the caller: the data is in memory before the job, and the cache is dropped after */
        if (o->src != NULL && o->kind == K_COPY)
            CacheClearE(S, PRE + o->smod + (o->rows - 1) * o->sp + o->len, CACRF_ClearD);
        CacheClearE(D, o->wbytes, CACRF_ClearD);
    }
}

/* Compares the window with what the operation must have made of it; returns the number of wrong words (the first one is described) */
static ULONG Verify(struct Op *o, const char *where)
{
    ULONG *d = (ULONG *)o->dwin;
    ULONG bad = 0, first = 0xffffffffUL;
    ULONG r, i, e, pw, lw = o->len / 4;

#define CHECK(index, expected) do { if (d[index] != (expected)) { if (bad++ == 0) first = (index); } } while (0)

    if (o->nocache)
        CacheClearE(o->dwin, o->wbytes, CACRF_ClearD);

    for (i = 0; i < o->pre / 4; i++)
        CHECK(i, SENT);

    if (o->kind == K_COPY || o->kind == K_FILL)
    {
        ULONG sw = o->sp / 4, dw = o->dp / 4;

        for (r = 0; r < o->rows; r++)
        {
            ULONG at = o->pre / 4 + r * dw;

            for (i = 0; i < lw; i++)
            {
                e = o->kind == K_FILL ? o->val : Pattern(r * sw + i, o->seed);
                CHECK(at + i, e);
            }
            if (r < o->rows - 1)
                for (i = lw; i < dw; i++)
                    CHECK(at + i, SENT);
        }

        for (i = (o->pre + (o->rows - 1) * o->dp + o->len) / 4; i < o->wbytes / 4; i++)
            CHECK(i, SENT);
    }
    else
    {
        pw = o->dp / 4;

        for (r = 0; r <= o->rows; r++)
        {
            ULONG at = o->pre / 4 + r * pw;

            for (i = 0; i < pw; i++)
            {
                if (i >= lw)
                    e = Pattern(r * pw + i, o->seed);
                else if (o->kind == K_DOWN)
                    e = Pattern((r ? r - 1 : 0) * pw + i, o->seed);
                else
                    e = Pattern((r < o->rows ? r + 1 : r) * pw + i, o->seed);
                CHECK(at + i, e);
            }
        }

        for (i = (o->pre + (o->rows + 1) * o->dp) / 4; i < o->wbytes / 4; i++)
            CHECK(i, SENT);
    }

#undef CHECK

    if (bad && printed++ < 20)
        Printf("FAIL %s %s: src+%ld dst+%ld len %ld rows %ld sp %ld dp %ld nocache %ld: %ld words wrong, the first at +%ld bytes of the window\n",
               (LONG)where, (LONG)kindname[o->kind], (LONG)o->smod, (LONG)o->dmod, (LONG)o->len, (LONG)o->rows, (LONG)o->sp, (LONG)o->dp,
               (LONG)o->nocache, (LONG)bad, (LONG)(first * 4));

    return bad;
}

#define MAXTAGS 20

/* Submits the job; o->job is NULL and o->error says why when it was refused */
static void Submit(struct Op *o)
{
    struct TagItem t[MAXTAGS];
    ULONG n = 0;

    o->error = 0xdeadbeef;

    if (o->kind == K_FILL)
    {
        t[n].ti_Tag = BDJ_FillValue; t[n++].ti_Data = o->val;
    }
    else
    {
        t[n].ti_Tag = BDJ_Src; t[n++].ti_Data = (ULONG)o->src;
        t[n].ti_Tag = BDJ_SrcPitch; t[n++].ti_Data = o->sp;
    }
    t[n].ti_Tag = BDJ_Dst; t[n++].ti_Data = (ULONG)o->dst;
    t[n].ti_Tag = BDJ_Length; t[n++].ti_Data = o->len;
    t[n].ti_Tag = BDJ_Rows; t[n++].ti_Data = o->rows;
    t[n].ti_Tag = BDJ_DstPitch; t[n++].ti_Data = o->dp;
    t[n].ti_Tag = BDJ_Priority; t[n++].ti_Data = o->prio;
    if (o->kind == K_DOWN || o->kind == K_UP)
    {
        t[n].ti_Tag = BDJ_Move; t[n++].ti_Data = TRUE;
    }
    if (o->nocache)
    {
        t[n].ti_Tag = BDJ_NoCache; t[n++].ti_Data = TRUE;
    }
    if (o->nowait)
    {
        t[n].ti_Tag = BDJ_NoWait; t[n++].ti_Data = TRUE;
    }
    if (o->notify)
    {
        t[n].ti_Tag = BDJ_ReplyPort; t[n++].ti_Data = (ULONG)NotifyPort;
    }
    t[n].ti_Tag = BDJ_ErrorCode; t[n++].ti_Data = (ULONG)&o->error;
    t[n].ti_Tag = TAG_DONE; t[n].ti_Data = 0;

    o->job = BDMA_AddJobTagList(Client, t);
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* Latency by size                                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

#define NCLASS 12
static const char * const classname[NCLASS] = { "<= 64 B", "<= 256 B", "<= 1 KB", "<= 4 KB", "<= 16 KB", "<= 64 KB", "<= 256 KB", "<= 1 MB", "<= 4 MB",
                                                "<= 16 MB", "<= 64 MB", "> 64 MB" };
static struct Stat bysize[4][NCLASS];       /* [kind][class]: submission to the end of the wait, one job alone */

static void ClassesInit(void)
{
    ULONG k, c;

    for (k = 0; k < 4; k++)
        for (c = 0; c < NCLASS; c++)
            StatInit(&bysize[k][c]);
}

static ULONG SizeClass(ULONG bytes)
{
    ULONG c = 0, limit = 64;

    while (c < NCLASS - 1 && bytes > limit)
    {
        c++;
        limit <<= 2;
    }

    return c;
}

static void ClassesPrint(void)
{
    ULONG k, c;
    BOOL any = FALSE;

    for (k = 0; k < 4; k++)
        for (c = 0; c < NCLASS; c++)
        {
            struct Stat *s = &bysize[k][c];

            if (s->n == 0)
                continue;
            if (!any)
                Printf("  kind  size            jobs   min us   avg us   max us   MB/s (avg)\n");
            any = TRUE;
            Printf("  %-5s %-12s %7ld %8ld %8ld %8ld   %4ld.%ld\n", (LONG)kindname[k], (LONG)classname[c], (LONG)s->n, (LONG)s->min, (LONG)(s->us / s->n),
                   (LONG)s->max, (LONG)(Rate10(s->blocks, s->us) / 10), (LONG)(Rate10(s->blocks, s->us) % 10));
        }
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* PATHS                                                                                                                                */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

#define PBUF 0x40000        /* 256 KB for the source, 256 KB for the destination window */

static const ULONG lens[] = { 4, 8, 12, 16, 20, 28, 32, 36, 60, 64, 100, 252, 256, 1020, 4096, 65540 };
static const ULONG rowsets[] = { 1, 2, 3, 37 };
#define NLENS (sizeof(lens) / sizeof(lens[0]))
#define NROWS (sizeof(rowsets) / sizeof(rowsets[0]))

static void PhasePaths(void)
{
    struct Mem S, D;
    struct Op o;
    ULONG kind, sm, dm, li, ri, pv, nc, cases = 0, bad0 = failed, t0, t1;
    struct Stat st[4];

    Printf("== PATHS: every kind x alignment x length x rows x pitch x cache\n");

    if (!MemGet(&S, PBUF) || !MemGet(&D, PBUF))
    {
        Printf("no memory\n");
        MemPut(&S);
        failed++;
        return;
    }

    for (kind = 0; kind < 4; kind++)
        StatInit(&st[kind]);

    t0 = timer_now();

    for (kind = 0; kind < 4 && !Break(); kind++)
        for (nc = 0; nc < 2 && !Break(); nc++)
            for (li = 0; li < NLENS && !Break(); li++)
                for (ri = 0; ri < NROWS; ri++)
                    for (dm = 0; dm < 16; dm += 4)
                        for (sm = 0; sm < (kind == K_COPY ? 16U : 4U); sm += 4)       /* the source alignment only matters to a copy */
                            for (pv = 0; pv < 4; pv++)
                            {
                                ULONG len = lens[li], rows = rowsets[ri];
                                ULONG a, b, us;

                                if (kind != K_COPY && sm != 0)
                                    continue;
                                if (len * rows > 160000 && !(rows == 1))
                                    continue;
                                if (len > 4096 && pv != 0)
                                    continue;
                                if ((kind == K_DOWN || kind == K_UP) && (len * (rows + 1) > 160000))
                                    continue;

                                o.kind = kind;
                                o.smod = sm;
                                o.dmod = dm;
                                o.len = len;
                                o.rows = rows;
                                if (kind == K_DOWN || kind == K_UP)
                                    o.sp = o.dp = len + 4 * pv;
                                else
                                {
                                    o.sp = len + 4 * pv + 12;
                                    o.dp = len + 4 * (3 - pv) + 20;
                                }
                                o.val = Pattern(cases, 77) | 1;
                                o.nocache = nc;
                                o.nowait = o.notify = FALSE;
                                o.prio = cases % 3;
                                o.signals = 0;

                                Prep(&o, S.p, D.p, cases + 1);

                                a = timer_now();
                                Submit(&o);
                                if (o.job == NULL)
                                {
                                    Fail();
                                    if (printed++ < 20)
                                        Printf("FAIL %s: refused, error %ld (src+%ld dst+%ld len %ld rows %ld)\n", (LONG)kindname[kind], (LONG)o.error,
                                               (LONG)sm, (LONG)dm, (LONG)len, (LONG)rows);
                                    cases++;
                                    continue;
                                }
                                b = BDMA_WaitJob(o.job);
                                us = timer_now() - a;

                                if (b != BDERR_OK)
                                {
                                    Fail();
                                    if (printed++ < 20)
                                        Printf("FAIL %s: the job ended with %ld (src+%ld dst+%ld len %ld rows %ld)\n", (LONG)kindname[kind], (LONG)b,
                                               (LONG)sm, (LONG)dm, (LONG)len, (LONG)rows);
                                }
                                else if (Verify(&o, "paths"))
                                    Fail();

                                StatAdd(&st[kind], len * rows, us);
                                if (!nc && rows == 1 && (kind == K_COPY || kind == K_FILL))
                                    StatAdd(&bysize[kind][SizeClass(len)], len, us);
                                cases++;
                            }

    t1 = timer_now();

    Printf("  %ld cases in %ld ms, %ld failed\n", (LONG)cases, (LONG)((t1 - t0) / 1000), (LONG)(failed - bad0));
    for (kind = 0; kind < 4; kind++)
        if (st[kind].n)
            Printf("  %-5s %6ld jobs, %7ld us per job on average (min %ld, max %ld)\n", (LONG)kindname[kind], (LONG)st[kind].n,
                   (LONG)(st[kind].us / st[kind].n), (LONG)st[kind].min, (LONG)st[kind].max);

    MemPut(&D);
    MemPut(&S);
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* Batches: the scheduler, and SOAK                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

static ULONG LogLen(ULONG maxbytes)
{
    ULONG top = 2, v, lg;

    while (top < 31 && (1UL << (top + 1)) <= maxbytes)
        top++;
    lg = 2 + Rnd(top - 1);
    v = (1UL << lg) + Rnd(1UL << lg);
    if (v > maxbytes)
        v = maxbytes;
    v &= ~3UL;

    return v < 4 ? 4 : v;
}

/* A random operation that fits a slot of `cap` bytes */
static void GenOp(struct Op *o, ULONG cap)
{
    ULONG r = Rnd(100), rows, maxlen;

    o->smod = 4 * Rnd(4);
    o->dmod = 4 * Rnd(4);
    o->val = Rnd(0xffffffffUL) | 1;
    o->nocache = Rnd(4) == 0;
    o->nowait = o->notify = FALSE;
    o->prio = Rnd(3);
    o->signals = 0;

    if (r < 30)
    {
        o->kind = K_COPY;
        o->rows = 1;
        o->len = o->sp = o->dp = LogLen(cap);
        return;
    }

    if (r < 55)
        o->kind = K_COPY;
    else if (r < 75)
        o->kind = K_FILL;
    else
        o->kind = Rnd(2) ? K_DOWN : K_UP;

    if (o->kind == K_FILL && Rnd(2))
    {
        o->rows = 1;
        o->len = o->sp = o->dp = LogLen(cap);
        return;
    }

    rows = 1 + Rnd(1279);
    if (o->kind == K_DOWN || o->kind == K_UP)
    {
        if (rows < 1)
            rows = 1;
        maxlen = cap / (rows + 1);
    }
    else
    {
        if (rows < 2)
            rows = 2;
        maxlen = cap / rows;
    }
    if (maxlen < 36)
    {
        rows = cap / 36 - 2;
        if (rows > 1279)
            rows = 1279;
        maxlen = cap / (rows + 1);
    }
    maxlen -= 32;

    o->rows = rows;
    o->len = LogLen(maxlen);
    if (o->kind == K_DOWN || o->kind == K_UP)
        o->sp = o->dp = o->len + 4 * Rnd(8);
    else
    {
        o->sp = o->len + 4 * Rnd(8);
        o->dp = o->len + 4 * Rnd(8);
    }
}

struct Slots {
    UBYTE *base;
    ULONG size;         /* of one slot: slot k has its source at base + 2k * size and its destination at base + (2k + 1) * size */
    ULONG count;
};

static struct Stat batchstat, tailstat;
static ULONG nops, nbusy, naborted, nnotify, noksignal;

/* One batch of `n` jobs in flight together. Returns the bytes moved. */
static ULONG Batch(struct Slots *sl, ULONG n, BOOL mix, ULONG signals)
{
    struct Op ops[8];
    ULONG k, total = 0, t0, t1, victim = 0xffffffffUL, bytes[8];
    ULONG notified = 0;
    BOOL live[8];

    if (n > 8)
        n = 8;
    if (n > sl->count)
        n = sl->count;

    for (k = 0; k < n; k++)
    {
        struct Op *o = &ops[k];

        GenOp(o, sl->size - 192);
        if (mix)
        {
            o->nowait = Rnd(5) == 0;
            o->notify = signals != 0 && Rnd(3) == 0;
            o->signals = signals;
        }
        Prep(o, sl->base + (2 * k) * sl->size, sl->base + (2 * k + 1) * sl->size, Rnd(0xffff) + 1);
        bytes[k] = o->len * o->rows;
    }

    if (mix && n >= 2 && Rnd(3) == 0)
        victim = Rnd(n);

    t0 = timer_now();
    for (k = 0; k < n; k++)
    {
        Submit(&ops[k]);
        if (ops[k].job == NULL)
        {
            if (ops[k].error == BDERR_BUSY && ops[k].nowait)
                nbusy++;
            else
            {
                Fail();
                if (printed++ < 20)
                    Printf("FAIL batch: %s refused with %ld (len %ld rows %ld)\n", (LONG)kindname[ops[k].kind], (LONG)ops[k].error, (LONG)ops[k].len,
                           (LONG)ops[k].rows);
            }
        }
        else if (ops[k].notify && k != victim)       /* an aborted job owes no signal */
            notified++;
    }

    /* wait for the jobs: one of them may be polled first with BDMA_CheckJob(), one aborted instead of waited for */
    for (k = 0; k < n; k++)
    {
        struct Op *o = &ops[k];

        if (o->job == NULL)
        {
            live[k] = FALSE;                /* refused */
            continue;
        }

        if (k == victim)
        {
            BDMA_AbortJob(o->job);
            o->job = NULL;
            live[k] = FALSE;
            naborted++;
            continue;
        }

        if (mix && Rnd(4) == 0)
        {
            ULONG a = timer_now();

            while (!BDMA_CheckJob(o->job) && timer_now() - a < 5000000UL)
                ;
        }

        o->error = BDMA_WaitJob(o->job);
        o->job = NULL;
        live[k] = o->error == BDERR_OK;
        if (o->error == BDERR_BUSY && o->nowait)
            nbusy++;           /* refused by BDJ_NoWait: replied at once with BDERR_BUSY */
        else if (!live[k])
        {
            Fail();
            if (printed++ < 20)
                Printf("FAIL batch: %s ended with %ld (len %ld rows %ld)\n", (LONG)kindname[o->kind], (LONG)o->error, (LONG)o->len, (LONG)o->rows);
        }
    }
    t1 = timer_now();

    for (k = 0; k < n; k++)
    {
        struct Op *o = &ops[k];

        if (!live[k])
            continue;
        total += bytes[k];
        nops++;
        if (Verify(o, "batch"))
            Fail();
    }

    nnotify += notified;           /* each of them was waited for through the port that it named: the reply came */
    noksignal += notified;

    if (n == 1 && live[0])
        StatAdd(&bysize[ops[0].kind][SizeClass(bytes[0])], bytes[0], t1 - t0);
    StatAdd(&batchstat, total, t1 - t0);

    return total;
}

/* A read after a write, a write after a write and a write after a read must give what the jobs run one by one would give */
static void Conflicts(UBYTE *a, UBYTE *b, UBYTE *c, UBYTE *d, ULONG size)
{
    ULONG *A = (ULONG *)a, *B = (ULONG *)b, *C = (ULONG *)c, *Dd = (ULONG *)d;
    ULONG i, bad, round = Rnd(0xffff) + 1, pr[4], n = size / 4;
    struct BDMAJob *j[4];
    ULONG e[4];

    for (i = 0; i < 4; i++)
        pr[i] = Rnd(3);

    /* RAW: A -> B -> C -> D, submitted back to back, waited for in reverse order */
    for (i = 0; i < n; i++)
    {
        A[i] = Pattern(i, round);
        B[i] = C[i] = Dd[i] = 0;
    }
    j[0] = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, size, BDJ_Priority, pr[0], BDJ_ErrorCode, (ULONG)&e[0], TAG_DONE);
    j[1] = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)b, BDJ_Dst, (ULONG)c, BDJ_Length, size, BDJ_Priority, pr[1], BDJ_ErrorCode, (ULONG)&e[1], TAG_DONE);
    j[2] = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)c, BDJ_Dst, (ULONG)d, BDJ_Length, size, BDJ_Priority, pr[2], BDJ_ErrorCode, (ULONG)&e[2], TAG_DONE);
    for (i = 3; i > 0; i--)
        if (j[i - 1])
            BDMA_WaitJob(j[i - 1]);
        else
            Fail();
    for (i = 0, bad = 0; i < n; i++)
        if (B[i] != A[i] || C[i] != A[i] || Dd[i] != A[i])
            bad++;
    if (bad)
    {
        Fail();
        if (printed++ < 20)
            Printf("FAIL conflict RAW (A>B>C>D, size %ld, priorities %ld %ld %ld): %ld words wrong\n", (LONG)size, (LONG)pr[0], (LONG)pr[1], (LONG)pr[2],
                   (LONG)bad);
    }

    /* WAW: copy A to B, fill B, copy C to B: the last one wins */
    for (i = 0; i < n; i++)
    {
        A[i] = Pattern(i, round + 1);
        C[i] = Pattern(i, round + 2);
        B[i] = 0;
    }
    j[0] = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, size, BDJ_Priority, pr[0], TAG_DONE);
    j[1] = BDMA_AddJobTags(Client, BDJ_FillValue, 0x11223344, BDJ_Dst, (ULONG)b, BDJ_Length, size, BDJ_Priority, pr[1], TAG_DONE);
    j[2] = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)c, BDJ_Dst, (ULONG)b, BDJ_Length, size, BDJ_Priority, pr[2], TAG_DONE);
    for (i = 0; i < 3; i++)
        if (j[i])
            BDMA_WaitJob(j[i]);
        else
            Fail();
    for (i = 0, bad = 0; i < n; i++)
        if (B[i] != C[i])
            bad++;
    if (bad)
    {
        Fail();
        if (printed++ < 20)
            Printf("FAIL conflict WAW (size %ld, priorities %ld %ld %ld): %ld words wrong\n", (LONG)size, (LONG)pr[0], (LONG)pr[1], (LONG)pr[2], (LONG)bad);
    }

    /* WAR: copy A to B, then fill A: B must hold the old A */
    for (i = 0; i < n; i++)
    {
        A[i] = Pattern(i, round + 3);
        B[i] = 0;
    }
    j[0] = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, size, BDJ_Priority, pr[0], TAG_DONE);
    j[1] = BDMA_AddJobTags(Client, BDJ_FillValue, 0x55667788, BDJ_Dst, (ULONG)a, BDJ_Length, size, BDJ_Priority, BDPRI_HIGH, TAG_DONE);
    for (i = 0; i < 2; i++)
        if (j[i])
            BDMA_WaitJob(j[i]);
        else
            Fail();
    for (i = 0, bad = 0; i < n; i++)
        if (B[i] != Pattern(i, round + 3) || A[i] != 0x55667788)
            bad++;
    if (bad)
    {
        Fail();
        if (printed++ < 20)
            Printf("FAIL conflict WAR (size %ld, priority %ld): %ld words wrong\n", (LONG)size, (LONG)pr[0], (LONG)bad);
    }
}

static void PhaseQueue(void)
{
    struct Mem m, q;
    struct Slots sl;
    ULONG round, rounds = 400, bad0 = failed, t0;
    ULONG signals = NotifyPort != NULL;

    Printf("== QUEUE: batches of jobs, priorities, NoWait, notification, abort, order of conflicting jobs\n");

    StatInit(&batchstat);
    nops = nbusy = naborted = nnotify = noksignal = 0;

    sl.size = 0x20000;
    sl.count = 8;
    sl.base = NULL;
    if (!MemGet(&m, 16 * sl.size) || !MemGet(&q, 4 * 0x10000))
    {
        Printf("no memory\n");
        MemPut(&m);
        failed++;
        return;
    }
    sl.base = m.p;


    t0 = timer_now();
    for (round = 0; round < rounds && !Break(); round++)
    {
        Batch(&sl, 1 + Rnd(8), TRUE, signals);
        if ((round & 3) == 0)
        {
            ULONG size = 4 * (1 + Rnd(0x3fff));

            Conflicts(q.p, q.p + 0x10000, q.p + 0x20000, q.p + 0x30000, size);
        }
    }

    Printf("  %ld rounds, %ld jobs verified, %ld refused by NoWait (BUSY), %ld aborted, %ld notifications asked and received, %ld ms, %ld failed\n",
           (LONG)round, (LONG)nops, (LONG)nbusy, (LONG)naborted, (LONG)nnotify, (LONG)((timer_now() - t0) / 1000), (LONG)(failed - bad0));
    if (batchstat.n)
        Printf("  batches: %ld, %ld.%ld MB/s over the whole batch (submission, wait and the cache included)\n", (LONG)batchstat.n,
               (LONG)(Rate10(batchstat.blocks, batchstat.us) / 10), (LONG)(Rate10(batchstat.blocks, batchstat.us) % 10));

    MemPut(&q);
    MemPut(&m);
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* BIG                                                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

static void Row(const char *what, ULONG size, struct Stat *s, ULONG bad)
{
    ULONG best = s->min ? Rate10(s->blocks / s->n + (s->rem != 0), s->min) : 0;
    ULONG avg = Rate10(s->blocks, s->us);

    Printf("  %6ld KB  %-14s %3ld %9ld %9ld %9ld   %5ld.%ld %5ld.%ld   %s\n", (LONG)(size >> 10), (LONG)what, (LONG)s->n, (LONG)s->min,
           (LONG)(s->us / (s->n ? s->n : 1)), (LONG)s->max, (LONG)(best / 10), (LONG)(best % 10), (LONG)(avg / 10), (LONG)(avg % 10),
           (LONG)(bad ? "WRONG" : "ok"));
}

/* Counts the words that differ */
static ULONG Differ(const ULONG *a, const ULONG *b, ULONG words)
{
    ULONG i, bad = 0;

    for (i = 0; i < words; i++)
        if (a[i] != b[i])
            bad++;

    return bad;
}

static void PhaseBig(ULONG maxmb)
{
    struct Mem S, D;
    ULONG size, maxsize = maxmb << 20, reps, r, bad0 = failed, total = 0, temp;
    ULONG *s, *d;

    Printf("== BIG: very big jobs, up to %ld MB (this holds the memory bus for a long time: Ctrl-C stops between two jobs)\n", (LONG)maxmb);

    while (maxsize >= 0x100000)
    {
        if (MemGet(&S, maxsize))
        {
            if (MemGet(&D, maxsize))
                break;
            MemPut(&S);
        }
        maxsize >>= 1;
    }
    if (maxsize < 0x100000)
    {
        Printf("no memory for two blocks of 1 MB\n");
        failed++;
        return;
    }
    if (maxsize != maxmb << 20)
        Printf("  (memory for %ld MB blocks only)\n", (LONG)(maxsize >> 20));

    s = (ULONG *)S.p;
    d = (ULONG *)D.p;

    temp = ReadTemp();
    Printf("  temperature before: ");
    PrintTemp(temp);
    Printf("\n  size        what           n    min us    avg us    max us   MB/s(min) MB/s(avg)  result\n");

    for (size = 0x100000; size <= maxsize && !Break(); size <<= 1)
    {
        struct Stat st;
        ULONG k, bad, a;
        ULONG rows, rlen;

        reps = size <= 0x400000 ? 4 : 2;

        for (k = 0; k < size / 4; k++)
            s[k] = Pattern(k, size >> 20);

        /* one job, one row */
        StatInit(&st);
        for (r = 0, bad = 0; r < reps; r++)
        {
            for (k = 0; k < size / 4; k++)
                d[k] = 0;
            a = timer_now();
            if (BDMA_WaitJob(BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, size, TAG_DONE)) != BDERR_OK)
                bad++;
            StatAdd(&st, size, timer_now() - a);
            bad += Differ(s, d, size / 4);
        }
        if (bad)
            Fail();
        Row("DMA copy", size, &st, bad);
        total += size * reps;

        /* the CPU for comparison */
        StatInit(&st);
        for (r = 0; r < 2; r++)
        {
            a = timer_now();
            CopyMemQuick(s, d, size);
            StatAdd(&st, size, timer_now() - a);
        }
        Row("CopyMemQuick", size, &st, 0);

        /* one job, 1280 rows (the biggest rectangle); the pitch is 64 bytes longer than the row, and the whole rectangle fits in the block */
        rows = 1280;
        rlen = ((size / rows) - 64) & ~15UL;
        StatInit(&st);
        for (r = 0, bad = 0; r < reps; r++)
        {
            ULONG err = 0xdead;
            struct BDMAJob *job;

            for (k = 0; k < size / 4; k++)
                d[k] = 0;
            a = timer_now();
            job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, rlen, BDJ_Rows, rows, BDJ_SrcPitch, rlen + 64,
                                  BDJ_DstPitch, rlen + 64, BDJ_ErrorCode, (ULONG)&err, TAG_DONE);
            if (job == NULL)
            {
                Printf("  the rectangle was refused: error %ld (length %ld, rows %ld, pitch %ld)\n", (LONG)err, (LONG)rlen, (LONG)rows, (LONG)(rlen + 64));
                bad++;
                break;
            }
            if (BDMA_WaitJob(job) != BDERR_OK)
                bad++;
            StatAdd(&st, rlen * rows, timer_now() - a);
        }
        {
            /* only the rows are compared: the 64 bytes between two rows are not written */
            ULONG y, i, pw = (rlen + 64) / 4;

            if (st.n)
                for (y = 0; y < rows; y++)
                    for (i = 0; i < rlen / 4; i++)
                        if (d[y * pw + i] != s[y * pw + i])
                            bad++;
        }
        if (bad)
            Fail();
        Row("DMA 1280 rows", rlen * rows, &st, bad);
        total += rlen * rows * st.n;

        /* a fill */
        StatInit(&st);
        for (r = 0, bad = 0; r < reps; r++)
        {
            for (k = 0; k < size / 4; k++)
                d[k] = 0;
            a = timer_now();
            if (BDMA_WaitJob(BDMA_AddJobTags(Client, BDJ_FillValue, 0xC0FFEE42, BDJ_Dst, (ULONG)d, BDJ_Length, size, TAG_DONE)) != BDERR_OK)
                bad++;
            StatAdd(&st, size, timer_now() - a);
            for (k = 0; k < size / 4; k++)
                if (d[k] != 0xC0FFEE42)
                    bad++;
        }
        if (bad)
            Fail();
        Row("DMA fill", size, &st, bad);
        total += size * reps;

        /* the CPU fill for comparison */
        StatInit(&st);
        a = timer_now();
        for (k = 0; k < size / 4; k++)
            d[k] = 0xC0FFEE42;
        StatAdd(&st, size, timer_now() - a);
        Row("CPU fill", size, &st, 0);

        /* a scroll of the whole block by one row, down then up, as one job of 1279 rows */
        {
            ULONG pw = (rlen ? rlen : 16) / 4;
            ULONG rr = 1279, words = (rr + 1) * pw, i;
            ULONG err = 0xdead;
            struct BDMAJob *job;

            if (words * 4 <= size)
            {
                StatInit(&st);
                for (i = 0; i < words; i++)
                    d[i] = Pattern(i, 5);
                a = timer_now();
                job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)d, BDJ_Dst, (ULONG)d + pw * 4, BDJ_Length, pw * 4, BDJ_Rows, rr, BDJ_SrcPitch, pw * 4,
                                      BDJ_DstPitch, pw * 4, BDJ_Move, TRUE, BDJ_ErrorCode, (ULONG)&err, TAG_DONE);
                bad = job == NULL ? 1 : BDMA_WaitJob(job) != BDERR_OK;
                StatAdd(&st, pw * 4 * rr, timer_now() - a);
                for (i = 0; i < words; i++)
                    if (d[i] != Pattern(i < pw ? i : i - pw, 5))
                        bad++;
                if (bad)
                    Fail();
                Row("DMA scroll down", pw * 4 * rr, &st, bad);
                total += pw * 4 * rr;

                StatInit(&st);
                for (i = 0; i < words; i++)
                    d[i] = Pattern(i, 6);
                a = timer_now();
                job = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)d + pw * 4, BDJ_Dst, (ULONG)d, BDJ_Length, pw * 4, BDJ_Rows, rr, BDJ_SrcPitch, pw * 4,
                                      BDJ_DstPitch, pw * 4, BDJ_Move, TRUE, BDJ_ErrorCode, (ULONG)&err, TAG_DONE);
                bad = job == NULL ? 1 : BDMA_WaitJob(job) != BDERR_OK;
                StatAdd(&st, pw * 4 * rr, timer_now() - a);
                for (i = 0; i < words; i++)
                    if (d[i] != Pattern(i < rr * pw ? i + pw : i, 6))
                        bad++;
                if (bad)
                    Fail();
                Row("DMA scroll up", pw * 4 * rr, &st, bad);
                total += pw * 4 * rr;
            }
        }

        temp = ReadTemp();
        Printf("  (temperature: ");
        PrintTemp(temp);
        Printf(")\n");
    }

    /* the slices at work: both channels hold a low priority job, a small urgent job is submitted behind them; without slices it would wait for the end of one of them */
    if (!Break() && maxsize >= 0x400000)
    {
        UBYTE *tiny = AllocMem(8192, MEMF_ANY | MEMF_CLEAR);
        ULONG half = (maxsize < 0x1000000 ? maxsize : 0x1000000) / 2, t0, thigh = 0, tbig = 0;
        struct BDMAJob *jb1, *jb2, *jh;

        if (tiny != NULL)
        {
            t0 = timer_now();
            jb1 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s, BDJ_Dst, (ULONG)d, BDJ_Length, half, BDJ_Priority, BDPRI_LOW, TAG_DONE);
            jb2 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)s + half, BDJ_Dst, (ULONG)d + half, BDJ_Length, half, BDJ_Priority, BDPRI_LOW, TAG_DONE);
            jh = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)tiny, BDJ_Dst, (ULONG)tiny + 4096, BDJ_Length, 4096, BDJ_Priority, BDPRI_HIGH, TAG_DONE);
            if (jb1 != NULL && jb2 != NULL && jh != NULL)
            {
                if (BDMA_WaitJob(jh) != BDERR_OK)
                    Fail();
                thigh = timer_now() - t0;
                if (BDMA_WaitJob(jb1) != BDERR_OK || BDMA_WaitJob(jb2) != BDERR_OK)
                    Fail();
                tbig = timer_now() - t0;
                Printf("  an urgent 4 KB job behind two low priority jobs of %ld MB: done after %ld us; the big ones after %ld us\n", (LONG)(half >> 20), (LONG)thigh,
                       (LONG)tbig);
            }
            FreeMem(tiny, 8192);
        }
    }

    Printf("  %ld MB moved by the DMA in this phase, %ld failed\n", (LONG)(total >> 20), (LONG)(failed - bad0));

    MemPut(&D);
    MemPut(&S);
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */
/* SOAK                                                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------------------------------ */

static void PhaseSoak(ULONG seconds, ULONG tempmax)
{
    struct Mem m;
    struct Slots sl;
    ULONG start, last, lastops = 0, lastblocks = 0, bad0 = failed, now, temp, maxtemp = 0, paused = 0;
    ULONG batches = 0, elapsed;

    Printf("== SOAK: random jobs for %ld s, one to three in flight, everything verified; pauses above %ld C (Ctrl-C stops)\n", (LONG)seconds,
           (LONG)tempmax);

    for (sl.size = 0x200000; sl.size >= 0x10000; sl.size >>= 1)
        if (MemGet(&m, 6 * sl.size))
            break;
    if (sl.size < 0x10000)
    {
        Printf("no memory\n");
        failed++;
        return;
    }
    sl.base = m.p;
    sl.count = 3;
    Printf("  slots of %ld KB\n", (LONG)(sl.size >> 10));

    StatInit(&batchstat);
    nops = nbusy = naborted = nnotify = noksignal = 0;

    temp = ReadTemp();
    if (temp > maxtemp)
        maxtemp = temp;
    Printf("  temperature at the start: ");
    PrintTemp(temp);
    Printf("\n");

    start = last = timer_now();
    while (!Break() && failed - bad0 < 10)
    {
        now = timer_now();
        elapsed = (now - start) / 1000000UL;
        if (elapsed >= seconds + paused)
            break;

        Batch(&sl, 1 + Rnd(3), FALSE, 0);
        batches++;

        if (now - last >= 5000000UL)
        {
            temp = ReadTemp();
            if (temp > maxtemp)
                maxtemp = temp;
            if (temp && temp / 1000 >= tempmax)
            {
                ULONG waited = 0;

                Printf("  %ld C: pause until it is below %ld C\n", (LONG)(temp / 1000), (LONG)(tempmax - 5));
                while (!Break() && waited < 120 && temp / 1000 >= tempmax - 5)
                {
                    Delay(50);
                    waited++;
                    temp = ReadTemp();
                }
                paused += waited;
            }
            if (now - last >= 10000000UL || 1)
            {
                ULONG dops = nops - lastops, dblocks = batchstat.blocks - lastblocks;

                Printf("  %4ld s: %7ld jobs (+%ld), %ld MB, ", (LONG)elapsed, (LONG)nops, (LONG)dops, (LONG)(batchstat.blocks >> 12));
                Printf("%ld.%ld MB/s over the last interval, ", (LONG)(Rate10(dblocks, now - last) / 10), (LONG)(Rate10(dblocks, now - last) % 10));
                PrintTemp(temp);
                Printf(", %ld failed\n", (LONG)(failed - bad0));
                lastops = nops;
                lastblocks = batchstat.blocks;
            }
            last = timer_now();
        }
    }

    temp = ReadTemp();
    if (temp > maxtemp)
        maxtemp = temp;

    Printf("  %ld batches, %ld jobs verified, %ld MB moved, %ld.%ld MB/s over the batches (verification not counted), temperature at the end ",
           (LONG)batches, (LONG)nops, (LONG)(batchstat.blocks >> 12), (LONG)(Rate10(batchstat.blocks, batchstat.us) / 10),
           (LONG)(Rate10(batchstat.blocks, batchstat.us) % 10));
    PrintTemp(temp);
    Printf(", highest ");
    PrintTemp(maxtemp);
    Printf(", paused %ld s, %ld failed\n", (LONG)paused, (LONG)(failed - bad0));

    MemPut(&m);
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

int main(int argc, struct WBStartup *wbmsg)
{
    struct RDArgs *rda;
    LONG args[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    BOOL paths, queue, big, soak;
    ULONG seconds = 60, mb = 64, tempmax = 80, seed;
    ULONG jobs0 = 0, mb0 = 0, fail0 = 0, jobs1 = 0, mb1 = 0, fail1 = 0, available = 0;

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("PATHS/S,QUEUE/S,BIG/S,SOAK/S,ALL/S,SECONDS/K/N,MB/K/N,TEMPMAX/K/N,SEED/K/N", args, NULL);
    if (rda == NULL)
    {
        Printf("usage: brcm-dma-stress [PATHS] [QUEUE] [BIG] [SOAK] [ALL] [SECONDS n] [MB n] [TEMPMAX n] [SEED n]\n");
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    paths = args[0] || args[4];
    queue = args[1] || args[4];
    big = args[2] || args[4];
    soak = args[3] || args[4];
    if (!paths && !queue && !big && !soak)
        paths = queue = TRUE;
    if (args[5])
        seconds = *(ULONG *)args[5];
    if (args[6])
        mb = *(ULONG *)args[6];
    if (args[7])
        tempmax = *(ULONG *)args[7];
    seed = args[8] ? *(ULONG *)args[8] : timer_now();
    FreeArgs(rda);

    if (seconds > 3600)
        seconds = 3600;     /* the timer wraps every 71 minutes */
    if (mb < 1)
        mb = 1;
    if (seed == 0)
        seed = 1;
    rng = seed;

    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (BrcmDmaBase == NULL)
    {
        Printf("%s is not there\n", (LONG)BRCMDMANAME);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }
    MailboxBase = OpenResource(MAILBOXNAME);

    BDMA_QueryInfoTags(BDI_Available, (ULONG)&available, TAG_DONE);
    if (!available)
    {
        Printf("%s is there, but the engine is not available (run brcm-dma-test STEP 1 to see why)\n", (LONG)BRCMDMANAME);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    Client = BDMA_OpenClientTags(BDC_Name, (ULONG)"brcm-dma-stress", TAG_DONE);
    NotifyPort = CreateMsgPort();
    if (Client == NULL)
    {
        Printf("no client\n");
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    Printf("%s version %ld.%ld, seed %ld, temperature ", (LONG)BRCMDMANAME, (LONG)((struct Library *)BrcmDmaBase)->lib_Version,
           (LONG)((struct Library *)BrcmDmaBase)->lib_Revision, (LONG)seed);
    PrintTemp(ReadTemp());
    Printf("\n");

    ClassesInit();
    StatInit(&batchstat);
    StatInit(&tailstat);

    BDMA_QueryInfoTags(BDI_Jobs, (ULONG)&jobs0, BDI_MegaBytes, (ULONG)&mb0, BDI_Failures, (ULONG)&fail0, TAG_DONE);

    if (paths && !Break())
        PhasePaths();
    if (queue && !Break())
        PhaseQueue();
    if (big && !Break())
        PhaseBig(mb);
    if (soak && !Break())
        PhaseSoak(seconds, tempmax);

    Printf("== Latency of a single job by size (submission to the end of the wait, cache maintenance included)\n");
    ClassesPrint();

    BDMA_QueryInfoTags(BDI_Jobs, (ULONG)&jobs1, BDI_MegaBytes, (ULONG)&mb1, BDI_Failures, (ULONG)&fail1, TAG_DONE);
    Printf("== The resource counted: %ld jobs, %ld MB, %ld failures (ours: %ld checks failed)%s\n", (LONG)(jobs1 - jobs0), (LONG)(mb1 - mb0),
           (LONG)(fail1 - fail0), (LONG)failed, (LONG)(stopped ? ", stopped by Ctrl-C" : ""));
    Printf("%ld failed\n", (LONG)failed);

    BDMA_CloseClient(Client);
    if (NotifyPort != NULL)
        DeleteMsgPort(NotifyPort);
    CloseLibrary((struct Library *)DOSBase);

    return failed ? 5 : 0;
}
