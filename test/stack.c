/*
    brcm-dma-stack -- the stack that the calls of a hook of VideoCore.card use inside brcm-dma.resource.

    The hooks of the card run in the task of whoever draws, with a small stack (about 250 bytes are left to a hook, according to its notes), and the calls of the
    resource run on that stack. A process with a stack of known size paints it with a pattern, makes the calls as the card does (BDMA_SetJobTags with the six
    pairs of a rectangle, BDMA_StartJob, BDMA_WaitJob), and looks how far down the pattern was overwritten. It is another task than the one that opened the
    client: it also tests that any task may wait for a job.

    The figures are the bytes used BELOW the frame of the function that makes the call: the tag array that the inline stub builds is in that frame (about 56
    bytes for the six pairs) and comes on top, as the frame of the hook itself does. Interrupts do not use this stack.

      brcm-dma-stack
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/brcm-dma.h>

#include <resources/brcm-dma.h>

#include "hw-vc4.h"

struct DosLibrary * DOSBase;
struct ExecBase * SysBase;
APTR BrcmDmaBase;

#define PATTERN     0xA5
#define STACKSIZE   4096

static struct BDMAClient *Client;
static struct BDMAJob *Job;
static struct Task *Parent;
static ULONG Mask;
static UBYTE *Src, *Dst;

#define NMEAS 4
static ULONG used[NMEAS];       /* set, start, wait, the three together (the wait of that one is not counted) */
static ULONG errors[NMEAS];
static ULONG lowest;            /* the size of the stack that the process really had */
static BOOL overflow;

/* Paints the stack below the stack pointer of the function that calls it (inline: its own frame must not come in between) */
#define PAINT(lowerp, spp)                                                         \
    do {                                                                           \
        struct Task *_t = FindTask(NULL);                                          \
        ULONG _sp;                                                                 \
        UBYTE *_p;                                                                 \
        asm volatile("move.l %%a7,%0" : "=r"(_sp));                                \
        *(spp) = _sp & ~3UL;                                                       \
        *(lowerp) = (UBYTE *)_t->tc_SPLower;                                       \
        for (_p = *(lowerp); (ULONG)_p < *(spp); _p++)                             \
            *_p = PATTERN;                                                         \
    } while (0)

/* How many bytes under `sp` were written since the painting; the whole stack and an overflow if the pattern is gone from the bottom */
static ULONG Used(UBYTE *lower, ULONG sp)
{
    UBYTE *p;

    for (p = lower; (ULONG)p < sp && *p == PATTERN; p++)
        ;

    if (p == lower)
        overflow = TRUE;

    return sp - (ULONG)p;
}

__attribute__((used)) ULONG Child(void)
{
    UBYTE *lower;
    ULONG sp, e;
    UBYTE *a = Src, *b = Dst;
    struct Task *me = FindTask(NULL);

    lowest = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;

    /* BDMA_SetJobTags: the six pairs of a rectangle (the job is idle) */
    PAINT(&lower, &sp);
    e = BDMA_SetJobTags(Job, BDJ_Src, (ULONG)a, BDJ_Dst, (ULONG)b, BDJ_Length, 64, BDJ_Rows, 4, BDJ_SrcPitch, 128, BDJ_DstPitch, 128, TAG_DONE);
    used[0] = Used(lower, sp);
    errors[0] = e;

    /* BDMA_StartJob */
    PAINT(&lower, &sp);
    BDMA_StartJob(Job);
    used[1] = Used(lower, sp);

    /* BDMA_WaitJob (the job runs: it sleeps in another task than the one that opened the client) */
    PAINT(&lower, &sp);
    e = BDMA_WaitJob(Job);
    used[2] = Used(lower, sp);
    errors[2] = e;

    /* the three one after the other, the deepest of them: what a hook does */
    PAINT(&lower, &sp);
    e = BDMA_SetJobTags(Job, BDJ_Src, (ULONG)a + 4, BDJ_Dst, (ULONG)b + 4, BDJ_Length, 60, BDJ_Rows, 2, BDJ_SrcPitch, 128, BDJ_DstPitch, 128, TAG_DONE);
    BDMA_StartJob(Job);
    e |= BDMA_WaitJob(Job);
    used[3] = Used(lower, sp);
    errors[3] = e;

    Signal(Parent, Mask);

    return 0;
}

int main(int argc, struct WBStartup *wbmsg)
{
    ULONG available = 0, k, error = 0xdead;
    BYTE bit;
    struct Process *child;
    static const char * const names[NMEAS] = { "BDMA_SetJobTags (6 pairs)", "BDMA_StartJob", "BDMA_WaitJob (another task, asleep)", "Set + Start + Wait" };

    (void)argc;
    (void)wbmsg;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (DOSBase == NULL)
        return 20;

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

    Src = AllocMem(1024, MEMF_ANY | MEMF_CLEAR);
    Dst = AllocMem(1024, MEMF_ANY | MEMF_CLEAR);
    Client = BDMA_OpenClientTags(BDC_Name, (ULONG)"brcm-dma-stack", TAG_DONE);
    if (Src == NULL || Dst == NULL || Client == NULL)
    {
        Printf("no memory or no client\n");
        return 20;
    }
    Job = BDMA_AllocJobTags(Client, BDJ_Src, (ULONG)Src, BDJ_Dst, (ULONG)Dst + 512, BDJ_Length, 64, BDJ_ErrorCode, (ULONG)&error, TAG_DONE);
    if (Job == NULL)
    {
        Printf("no job: error %ld\n", (LONG)error);
        return 20;
    }

    bit = AllocSignal(-1);
    Parent = FindTask(NULL);
    Mask = 1UL << bit;
    SetSignal(0, Mask);

    child = CreateNewProcTags(NP_Entry, (ULONG)Child, NP_StackSize, STACKSIZE, NP_Name, (ULONG)"brcm-dma-stack child", NP_Cli, FALSE, TAG_DONE);
    if (child == NULL)
    {
        Printf("no process\n");
        return 20;
    }
    Wait(Mask);

    Printf("stack of the process: %ld bytes; bytes used under the frame of the caller:\n", (LONG)lowest);
    for (k = 0; k < NMEAS; k++)
        Printf("  %-36s %4ld bytes%s\n", (LONG)names[k], (LONG)used[k], (LONG)(errors[k] ? "   (the call answered an error)" : ""));
    if (overflow)
        Printf("  the pattern is gone from the bottom of the stack: the stack was too small to measure\n");
    Printf("  to this add the tag array that the stub builds in the frame of the caller: 6 pairs and the end, 56 bytes\n");

    BDMA_FreeJob(Job);
    BDMA_CloseClient(Client);
    FreeSignal(bit);
    FreeMem(Src, 1024);
    FreeMem(Dst, 1024);
    CloseLibrary((struct Library *)DOSBase);

    return 0;
}
