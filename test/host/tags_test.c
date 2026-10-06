/*
    Host test of the TagList code of the resource (src/tags.c, compiled unchanged, with a stand-in of NextTagItem()). The cases of the
    parser are the same as in the Amiga tool. Build and run: test/host/run.sh
*/

#include "brcm-dma.h"

#define MAXTAGS 16

struct Case {
    const char *name;
    struct TagItem tags[MAXTAGS];
    LONG expected;
};

static const struct Case cases[] = {

    /* a rectangle copy: well formed, so it is only "unavailable" while there is no engine */
    { "rectangle copy",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 1024 }, { BDJ_Rows, 16 }, { BDJ_SrcPitch, 2048 }, { BDJ_DstPitch, 2048 }, { TAG_DONE, 0 } },
      BDERR_UNAVAILABLE },
    { "1D copy, pitches and rows left out",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 65536 }, { TAG_DONE, 0 } }, BDERR_UNAVAILABLE },
    { "fill",
      { { BDJ_FillValue, 0x00ff8040 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 4096 }, { BDJ_Rows, 8 }, { BDJ_DstPitch, 7680 }, { TAG_DONE, 0 } },
      BDERR_UNAVAILABLE },
    { "move (a scroll), priority, notification",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x3000 }, { BDJ_Length, 512 }, { BDJ_Rows, 4 }, { BDJ_SrcPitch, 1024 }, { BDJ_DstPitch, 1024 }, { BDJ_Move, TRUE },
        { BDJ_Priority, BDPRI_HIGH }, { BDJ_NotifyTask, 0x10 }, { BDJ_NotifySignals, 0x100 }, { BDJ_NoCache, TRUE }, { TAG_DONE, 0 } },
      BDERR_UNAVAILABLE },
    { "a tag this version does not know is ignored",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDMA_Dummy + 999, 1 }, { TAG_DONE, 0 } }, BDERR_UNAVAILABLE },
    { "... and refused with BDJ_Strict",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDMA_Dummy + 999, 1 }, { BDJ_Strict, TRUE }, { TAG_DONE, 0 } }, BDERR_ARGS },
    { "TAG_IGNORE and TAG_SKIP",
      { { BDJ_Src, 0x1000 }, { TAG_IGNORE, 0 }, { BDJ_Dst, 0x2000 }, { TAG_SKIP, 1 }, { BDJ_Length, 7 }, { BDJ_Length, 64 }, { TAG_DONE, 0 } }, BDERR_UNAVAILABLE },
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
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_Rows, 1281 }, { TAG_DONE, 0 } }, BDERR_TOOBIG },
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
    { "a task without signals",
      { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 64 }, { BDJ_NotifyTask, 0x10 }, { TAG_DONE, 0 } }, BDERR_ARGS },
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))

static ULONG failed;

static void Check(const char *name, ULONG got, ULONG expected)
{
    if (got != expected)
    {
        failed++;
        printf("FAIL %s: %lu, expected %lu\n", name, (unsigned long)got, (unsigned long)expected);
    }
}

int main(void)
{
    ULONG i;
    struct BDMARequest r;
    struct Library *UtilityBase = NULL;     /* the stand-in does not use it */

    for (i = 0; i < NCASES; i++)
    {
        LONG got = BDMA_ParseTags(UtilityBase, cases[i].tags, &r);

        /* the checks give BDERR_OK where the resource then answers "unavailable" */
        LONG expected = cases[i].expected == BDERR_UNAVAILABLE ? BDERR_OK : cases[i].expected;

        Check(cases[i].name, got, expected);
    }

    /* defaults: pitches follow the length, the rows are one, the priority is normal */
    {
        struct TagItem t[] = { { BDJ_Src, 0x1000 }, { BDJ_Dst, 0x2000 }, { BDJ_Length, 4096 }, { TAG_DONE, 0 } };

        Check("defaults", BDMA_ParseTags(UtilityBase, t, &r), BDERR_OK);
        Check("default rows", r.bdr_Rows, 1);
        Check("default source pitch", r.bdr_SrcPitch, 4096);
        Check("default destination pitch", r.bdr_DstPitch, 4096);
        Check("default priority", r.bdr_Priority, BDPRI_NORMAL);
    }

    /* the query: the values go where the tags point, an unknown tag is left alone. A pointer does not fit the 32 bit ti_Data of the
       host, so the places are looked up in a table here; on the Amiga the same code writes through the pointer (brcm-dma-test). */
    {
        struct BDMAStatus s = { 1, 0x00000003, 1, BDM_BCM2711, BDFF_RECT | BDFF_FILL, 0x3000, 1280, 7, 8, 9 };

        /* no place (ti_Data 0) counts nothing: what can be said without a pointer */
        struct TagItem q[] = { { BDI_Version, 0 }, { BDI_Model, 0 }, { BDMA_Dummy + 999, 0 }, { TAG_DONE, 0 } };

        Check("query without places", BDMA_FillQuery(UtilityBase, q, &s), 0);
    }

    printf("%u checks, %u failed\n", (unsigned)(NCASES + 6), (unsigned)failed);
    return failed ? 1 : 0;
}
