/*
    brcm-dma-test -- the test tool of brcm-dma.resource.

    Self checking. Without argument it runs everything; "brcm-dma-test STEP n" runs only the step n (a step that freezes the machine is then
    found by running them one after the other, each from its own command). Exit code 0 if all passed.

      0  the resource is there, its version
      1  BDMA_QueryInfoTagList: what the engine is (this one starts the engine: the channels, the interrupts, the self test)
      2  the TagList: the cases that are refused, and the ones that are accepted (they run, on real memory)
      3  a 1D copy, a rectangle copy, a fill: the memory is compared with a model
      4  a move by whole rows (a scroll), up and down
      5  two jobs at once, and jobs that conflict (the later one reads what the earlier one writes)
      6  abort of a job in the queue, BDMA_CheckJob, BDJ_NotifyTask, the statistics, BDJ_NoWait
      7  speed: 1 MB copied and a fill, the cycles of the 68k counted by the clock of the system (CopyMem for comparison)
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/dos.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>
#include <devices/timer.h>
#include <exec/ports.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/brcm-dma.h>
#include <proto/gic400.h>
#include <libraries/gic400.h>

#include <resources/brcm-dma.h>

#include "hw-vc6.h"
#include "brcm-dma.h"

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
APTR BrcmDmaBase;
struct BDMAClient *Client;
struct Library *GIC400_Base;

#define MAXTAGS 16

/* The memory the jobs work on: three zones far apart, so that a well formed case never overlaps itself */
#define ZONE_A   0x00000
#define ZONE_B   0x20000
#define ZONE_C   0x40000
#define ZONE_SIZE 0x20000
#define ARENA    0x60000

static UBYTE *arena;

struct Case {
    const char *name;
    struct TagItem tags[MAXTAGS];
    LONG expected;
};

/* The addresses of the cases are 0x1000 (zone A), 0x2000 (zone B), 0x3000 (zone C) and 0x1002 (misaligned); they are replaced by the real ones */
static const struct Case cases[] = {

    { "rectangle copy",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 1024 }, { BDJ_Rows, 16 }, { BDJ_SrcPitch, 2048 }, { BDJ_DstPitch, 2048 }, { TAG_DONE, 0 } },
      BDERR_OK },
    { "1D copy, pitches and rows left out",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 65536 }, { TAG_DONE, 0 } }, BDERR_OK },
    { "fill",
      { { BDJ_FillValue, 0x00ff8040 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 4096 }, { BDJ_Rows, 8 }, { BDJ_DstPitch, 7680 }, { TAG_DONE, 0 } },
      BDERR_OK },
    { "move (a scroll), priority, notification",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x3000 }, { BDJ_Length, 512 }, { BDJ_Rows, 4 }, { BDJ_SrcPitch, 1024 }, { BDJ_DstPitch, 1024 }, { BDJ_Move, TRUE },
        { BDJ_Priority, BDPRI_HIGH }, { BDJ_NoCache, FALSE }, { TAG_DONE, 0 } },
      BDERR_OK },
    { "a tag this version does not know is ignored",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDMA_Dummy + 999, 1 }, { TAG_DONE, 0 } }, BDERR_OK },
    { "... and refused with BDJ_Strict",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDMA_Dummy + 999, 1 }, { BDJ_Strict, TRUE }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "TAG_IGNORE and TAG_SKIP",
      { { BDJ_Src, 0x1000 }, { TAG_IGNORE, 0 }, { BDJ_Dst, 0x2000 }, { TAG_SKIP, 1 }, { BDJ_Length, 7 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_OK },
    { "no destination",
      { { BDJ_Src, 0x1000 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "no length",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a source and a value",
      { { BDJ_Src, 0x1000 }, { BDJ_FillValue, 1 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "neither a source nor a value",
      { { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "an address that is not 4 byte aligned",
      { { BDJ_Src, 0x1002 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a length that is not a multiple of 4",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 62 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a pitch smaller than the length",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 128 }, { BDJ_Rows, 2 }, { BDJ_DstPitch, 64 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "too many rows",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Rows, 65536 }, { TAG_DONE, 0 } }, BDERR_TOOBIG },
    { "no rows at all",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Rows, 0 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a source pitch for a fill",
      { { BDJ_FillValue, 0 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_SrcPitch, 64 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a move with different pitches",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x3000 }, { BDJ_Length, 64 }, { BDJ_Rows, 2 }, { BDJ_SrcPitch, 128 }, { BDJ_DstPitch, 256 }, { BDJ_Move, TRUE }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a move of a fill",
      { { BDJ_FillValue, 0 }, { BDJ_Dst, 0x3000 }, { BDJ_Length, 64 }, { BDJ_Move, TRUE }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a priority out of range",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Priority, 9 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "the same memory as source and destination",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x1000 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_OVERLAP },
    { "a destination that is not memory",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x7f000000 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_RANGE },
    { "a class that the engine has",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Classes, BDCLASS_DMA40 | BDCLASS_LITE }, { TAG_DONE, 0 } }, BDERR_OK },
    { "classes that the engine does not have",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Classes, BDCLASS_NORMAL | BDCLASS_LITE }, { TAG_DONE, 0 } }, BDERR_UNAVAILABLE },
    { "an empty set of classes",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Classes, 0 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a class that does not exist",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Classes, 0x10 }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "a shift inside a row",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x1000 + 64 }, { BDJ_Length, 128 }, { BDJ_Rows, 2 }, { BDJ_SrcPitch, 256 }, { BDJ_DstPitch, 256 }, { BDJ_Move, TRUE }, { TAG_DONE, 0 } }, BDERR_OVERLAP },
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))

static ULONG failed;

static void Check(const char *name, LONG got, LONG expected)
{
    if (got != expected)
    {
        failed++;
        Printf("FAIL %s: %ld, expected %ld\n", (LONG)name, got, expected);
    }
}

static ULONG Pattern(ULONG i, ULONG seed)
{
    return (i * 2654435761UL) ^ (seed * 0x01010101UL) ^ 0x5a5a1234UL;
}

static void FillZone(ULONG zone, ULONG bytes, ULONG seed)
{
    ULONG *p = (ULONG *)(arena + zone);
    ULONG i;

    for (i = 0; i < bytes / 4; i++)
        p[i] = Pattern(i, seed);
}

static void ClearZone(ULONG zone, ULONG bytes, ULONG value)
{
    ULONG *p = (ULONG *)(arena + zone);
    ULONG i;

    for (i = 0; i < bytes / 4; i++)
        p[i] = value;
}

/* The job, waited for: BDERR_OK or the error. Asked for the same way every time. */
static LONG Run(const struct TagItem *tags)
{
    ULONG error = 0xdeadbeef;
    struct TagItem list[MAXTAGS + 2];
    ULONG n = 0, k;
    struct BDMAJob *job;

    for (k = 0; tags[k].ti_Tag != TAG_DONE; k++)
        list[n++] = tags[k];
    list[n].ti_Tag = BDJ_ErrorCode;
    list[n++].ti_Data = (ULONG)&error;
    list[n].ti_Tag = TAG_DONE;
    list[n].ti_Data = 0;

    job = BDMA_AddJobTagList(Client, list);

    if (job == NULL)
        return error;

    return BDMA_WaitJob(job);
}

static ULONG Address(ULONG tag, ULONG value)
{
    if (tag != BDJ_Src && tag != BDJ_Dst)
        return value;

    switch (value)
    {
        case 0x1000: return (ULONG)arena + ZONE_A;
        case 0x1002: return (ULONG)arena + ZONE_A + 2;
        case 0x1000 + 64: return (ULONG)arena + ZONE_A + 64;
        case 0x2000: return (ULONG)arena + ZONE_B;
        case 0x3000: return (ULONG)arena + ZONE_C;
        default: return value;
    }
}

static void StepQuery(void)
{
    ULONG available = 0xdead, version = 0xdead, model = 0xdead, features = 0xdead, channels = 0xdead, rows = 0xdead, unknown = 0x12345678;
    ULONG interrupt = 0xdead, answered;

    answered = BDMA_QueryInfoTags(BDI_Available, (ULONG)&available, BDI_Version, (ULONG)&version, BDI_Model, (ULONG)&model,
                             BDI_Features, (ULONG)&features, BDI_Channels, (ULONG)&channels, BDI_MaxRows, (ULONG)&rows,
                             BDI_Interrupt, (ULONG)&interrupt, BDMA_Dummy + 999, (ULONG)&unknown, TAG_DONE);

    Printf("BDMA_QueryInfoTags: %ld answers: %s, interrupt %ld, version %ld.%ld, model %ld, features %08lx, channels %08lx, max rows %ld\n", (LONG)answered,
           (LONG)(available ? "available" : "no engine"), (LONG)interrupt, (LONG)(version >> 16), (LONG)(version & 0xffff), (LONG)model, features,
           channels, (LONG)rows);

    if (!available)
    {
        struct BDMABase *b = (struct BDMABase *)BrcmDmaBase;
        ULONG c;

        Printf("  why not: reason %ld (1 resources, 2 device tree, 3 no channel, 4 no gic, 5 old gic, 6 alloc, 7 lock, 8 channels), device tree mask %08lx, firmware mask %08lx, %ld channels\n",
               (LONG)b->bdb_Reason, b->bdb_ChannelMask, b->bdb_Firmware, (LONG)b->bdb_Channels);
        for (c = 0; c < b->bdb_Channels; c++)
            Printf("  channel %ld: irq %ld, registered %ld, why %ld (1 addint refused, 2 no interrupt, 3 wrong data), error %ld\n", (LONG)b->bdb_Channel[c].bc_Number,
                   (LONG)b->bdb_Channel[c].bc_Interruptid, (LONG)b->bdb_Channel[c].bc_Registered, (LONG)b->bdb_Channel[c].bc_Why, (LONG)b->bdb_Channel[c].bc_Error);
        for (c = 0; c < b->bdb_Channels; c++)
            Printf("  channel %ld: handler called %ld times, at the end of the self test CS %08lx, CB %08lx, data arrived %ld\n", (LONG)b->bdb_Channel[c].bc_Number,
                   (LONG)b->bdb_Channel[c].bc_Calls, b->bdb_Channel[c].bc_TestCS, b->bdb_Channel[c].bc_TestCB, (LONG)b->bdb_Channel[c].bc_TestCopied);

        /* the state of their interrupts in the GIC (read only) */
        GIC400_Base = OpenLibrary("gic400.library", 0);
        if (GIC400_Base != NULL)
        {
            for (c = 0; c < b->bdb_Channels; c++)
            {
                BOOL pending = 0xdead, active = 0xdead, enabled = 0xdead;
                LONG r = GetIntStatus(b->bdb_Channel[c].bc_Interruptid, &pending, &active, &enabled);

                Printf("  irq %ld in the GIC: answer %ld, pending %ld, active %ld, enabled %ld\n", (LONG)b->bdb_Channel[c].bc_Interruptid, r, (LONG)pending, (LONG)active,
                       (LONG)enabled);
            }

            CloseLibrary(GIC400_Base);
        }
    }

    Check("query: answers", answered, 7);
    Check("query: version", version, ((struct Library *)BrcmDmaBase)->lib_Version << 16 | ((struct Library *)BrcmDmaBase)->lib_Revision);
    Check("query: the place of an unknown tag is left alone", unknown, 0x12345678);
    Check("query: no place for the answer", BDMA_QueryInfoTags(BDI_Version, 0, TAG_DONE), 0);
    Check("query: the engine is available", available, TRUE);
    Check("query: the model", model, BDM_BCM2711);
    {
        ULONG classes = 0xdead;

        BDMA_QueryInfoTags(BDI_Classes, (ULONG)&classes, TAG_DONE);
        Check("query: the classes (the 40 bit channels only)", classes, BDCLASS_DMA40);
    }
}

static void StepTags(void)
{
    ULONG i;

    for (i = 0; i < NCASES; i++)
    {
        struct TagItem list[MAXTAGS + 1];
        ULONG k;

        for (k = 0; cases[i].tags[k].ti_Tag != TAG_DONE; k++)
        {
            list[k].ti_Tag = cases[i].tags[k].ti_Tag;
            list[k].ti_Data = Address(list[k].ti_Tag, cases[i].tags[k].ti_Data);
        }
        list[k].ti_Tag = TAG_DONE;
        list[k].ti_Data = 0;

        Check(cases[i].name, Run(list), cases[i].expected);
    }

    /* TAG_MORE: the list goes on in another one */
    {
        ULONG error = 0xdeadbeef;
        struct TagItem second[] = { { BDJ_Length, 4096 }, { BDJ_ErrorCode, (ULONG)&error }, { TAG_DONE, 0 } };
        struct TagItem first[] = { { BDJ_Src, 0 }, { BDJ_Dst, 0 }, { TAG_MORE, (ULONG)second } };
        struct BDMAJob *job;

        first[0].ti_Data = (ULONG)arena + ZONE_A;
        first[1].ti_Data = (ULONG)arena + ZONE_B;

        job = BDMA_AddJobTagList(Client, first);
        Check("TAG_MORE", error, BDERR_OK);
        if (job != NULL)
            BDMA_WaitJob(job);
    }

    /* the variant with a variable number of arguments (generated inline from the SFD) */
    {
        ULONG error = 0xdeadbeef;
        struct BDMAJob *job;

        job = BDMA_AddJobTags(Client, BDJ_FillValue, 0, BDJ_Dst, (ULONG)arena + ZONE_B, BDJ_Length, 256, BDJ_ErrorCode, (ULONG)&error, TAG_DONE);
        Check("BDMA_AddJobTags", error, BDERR_OK);
        if (job != NULL)
            BDMA_WaitJob(job);
    }
}

static void StepCopy(void)
{
    ULONG *a = (ULONG *)(arena + ZONE_A), *b = (ULONG *)(arena + ZONE_B), i, y, bad;

    /* 1D, 64 KB, aligned */
    FillZone(ZONE_A, 65536, 1);
    ClearZone(ZONE_B, 65536, 0);
    Check("1D copy", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)b }, { BDJ_Length, 65536 }, { TAG_DONE, 0 } }), BDERR_OK);
    for (i = 0, bad = 0; i < 65536 / 4; i++)
        if (b[i] != a[i])
            bad++;
    Check("1D copy: longwords that differ", bad, 0);

    /* 1D with a destination that is not on 16 bytes: the head and the tail of 32 bit accesses */
    ClearZone(ZONE_B, 65536, 0);
    Check("1D copy to +4", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)b + 4 }, { BDJ_Length, 1000 }, { TAG_DONE, 0 } }), BDERR_OK);
    for (i = 0, bad = 0; i < 250; i++)
        if (b[1 + i] != a[i])
            bad++;
    Check("1D copy to +4: longwords that differ", bad, 0);
    Check("1D copy to +4: longword before", b[0], 0);
    Check("1D copy to +4: longword after", b[251], 0);

    /* a rectangle: 100 x 37, source pitch 640, destination pitch 1024, destination at +12 */
    FillZone(ZONE_A, 640 * 37, 2);
    ClearZone(ZONE_B, 1024 * 40, 0xAAAAAAAA);
    Check("rectangle", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)b + 12 }, { BDJ_Length, 400 }, { BDJ_Rows, 37 },
                                              { BDJ_SrcPitch, 640 }, { BDJ_DstPitch, 1024 }, { TAG_DONE, 0 } }), BDERR_OK);
    for (y = 0, bad = 0; y < 37; y++)
        for (i = 0; i < 100; i++)
            if (*(ULONG *)((UBYTE *)b + 12 + y * 1024 + i * 4) != *(ULONG *)((UBYTE *)a + y * 640 + i * 4))
                bad++;
    Check("rectangle: longwords that differ", bad, 0);
    Check("rectangle: before the first row", *(ULONG *)((UBYTE *)b + 8), 0xAAAAAAAA);
    Check("rectangle: after the first row", *(ULONG *)((UBYTE *)b + 12 + 400), 0xAAAAAAAA);
    Check("rectangle: between two rows", *(ULONG *)((UBYTE *)b + 1024 + 8), 0xAAAAAAAA);

    /* a fill: 61 rows of 252 bytes (not a multiple of 16) at +20, pitch 512 */
    ClearZone(ZONE_B, 512 * 64, 0x11111111);
    Check("fill", Run((struct TagItem[]){ { BDJ_FillValue, 0xC0FFEE42 }, { BDJ_Dst, (ULONG)b + 20 }, { BDJ_Length, 252 }, { BDJ_Rows, 61 },
                                         { BDJ_DstPitch, 512 }, { TAG_DONE, 0 } }), BDERR_OK);
    for (y = 0, bad = 0; y < 61; y++)
        for (i = 0; i < 63; i++)
            if (*(ULONG *)((UBYTE *)b + 20 + y * 512 + i * 4) != 0xC0FFEE42)
                bad++;
    Check("fill: longwords that differ", bad, 0);
    Check("fill: before", *(ULONG *)((UBYTE *)b + 16), 0x11111111);
    Check("fill: after", *(ULONG *)((UBYTE *)b + 20 + 252), 0x11111111);
    Check("fill: after the last row", *(ULONG *)((UBYTE *)b + 20 + 61 * 512), 0x11111111);

    /* more rows than one chain holds (BDMA_SLICE_UNITS): the job runs in slices, each of them a chain */
    FillZone(ZONE_A, 3000 * 16, 14);
    ClearZone(ZONE_B, 3000 * 16 + 64, 0x22222222);
    Check("3000 rows", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)b }, { BDJ_Length, 16 }, { BDJ_Rows, 3000 },
                                              { BDJ_SrcPitch, 16 }, { BDJ_DstPitch, 16 }, { TAG_DONE, 0 } }), BDERR_OK);
    for (i = 0, bad = 0; i < 3000 * 4; i++)
        if (b[i] != a[i])
            bad++;
    Check("3000 rows: longwords that differ", bad, 0);
    Check("3000 rows: after the last row", b[3000 * 4], 0x22222222);
}

/* A scroll, compared with a model of it made by the CPU: BDJ_Move by whole rows, down (the rows go backwards) and up */
static void StepMove(void)
{
    ULONG *a = (ULONG *)(arena + ZONE_A), *m = (ULONG *)(arena + ZONE_C), i, y, bad;
    ULONG pitch = 256, rows = 64;

    /* down by 8 rows: the destination is above the source and overlaps it */
    FillZone(ZONE_A, pitch * rows, 3);
    for (i = 0; i < pitch * rows / 4; i++)
        m[i] = a[i];
    for (y = 0; y < 40; y++)                    /* the model: from the last row to the first */
        for (i = 0; i < 64; i++)
            m[(39 - y + 8) * 64 + i] = m[(39 - y) * 64 + i];
    Check("move down", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)a + 8 * pitch }, { BDJ_Length, pitch }, { BDJ_Rows, 40 },
                                              { BDJ_SrcPitch, pitch }, { BDJ_DstPitch, pitch }, { BDJ_Move, TRUE }, { TAG_DONE, 0 } }), BDERR_OK);
    for (i = 0, bad = 0; i < pitch * rows / 4; i++)
        if (a[i] != m[i])
            bad++;
    Check("move down: longwords that differ", bad, 0);

    /* up by 5 rows: the destination is below the source */
    FillZone(ZONE_A, pitch * rows, 4);
    for (i = 0; i < pitch * rows / 4; i++)
        m[i] = a[i];
    for (y = 0; y < 40; y++)
        for (i = 0; i < 64; i++)
            m[y * 64 + i] = m[(y + 5) * 64 + i];
    Check("move up", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a + 5 * pitch }, { BDJ_Dst, (ULONG)a }, { BDJ_Length, pitch }, { BDJ_Rows, 40 },
                                            { BDJ_SrcPitch, pitch }, { BDJ_DstPitch, pitch }, { BDJ_Move, TRUE }, { TAG_DONE, 0 } }), BDERR_OK);
    for (i = 0, bad = 0; i < pitch * rows / 4; i++)
        if (a[i] != m[i])
            bad++;
    Check("move up: longwords that differ", bad, 0);

    /* a destination that is not on 16 bytes and a width that is not a multiple of 16 (the head and the tail on every row) */
    FillZone(ZONE_A, pitch * rows, 5);
    for (i = 0; i < pitch * rows / 4; i++)
        m[i] = a[i];
    for (y = 0; y < 30; y++)
        for (i = 0; i < 10; i++)
            m[(29 - y + 6) * 64 + 3 + i] = m[(29 - y) * 64 + 3 + i];
    Check("move down, unaligned", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a + 12 }, { BDJ_Dst, (ULONG)a + 6 * pitch + 12 }, { BDJ_Length, 40 }, { BDJ_Rows, 30 },
                                                         { BDJ_SrcPitch, pitch }, { BDJ_DstPitch, pitch }, { BDJ_Move, TRUE }, { TAG_DONE, 0 } }), BDERR_OK);
    for (i = 0, bad = 0; i < pitch * rows / 4; i++)
        if (a[i] != m[i])
            bad++;
    Check("move down, unaligned: longwords that differ", bad, 0);
}

static void StepTwo(void)
{
    ULONG *a = (ULONG *)(arena + ZONE_A), *b = (ULONG *)(arena + ZONE_B), *c = (ULONG *)(arena + ZONE_C), i, bad;
    ULONG e1 = 0xdead, e2 = 0xdead;
    struct BDMAJob *j1, *j2;

    /* two independent jobs, started one after the other: they run on the two channels */
    FillZone(ZONE_A, 0x8000, 6);
    FillZone(ZONE_A + 0x8000, 0x8000, 7);
    ClearZone(ZONE_B, 0x10000, 0);
    j1 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, 0x8000, BDJ_ErrorCode, (ULONG)&e1, TAG_DONE);
    j2 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a + 0x8000, BDJ_Dst, (ULONG)b + 0x8000, BDJ_Length, 0x8000, BDJ_ErrorCode, (ULONG)&e2, TAG_DONE);
    Check("two jobs: first accepted", e1, BDERR_OK);
    Check("two jobs: second accepted", e2, BDERR_OK);
    if (j1 && j2)
    {
        Check("two jobs: first", BDMA_WaitJob(j1), BDERR_OK);
        Check("two jobs: second", BDMA_WaitJob(j2), BDERR_OK);
    }
    for (i = 0, bad = 0; i < 0x10000 / 4; i++)
        if (b[i] != a[i])
            bad++;
    Check("two jobs: longwords that differ", bad, 0);

    /* the order: the second job reads what the first one writes. Submitted at once, the result must be that of the jobs run one by one:
       A -> B (clear B first so a wrong order shows), then B -> C */
    FillZone(ZONE_A, 0x10000, 8);
    ClearZone(ZONE_B, 0x10000, 0);
    ClearZone(ZONE_C, 0x10000, 0);
    j1 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, 0x10000, BDJ_ErrorCode, (ULONG)&e1, TAG_DONE);
    j2 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)b, BDJ_Dst, (ULONG)c, BDJ_Length, 0x10000, BDJ_Priority, BDPRI_HIGH, BDJ_ErrorCode, (ULONG)&e2, TAG_DONE);
    Check("order: first accepted", e1, BDERR_OK);
    Check("order: second accepted", e2, BDERR_OK);
    if (j1 && j2)
    {
        Check("order: second", BDMA_WaitJob(j2), BDERR_OK);
        Check("order: first", BDMA_WaitJob(j1), BDERR_OK);
    }
    for (i = 0, bad = 0; i < 0x10000 / 4; i++)
        if (c[i] != a[i])
            bad++;
    Check("order: longwords of C that differ from A", bad, 0);

    /* a write after a write to the same place: the last one wins */
    FillZone(ZONE_A, 0x10000, 9);
    j1 = BDMA_AddJobTags(Client, BDJ_FillValue, 0x11111111, BDJ_Dst, (ULONG)b, BDJ_Length, 0x10000, TAG_DONE);
    j2 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, 0x10000, TAG_DONE);
    if (j1 && j2)
    {
        BDMA_WaitJob(j1);
        BDMA_WaitJob(j2);
    }
    for (i = 0, bad = 0; i < 0x10000 / 4; i++)
        if (b[i] != a[i])
            bad++;
    Check("write after write: longwords that differ", bad, 0);
}

static void StepMisc(void)
{
    ULONG *a = (ULONG *)(arena + ZONE_A), *b = (ULONG *)(arena + ZONE_B);
    ULONG e = 0xdead, jobs0 = 0, jobs1 = 0, fail0 = 0, fail1 = 0;
    struct BDMAJob *j1, *j2, *j3;

    BDMA_QueryInfoTags(BDI_Jobs, (ULONG)&jobs0, BDI_Failures, (ULONG)&fail0, TAG_DONE);

    /* abort of a job that waits: j1 and j2 write the same place, so j2 waits for j1; it is aborted at once (nothing printed in between). If it
       was over already (the jobs are short) the fill is in B, else it never ran and B holds the copy. */
    {
        UBYTE state = 99;
        ULONG row, bad = 0;

        /* The first job is slow on purpose (1280 rows of 16 bytes, a control block each: a few hundred microseconds) and BDJ_NoCache makes the
           jobs cheap to submit, so the second one is still in the queue when it is aborted. The state of the job is read before the abort
           (the structure is the resource's own, the test knows it); the cache is written back and dropped by hand, which is what the option
           leaves to the caller. */
        FillZone(ZONE_A, 0x20000, 10);
        ClearZone(ZONE_B, 0x20000, 0);
        CacheClearE(a, 0x20000, CACRF_ClearD);
        CacheClearE(b, 0x20000, CACRF_ClearD);
        j1 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, 16, BDJ_Rows, 1280, BDJ_SrcPitch, 64, BDJ_DstPitch, 64, BDJ_NoCache, TRUE,
                             TAG_DONE);
        j2 = BDMA_AddJobTags(Client, BDJ_FillValue, 0x77777777, BDJ_Dst, (ULONG)b, BDJ_Length, 128, BDJ_NoCache, TRUE, BDJ_ErrorCode, (ULONG)&e, TAG_DONE);
        Check("abort: accepted", e, BDERR_OK);
        if (j2)
        {
            {
                ULONG st = 99;

                BDMA_QueryJobTags(j2, BDJI_State, (ULONG)&st, TAG_DONE);     /* BDJS_QUEUED 0, BDJS_RUNNING 1, BDJS_DONE 2 */
                state = (UBYTE)st;
            }
            BDMA_AbortJob(j2);
        }
        if (j1)
            Check("abort: the first job", BDMA_WaitJob(j1), BDERR_OK);
        CacheClearE(b, 0x20000, CACRF_ClearD);
        Printf("  the second job was %s when it was aborted\n", (LONG)(state == 0 ? "in the queue" : state == 1 ? "running" : "over"));
        for (row = 0; row < 1280; row++)
            if (b[row * 16] != a[row * 16] || b[row * 16 + 3] != a[row * 16 + 3])
                bad++;
        if (state == 0)
            Check("abort in the queue: rows of the copy that differ (the fill never ran)", bad, 0);
        else
            Printf("  (not the case of a job in the queue: %ld rows differ, as the fill ran over the start)\n", (LONG)bad);
        Check("abort: the state was one of the three", state <= 2, TRUE);
    }

    /* BDMA_CheckJob and the reply to a port: the port signals the task, the message is the job */
    {
        struct MsgPort *port = CreateMsgPort();

        if (port != NULL)
        {
            e = 0xdead;
            j3 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, 0x10000, BDJ_ReplyPort, (ULONG)port,
                                 BDJ_ErrorCode, (ULONG)&e, TAG_DONE);
            Check("notify: accepted", e, BDERR_OK);
            if (j3)
            {
                while (!BDMA_CheckJob(j3))
                    Wait(1UL << port->mp_SigBit);

                Check("notify: the job is over", BDMA_CheckJob(j3), TRUE);
                Check("notify: the reply is on the port", (LONG)(port->mp_MsgList.lh_Head->ln_Succ != NULL), TRUE);
                Check("notify: the job gave its result", BDMA_WaitJob(j3), BDERR_OK);
            }
            DeleteMsgPort(port);
        }
    }

    /* BDJ_NoWait with both channels busy: two big jobs on disjoint memory, then a third one that must not wait */
    e = 0xdead;
    j1 = BDMA_AddJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, 16, BDJ_Rows, 1280, BDJ_SrcPitch, 64, BDJ_DstPitch, 64, BDJ_NoCache, TRUE, TAG_DONE);
    j2 = BDMA_AddJobTags(Client, BDJ_FillValue, 3, BDJ_Dst, (ULONG)arena + ZONE_C, BDJ_Length, 16, BDJ_Rows, 1280, BDJ_DstPitch, 64, BDJ_NoCache, TRUE, TAG_DONE);
    j3 = BDMA_AddJobTags(Client, BDJ_FillValue, 1, BDJ_Dst, (ULONG)arena + ZONE_A + 0x1ff00, BDJ_Length, 256, BDJ_NoCache, TRUE, BDJ_NoWait, TRUE,
                         BDJ_ErrorCode, (ULONG)&e, TAG_DONE);
    Printf("  BDJ_NoWait with the channels busy: error %ld (BDERR_BUSY = %ld, BDERR_OK = %ld: the first two jobs may be over already)\n", (LONG)e,
           (LONG)BDERR_BUSY, (LONG)BDERR_OK);
    if (j3)
        e = BDMA_WaitJob(j3);          /* a job that BDJ_NoWait refused is replied with BDERR_BUSY */
    Check("nowait: BUSY or OK", e == BDERR_BUSY || e == BDERR_OK, TRUE);
    if (j1)
        BDMA_WaitJob(j1);
    if (j2)
        BDMA_WaitJob(j2);

    BDMA_QueryInfoTags(BDI_Jobs, (ULONG)&jobs1, BDI_Failures, (ULONG)&fail1, TAG_DONE);
    Printf("  jobs done: %ld more, failures: %ld more\n", (LONG)(jobs1 - jobs0), (LONG)(fail1 - fail0));
    Check("statistics: no failure", fail1 - fail0, 0);
    Check("statistics: jobs counted", jobs1 - jobs0 >= 4, TRUE);
}

static void StepSpeed(void)
{
    UBYTE *a = arena + ZONE_A, *b = arena + ZONE_B;
    ULONG t0, t1, t2, t3;

    FillZone(ZONE_A, 0x20000, 11);

    t0 = timer_now();
    CopyMemQuick(a, b, 0x20000);
    t1 = timer_now();

    Check("speed: DMA copy", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)b }, { BDJ_Length, 0x20000 }, { TAG_DONE, 0 } }), BDERR_OK);
    t2 = timer_now();

    Check("speed: DMA fill", Run((struct TagItem[]){ { BDJ_FillValue, 0 }, { BDJ_Dst, (ULONG)b }, { BDJ_Length, 0x20000 }, { TAG_DONE, 0 } }), BDERR_OK);
    t3 = timer_now();

    Printf("  128 KB in cached Amiga RAM: CopyMemQuick %ld us, DMA copy %ld us, DMA fill %ld us\n", (LONG)(t1 - t0), (LONG)(t2 - t1), (LONG)(t3 - t2));
}

/* a port that causes a software interrupt: the interrupt takes the messages and counts them */
static volatile ULONG softint_count;

static ULONG SoftCode(struct MsgPort *port asm("a1"))
{
    while (GetMsg(port) != NULL)
        softint_count++;

    return 0;
}

static void StepClient(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct Interrupt si;
    struct BDMAClient *c2;
    struct BDMAJob *job;
    ULONG st = 99, done = 0, t0, e = 0xdead;
    UBYTE *a = arena + ZONE_A;

    FillZone(ZONE_A, 0x20000, 12);

    /* 1. a reply port that wakes a software interrupt, and a job started again (the interrupt is what takes the reply) */
    if (port != NULL)
    {
        si.is_Node.ln_Type = NT_INTERRUPT;
        si.is_Node.ln_Pri = 0;
        si.is_Node.ln_Name = (char *)"brcm-dma-test";
        si.is_Data = port;
        si.is_Code = (void (*)())SoftCode;
        port->mp_Flags = PA_SOFTINT;
        port->mp_SigTask = (struct Task *)&si;

        c2 = BDMA_OpenClientTags(BDC_Name, (ULONG)"softint", BDC_ReplyPort, (ULONG)port, TAG_DONE);
        Check("client: opened with a port of its own", c2 != NULL, TRUE);
        if (c2 != NULL)
        {
            job = BDMA_AllocJobTags(c2, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)arena + ZONE_B, BDJ_Length, 4096, BDJ_ErrorCode, (ULONG)&e, TAG_DONE);
            Check("client: job described", e, BDERR_OK);
            Check("job: idle after BDMA_AllocJobTags", BDMA_CheckJob(job), TRUE);
            if (job != NULL)
            {
                ClearZone(ZONE_B, 4096, 0);
                softint_count = 0;
                BDMA_StartJob(job);
                t0 = timer_now();
                while (softint_count < 1 && timer_now() - t0 < 200000)
                    ;
                Check("softint: the reply woke the interrupt", softint_count, 1);
                BDMA_QueryJobTags(job, BDJI_State, (ULONG)&st, BDJI_BytesDone, (ULONG)&done, TAG_DONE);
                Check("job info: state", st, BDJS_DONE);
                Check("job info: bytes done", done, 4096);
                Check("job: data copied", *(ULONG *)(arena + ZONE_B + 4092) == *(ULONG *)(a + 4092), TRUE);

                /* 2. the same job with other values: a wrong change is refused and leaves it as it was */
                Check("set: a new length", BDMA_SetJobTags(job, BDJ_Length, 8192, TAG_DONE), BDERR_OK);
                Check("set: a length that is not a multiple of 4", BDMA_SetJobTags(job, BDJ_Length, 62, TAG_DONE), BDERR_ARGS);
                ClearZone(ZONE_B, 8192, 0);
                BDMA_StartJob(job);
                t0 = timer_now();
                while (softint_count < 2 && timer_now() - t0 < 200000)
                    ;
                Check("softint: the second reply", softint_count, 2);
                BDMA_QueryJobTags(job, BDJI_BytesDone, (ULONG)&done, TAG_DONE);
                Check("job info: bytes done, started again with 8192", done, 8192);
                Check("job: data copied, 8192", *(ULONG *)(arena + ZONE_B + 8188) == *(ULONG *)(a + 8188), TRUE);

                BDMA_FreeJob(job);
            }
            BDMA_CloseClient(c2);
        }
        DeleteMsgPort(port);
    }

}

static void StepClose(void)
{
    struct BDMAClient *c3;
    struct BDMAJob *j1, *j2;
    UBYTE *a = arena + ZONE_A;

    FillZone(ZONE_A, 0x20000, 13);

    /* a client closed with jobs in flight: they are aborted, the resource goes on working */
    c3 = BDMA_OpenClientTags(BDC_Name, (ULONG)"closed too early", TAG_DONE);
    Check("client: another one opened", c3 != NULL, TRUE);
    if (c3 != NULL)
    {
        j1 = BDMA_AllocJobTags(c3, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)arena + ZONE_B, BDJ_Length, 0x20000, TAG_DONE);
        j2 = BDMA_AllocJobTags(c3, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)arena + ZONE_C, BDJ_Length, 0x20000, TAG_DONE);
        if (j1 != NULL && j2 != NULL)
        {
            BDMA_StartJob(j1);
            BDMA_StartJob(j2);
            BDMA_CloseClient(c3);
        }
        else
            BDMA_CloseClient(c3);
    }
    Check("the resource goes on after a client that closed in a hurry",
          Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)arena + ZONE_B }, { BDJ_Length, 4096 }, { TAG_DONE, 0 } }), BDERR_OK);
}

/* any task may wait for any job, a second wait gives the same result, a job that does not end is given up (BDJ_Timeout), the memory of the RTG board */
static void StepWait(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct BDMAJob *job;
    ULONG vbase = 0xdead, vsize = 0xdead, e = 0xdead;
    UBYTE *a = arena + ZONE_A;
    UBYTE *big, *big2;
    ULONG bigsize = 0x2000000;

    FillZone(ZONE_A, 0x20000, 15);

    BDMA_QueryInfoTags(BDI_VideoBase, (ULONG)&vbase, BDI_VideoSize, (ULONG)&vsize, TAG_DONE);
    Printf("  the memory of the RTG board: %08lx, %ld KB\n", vbase, (LONG)(vsize >> 10));
    Check("RTG memory: the memory of the tests is not in it", (ULONG)arena >= vbase + vsize || (ULONG)arena + ARENA <= vbase || vsize == 0, TRUE);

    /* a port that signals nobody (PA_IGNORE): this task is not its owner, it sleeps on a signal of its own; the reply stays on the port */
    if (port != NULL)
    {
        port->mp_Flags = PA_IGNORE;
        job = BDMA_AllocJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)arena + ZONE_B, BDJ_Length, 4096, BDJ_ReplyPort, (ULONG)port, TAG_DONE);
        if (job != NULL)
        {
            BDMA_StartJob(job);
            Check("wait: a port that is not the task's", BDMA_WaitJob(job), BDERR_OK);
            Check("wait: a second call gives the result again", BDMA_WaitJob(job), BDERR_OK);
            Check("wait: the reply is still on that port", GetMsg(port) != NULL, TRUE);
            BDMA_FreeJob(job);
        }
        DeleteMsgPort(port);
    }

    /* the port of the client, waited for twice */
    job = BDMA_AllocJobTags(Client, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)arena + ZONE_B, BDJ_Length, 4096, TAG_DONE);
    if (job != NULL)
    {
        BDMA_StartJob(job);
        Check("wait: a signal port", BDMA_WaitJob(job), BDERR_OK);
        Check("wait: a second call", BDMA_WaitJob(job), BDERR_OK);
        BDMA_StartJob(job);
        Check("wait: after a new start", BDMA_WaitJob(job), BDERR_OK);
        BDMA_FreeJob(job);
    }

    /* a job that does not end in its time: a tick of the watchdog is 20 ms, a 32 MB copy takes 33, a timeout of 1 us cannot be kept */
    big = AllocMem(bigsize, MEMF_ANY);
    big2 = AllocMem(bigsize, MEMF_ANY);
    if (big != NULL && big2 != NULL)
    {
        job = BDMA_AllocJobTags(Client, BDJ_Src, (ULONG)big, BDJ_Dst, (ULONG)big2, BDJ_Length, bigsize, BDJ_Timeout, 1, BDJ_NoCache, TRUE, BDJ_ErrorCode, (ULONG)&e, TAG_DONE);
        Check("timeout: the job is accepted", e, BDERR_OK);
        if (job != NULL)
        {
            BDMA_StartJob(job);
            Check("timeout: given up", BDMA_WaitJob(job), BDERR_TIMEOUT);
            Check("timeout: the channel works again", Run((struct TagItem[]){ { BDJ_Src, (ULONG)a }, { BDJ_Dst, (ULONG)arena + ZONE_B }, { BDJ_Length, 4096 }, { TAG_DONE, 0 } }), BDERR_OK);
            Check("timeout: no limit (0), the same job ends", BDMA_SetJobTags(job, BDJ_Timeout, 0, TAG_DONE), BDERR_OK);
            BDMA_StartJob(job);
            Check("timeout: no limit, done", BDMA_WaitJob(job), BDERR_OK);
            BDMA_FreeJob(job);
        }
    }
    else
        Printf("  (no memory for the timeout: skipped)\n");
    if (big != NULL)
        FreeMem(big, bigsize);
    if (big2 != NULL)
        FreeMem(big2, bigsize);
}

int main(int argc, struct WBStartup *wbmsg)
{
    struct RDArgs *rda;
    LONG args[1] = { 0 };
    LONG only = -1;

    (void)argc;
    (void)wbmsg;

    /* the startup code keeps ExecBase in a local variable of its own: the global one used by the inline calls is set here */
    SysBase = *(struct ExecBase **)4;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

    rda = ReadArgs("STEP/N", args, NULL);
    if (rda != NULL)
    {
        if (args[0] != 0)
            only = *(LONG *)args[0];
        FreeArgs(rda);
    }

    BrcmDmaBase = OpenResource(BRCMDMANAME);
    if (BrcmDmaBase == NULL)
    {
        Printf("%s is not there (not in the ROM of this Emu68?)\n", (LONG)BRCMDMANAME);
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    Printf("%s version %ld.%ld\n", (LONG)BRCMDMANAME, (LONG)((struct Library *)BrcmDmaBase)->lib_Version,
           (LONG)((struct Library *)BrcmDmaBase)->lib_Revision);

    Client = BDMA_OpenClientTags(BDC_Name, (ULONG)"brcm-dma-test", TAG_DONE);
    if (Client == NULL)
    {
        Printf("no client\n");
        CloseLibrary((struct Library *)DOSBase);
        return 10;
    }

    if (only == 0)
    {
        CloseLibrary((struct Library *)DOSBase);
        return 0;
    }

    arena = AllocMem(ARENA, MEMF_ANY | MEMF_CLEAR);
    if (arena == NULL)
    {
        Printf("no memory for the test\n");
        CloseLibrary((struct Library *)DOSBase);
        return 20;
    }

    if (only < 0 || only == 1) { Printf("step 1: query\n"); StepQuery(); }
    if (only < 0 || only == 2) { Printf("step 2: tags\n"); StepTags(); }
    if (only < 0 || only == 3) { Printf("step 3: copy, rectangle, fill\n"); StepCopy(); }
    if (only < 0 || only == 4) { Printf("step 4: move\n"); StepMove(); }
    if (only < 0 || only == 5) { Printf("step 5: two jobs, order\n"); StepTwo(); }
    if (only < 0 || only == 6) { Printf("step 6: abort, check, notify, statistics\n"); StepMisc(); }
    if (only < 0 || only == 7) { Printf("step 7: speed\n"); StepSpeed(); }
    if (only < 0 || only == 8) { Printf("step 8: client, job started again, software interrupt reply\n"); StepClient(); }
    if (only < 0 || only == 10) { Printf("step 10: any task waits, timeout, memory of the RTG board\n"); StepWait(); }
    if (only < 0 || only == 9) { Printf("step 9: a client closed with jobs in flight\n"); StepClose(); }

    Printf("%ld failed\n", (LONG)failed);

    BDMA_CloseClient(Client);
    FreeMem(arena, ARENA);
    CloseLibrary((struct Library *)DOSBase);
    return failed ? 5 : 0;
}
