/*
    brcm-dma-lab -- the hardware of every DMA channel of the Pi, looked at and measured: read the registers, then copy memory with one channel
    at a time and compare. It drives the registers itself (no brcm-dma.resource job), so it only touches a channel that is in the mask of
    the firmware, is not the firmware's, and is idle. The resource must not use that channel meanwhile (the engine uses 12 and 13 once started).

      brcm-dma-lab                         LIST: the registers of the channels 0 to 14, class, owner (read only)
      brcm-dma-lab CHAN n [SIZE kb] [WIDE] [ALIAS 0|4|8|12]
      brcm-dma-lab ALL [SIZE kb]           every eligible channel, one after the other (a freeze loses the rest: prefer CHAN)

    CHAN runs a matrix of variants (width, burst, wait for the write responses) on the channel: copy of 4 KB and of SIZE KB (default 256),
    compared with a model, timed. The line of a variant is printed (and flushed) BEFORE it runs: after a freeze the last line says which one.
    WIDE adds the 128 bit accesses of the legacy channels, which froze the machine once at 2.2 GHz (not proven, see CLAUDE.md 4.1): on request only.
    ALIAS is the top nibble of the bus address of the legacy channels (12 = 0xC0000000, the default; 0 = 0x00000000, 4, 8).
    The 2D mode (TDMODE) and the 256 bit accesses are never tried: both froze or timed out. Write the clock and the temperature in the report.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <workbench/startup.h>

#include <proto/exec.h>
#include <proto/dos.h>


#include "hw-vc6.h"
#include "mbox.h"

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;

#define NCHAN        15
#define FIRST40      11

static ULONG ReadReg(ULONG channel, ULONG reg)
{
    return dma_rd(channel, reg);
}

static ULONG Pattern(ULONG i, ULONG seed)
{
    return (i * 2654435761UL) ^ (seed * 0x01010101UL) ^ 0x5a5a1234UL;
}

static void Say(void)
{
    Flush(Output());
}

/* the class of a channel, from its own registers */
static const char *ChanClass(ULONG ch, BOOL *lite)
{
    *lite = FALSE;
    if (ch >= FIRST40)
        return "dma40";
    if (ReadReg(ch, LDEBUG) & (1UL << 28))
    {
        *lite = TRUE;
        return "lite";
    }

    return "normal";
}

static void List(ULONG mask)
{
    ULONG ch;

    Printf("DMA_ENABLE %08lx  INT_STATUS %08lx  firmware mask of the ARM %08lx\n", LE32(*(volatile ULONG *)DMA_ENABLE), LE32(*(volatile ULONG *)DMA_INT_STATUS), mask);
    Printf("ch  class   inmask  CS        CB        DEBUG     owner/state\n");
    for (ch = 0; ch < NCHAN; ch++)
    {
        BOOL lite;
        const char *cl = ChanClass(ch, &lite);
        ULONG cs = ReadReg(ch, DMA_CS), cb = ReadReg(ch, DMA_CB);
        ULONG dbg = ReadReg(ch, ch >= FIRST40 ? DMA_DEBUG : LDEBUG);

        Printf("%2ld  %-6s  %-6s  %08lx  %08lx  %08lx  %s%s\n", (LONG)ch, (LONG)cl, (LONG)((mask >> ch) & 1 ? "yes" : "no"), cs, cb, dbg,
               (LONG)((cs & CS_DISDEBUG) && ch >= FIRST40 ? "firmware (DISDEBUG) " : ""), (LONG)((cs & L_ACTIVE) || cb ? "BUSY" : "idle"));
    }
    Say();
}

struct Variant {
    const char *name;
    ULONG ti;           /* legacy: the TI; dma40: the info word of source and destination */
};

/* legacy: burst field in the bits 15:12 */
#define LBURST(n) ((ULONG)(n) << 12)
#define BASE_L  (L_TI_SINC | L_TI_DINC)

static const struct Variant legacy[] = {
    { "32 bit burst 0",            BASE_L },
    { "32 bit burst 3",            BASE_L | LBURST(3) },
    { "32 bit burst 7",            BASE_L | LBURST(7) },
    { "32 bit burst 15",           BASE_L | LBURST(15) },
    { "32 bit burst 0 waitresp",   BASE_L | L_TI_WAITRESP },
    { "32 bit burst 7 waitresp",   BASE_L | LBURST(7) | L_TI_WAITRESP },
};
static const struct Variant legacywide[] = {
    { "128 bit burst 0",           BASE_L | L_TI_SWIDTH | L_TI_DWIDTH },
    { "128 bit burst 7",           BASE_L | L_TI_SWIDTH | L_TI_DWIDTH | LBURST(7) },
    { "128 bit burst 15",          BASE_L | L_TI_SWIDTH | L_TI_DWIDTH | LBURST(15) },
};
static const struct Variant dma40v[] = {
    { "32 bit burst 0",            INFO_INC },
    { "32 bit burst 8",            INFO_INC | (7UL << 8) },
    { "128 bit burst 0",           INFO_INC | (2UL << 13) },
    { "128 bit burst 8",           INFO_INC | (2UL << 13) | (7UL << 8) },
    { "128 bit burst 16",          INFO_128_BURST16 | INFO_INC },
};

static ULONG us_since(ULONG t0)
{
    return timer_now() - t0;
}

/* One copy on a channel; returns 0 ok, 1 wrong data, 2 timeout (channel aborted), 3 error flag. *us is the time, *cs and *dbg the registers at the end */
static ULONG Run(ULONG ch, BOOL lite, ULONG ti, ULONG alias, ULONG *cb, ULONG cbbus, UBYTE *s, UBYTE *d, ULONG size, ULONG *us, ULONG *cs, ULONG *dbg, ULONG *bad)
{
    ULONG i, t0, state = 0, n = size / 4;
    ULONG *sw = (ULONG *)s, *dw = (ULONG *)d;

    (void)lite;
    for (i = 0; i < n; i++)
    {
        sw[i] = Pattern(i, size);
        dw[i] = 0xdeadbeef;
    }
    CacheClearE(s, size, CACRF_ClearD);
    CacheClearE(d, size, CACRF_ClearD);

    if (ch >= FIRST40)
    {
        cb[0] = LE32(0);
        cb[1] = LE32((ULONG)s);
        cb[2] = LE32(ti);
        cb[3] = LE32((ULONG)d);
        cb[4] = LE32(ti);
        cb[5] = LE32(size);
        cb[6] = LE32(0);
        cb[7] = LE32(0);
    }
    else
    {
        cb[0] = LE32(ti);
        cb[1] = LE32(alias | (ULONG)s);
        cb[2] = LE32(alias | (ULONG)d);
        cb[3] = LE32(size);
        cb[4] = LE32(0);
        cb[5] = LE32(0);
        cb[6] = LE32(0);
        cb[7] = LE32(0);
    }
    CacheClearE(cb, DMA_CB_BYTES, CACRF_ClearD);

    t0 = timer_now();
    if (ch >= FIRST40)
    {
        dma_wr(ch, DMA_CS, CS_END | CS_PROT);
        dma_wr(ch, DMA_CB, cbbus >> 5);
        dma_wr(ch, DMA_CS, CS_WAIT_FOR_WRITES | CS_ACTIVE | CS_PROT);
    }
    else
    {
        dma_wr(ch, DMA_CS, L_END | L_INT);
        dma_wr(ch, DMA_CB, cbbus);
        dma_wr(ch, DMA_CS, L_WAITWRITES | L_ACTIVE);
    }

    while (us_since(t0) < 500000UL)
    {
        ULONG c = dma_rd(ch, DMA_CS);

        if ((c & L_END) && !(c & L_ACTIVE))
            break;
    }
    *us = us_since(t0);
    *cs = dma_rd(ch, DMA_CS);
    *dbg = dma_rd(ch, ch >= FIRST40 ? DMA_DEBUG : LDEBUG);

    if (!((*cs & L_END) && !(*cs & L_ACTIVE)))
    {
        /* did not finish: abort and reset the channel */
        if (ch >= FIRST40)
        {
            dma_wr(ch, DMA_CS, dma_rd(ch, DMA_CS) & ~CS_ACTIVE);
            for (i = 0; i < 1000 && (dma_rd(ch, DMA_CS) & CS_TRANSACTIONS); i++)
                ;
            dma_wr(ch, DMA_CS, CS_PROT);
            dma_wr(ch, DMA_DEBUG, dma_rd(ch, DMA_DEBUG) | DEBUG_RESET);
        }
        else
        {
            dma_wr(ch, DMA_CS, L_ABORT);
            for (i = 0; i < 1000; i++)
                ;
            dma_wr(ch, DMA_CS, L_RESET);
        }
        state = 2;
    }
    else if (ch >= FIRST40 ? (*cs & CS_ERROR) != 0 : (*cs & L_ERROR) != 0)
        state = 3;

    if (ch >= FIRST40)
        dma_wr(ch, DMA_CS, CS_INT | CS_END | CS_PROT);
    else
        dma_wr(ch, DMA_CS, L_END | L_INT);

    CacheClearE(d, size, CACRF_ClearD);
    *bad = 0;
    for (i = 0; i < n; i++)
        if (dw[i] != Pattern(i, size))
            (*bad)++;
    if (state == 0 && *bad)
        state = 1;

    return state;
}

static ULONG Rate10(ULONG bytes, ULONG us)
{
    return us ? bytes * 10 / us : 0;
}

/* The matrix on one channel; returns the number of variants that failed */
static ULONG Bench(struct Library *mb, ULONG ch, ULONG mask, ULONG kb, BOOL wide, ULONG alias)
{
    BOOL lite;
    const char *cl = ChanClass(ch, &lite);
    ULONG cs0 = ReadReg(ch, DMA_CS), cb0 = ReadReg(ch, DMA_CB), handle, bus, failed = 0, v, nv, pass, best = 0;
    const struct Variant *vars;
    const char *bestname = "";
    ULONG *cb;
    UBYTE *raw, *s, *d;
    ULONG sizes[2], size;

    Printf("== channel %ld (%s)\n", (LONG)ch, (LONG)cl);
    if (!((mask >> ch) & 1))
    {
        Printf("   not in the mask of the firmware: skipped\n");
        return 0;
    }
    if (ch >= FIRST40 && (cs0 & CS_DISDEBUG))
    {
        Printf("   DISDEBUG: a channel of the firmware, skipped\n");
        return 0;
    }
    if ((cs0 & L_ACTIVE) || cb0 != 0)
    {
        Printf("   busy (CS %08lx, CB %08lx): somebody uses it, skipped\n", cs0, cb0);
        return 0;
    }
    if (ch < FIRST40 && !((LE32(*(volatile ULONG *)DMA_ENABLE) >> ch) & 1))
    {
        Printf("   not enabled in DMA_ENABLE: skipped\n");
        return 0;
    }

    handle = AllocateMemory(mb, 256, 32, MEM_FLAG_DIRECT | MEM_FLAG_COHERENT | MEM_FLAG_HINT_PERMALOCK);
    bus = handle == 0xffffffffUL ? 0 : LockMemory(mb, handle);
    if (bus == 0)
    {
        Printf("   no GPU memory for the control block\n");
        return 1;
    }
    cb = (ULONG *)(bus & 0x3fffffffUL);

    sizes[0] = 4096;
    sizes[1] = kb * 1024;
    if (lite && sizes[1] > 65532)
        sizes[1] = 65532;       /* 16 bit length of the lite channels */

    raw = AllocMem(2 * sizes[1] + 64, MEMF_FAST | MEMF_PUBLIC);
    if (raw == NULL)
        raw = AllocMem(2 * sizes[1] + 64, MEMF_ANY);
    if (raw == NULL)
    {
        Printf("   no memory\n");
        ReleaseMemory(mb, handle);
        return 1;
    }
    s = (UBYTE *)(((ULONG)raw + 15) & ~15UL);
    d = s + sizes[1];
    d = (UBYTE *)(((ULONG)d + 15) & ~15UL);

    for (pass = 0; pass < (ch < FIRST40 && wide ? 2U : 1U); pass++)
    {
        vars = ch >= FIRST40 ? dma40v : pass ? legacywide : legacy;
        nv = ch >= FIRST40 ? sizeof(dma40v) / sizeof(dma40v[0]) : pass ? sizeof(legacywide) / sizeof(legacywide[0]) : sizeof(legacy) / sizeof(legacy[0]);

        for (v = 0; v < nv; v++)
            for (size = 0; size < 2; size++)
            {
                ULONG us, cs, dbg, bad, st, bytes = sizes[size];

                Printf("   %-26s %7ld B: ", (LONG)vars[v].name, (LONG)bytes);
                Say();
                st = Run(ch, lite, vars[v].ti, alias, cb, ch >= FIRST40 ? (ULONG)cb : bus, s, d, bytes, &us, &cs, &dbg, &bad);
                if (st == 0)
                {
                    ULONG r = Rate10(bytes, us);

                    Printf("ok %7ld us  %5ld.%ld MB/s\n", (LONG)us, (LONG)(r / 10), (LONG)(r % 10));
                    if (size == 1 && r > best)
                    {
                        best = r;
                        bestname = vars[v].name;
                    }
                }
                else
                {
                    failed++;
                    Printf("%s after %ld us, %ld words wrong, CS %08lx DEBUG %08lx\n", (LONG)(st == 1 ? "WRONG" : st == 2 ? "TIMEOUT (aborted)" : "ERROR"), (LONG)us,
                           (LONG)bad, cs, dbg);
                }
                Say();
            }
    }

    Printf("   channel %ld best: %s, %ld.%ld MB/s; %ld variants failed\n", (LONG)ch, (LONG)bestname, (LONG)(best / 10), (LONG)(best % 10), (LONG)failed);
    Say();

    FreeMem(raw, 2 * sizes[1] + 64);
    ReleaseMemory(mb, handle);

    return failed;
}

int main(int argc, struct WBStartup *wbmsg)
{
    struct RDArgs *rda;
    LONG args[6] = { 0, 0, 0, 0, 0, 0 };
    struct Library *mb;
    ULONG mask, ch, kb = 256, alias = 0xC0000000UL, failed = 0;
    BOOL wide;

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("LIST/S,CHAN/K/N,SIZE/K/N,WIDE/S,ALIAS/K/N,ALL/S", args, NULL);
    if (rda == NULL)
    {
        Printf("usage: brcm-dma-lab [LIST] [CHAN n] [ALL] [SIZE kb] [WIDE] [ALIAS 0|4|8|12]\n");
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }
    if (args[2])
        kb = *(ULONG *)args[2];
    wide = args[3] != 0;
    if (args[4])
        alias = *(ULONG *)args[4] << 28;
    if (kb < 4)
        kb = 4;

    mb = OpenResource(MAILBOXNAME);
    if (mb == NULL)
    {
        Printf("%s is not there\n", (LONG)MAILBOXNAME);
        FreeArgs(rda);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }
    mask = GetDMAChannels(mb);

    if (!args[1] && !args[5])
        List(mask);
    else
    {
        List(mask);
        if (wide)
            Printf("WIDE: 128 bit accesses on the legacy channels, which froze the machine once at 2.2 GHz\n");
        Printf("alias %08lx for the legacy channels, size %ld KB\n", alias, (LONG)kb);
        if (args[1])
        {
            ch = *(ULONG *)args[1];
            if (ch >= NCHAN)
                Printf("channel 0 to 14\n");
            else
                failed += Bench(mb, ch, mask, kb, wide, alias);
        }
        else
            for (ch = 0; ch < NCHAN; ch++)
                failed += Bench(mb, ch, mask, kb, wide, alias);
    }

    Printf("%ld failed\n", (LONG)failed);
    FreeArgs(rda);
    CloseLibrary((struct Library *)DOSBase);

    return failed ? 5 : 0;
}
