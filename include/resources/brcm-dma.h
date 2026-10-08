#ifndef RESOURCES_BRCM_DMA_H
#define RESOURCES_BRCM_DMA_H
/*
**	$VER: brcm-dma.h 0.1
**
**	Public definitions of "brcm-dma.resource", the DMA engine of the Broadcom SoC of the Raspberry Pi
**	(PiStorm / Emu68). Everything goes through TagLists, as with OpenScreenTagList() and GetDTAttrs():
**	BDMA_AddJobTagList() describes a job, BDMA_QueryInfoTagList() asks about the engine. See Autodocs/brcm-dma.doc.
**
*/

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

#ifndef UTILITY_TAGITEM_H
#include <utility/tagitem.h>
#endif

#define BRCMDMANAME "brcm-dma.resource"

/* A client of the resource: opaque, made by BDMA_OpenClientTagList(), given back by BDMA_CloseClient() */
struct BDMAClient;

/* A transfer. It belongs to the client from BDMA_AllocJobTagList() to BDMA_FreeJob(). It is an exec message: the end of the job is a ReplyMsg()
   to its reply port (a PA_SIGNAL port, or a PA_SOFTINT one for an interrupt handler). The rest of the structure is the resource's own. */
struct BDMAJob {
    struct Message  bj_Msg;
    LONG            bj_Error;       /* BDERR_*, valid once the message was replied */
    ULONG           bj_Done;        /* bytes done once the job is over */
#ifdef BDMA_PRIVATE
#include "job-private.h"
#endif
};
struct BDMAJob;

/* The base of the tags of this resource (TAG_USER + a number picked for it, not registered) */
#define BDMA_Dummy           (TAG_USER + 0x0BCD0000)

/* brcm-dma add job */
#define BDJ_Src             (BDMA_Dummy + 1)     /* ULONG, address of the first byte to read (not with BDJ_FillValue) */
#define BDJ_Dst             (BDMA_Dummy + 2)     /* ULONG, address of the first byte to write (required) */
#define BDJ_FillValue       (BDMA_Dummy + 3)     /* ULONG, a value written again and again (instead of BDJ_Src): a pixel of BDJ_FillBytes bytes in its low bytes */
#define BDJ_Length          (BDMA_Dummy + 4)     /* ULONG, bytes of a row (required; a multiple of 4 for a fill) */
#define BDJ_Rows            (BDMA_Dummy + 5)     /* ULONG, number of rows, default 1 */
#define BDJ_SrcPitch        (BDMA_Dummy + 6)     /* ULONG, bytes between two source rows, default BDJ_Length */
#define BDJ_DstPitch        (BDMA_Dummy + 7)     /* ULONG, bytes between two destination rows, default BDJ_Length */
#define BDJ_Move            (BDMA_Dummy + 8)     /* BOOL, the areas may overlap by whole rows (a scroll); equal pitches */
#define BDJ_NoCache         (BDMA_Dummy + 9)     /* BOOL, no cache maintenance (memory that the CPU does not cache) */
#define BDJ_NoWait          (BDMA_Dummy + 10)    /* BOOL, BDERR_BUSY instead of queueing when every channel is busy */
#define BDJ_Priority        (BDMA_Dummy + 11)    /* ULONG, BDPRI_*, default BDPRI_NORMAL */
#define BDJ_ReplyPort       (BDMA_Dummy + 12)    /* struct MsgPort *, replied to when the job is over (default: the port of the client) */
#define BDJ_ErrorCode       (BDMA_Dummy + 14)    /* ULONG *, receives a BDERR_* (BDERR_OK when the job was accepted) */
#define BDJ_Strict          (BDMA_Dummy + 15)    /* BOOL, a tag that this version does not know is DMAERR_ARGS, not ignored */
#define BDJ_Classes         (BDMA_Dummy + 16)    /* ULONG, BDCLASS_*: the classes of channel the job may run on (default: every class that can do it) */
#define BDJ_Timeout         (BDMA_Dummy + 17)    /* ULONG, microseconds: the longest a slice of the job (1 MB at most) may stay on a channel; 0 = no limit; default 1000000. Over it the channel is stopped and the job is BDERR_TIMEOUT */
#define BDJ_FillBytes       (BDMA_Dummy + 18)    /* ULONG, bytes of the pixel that BDJ_FillValue repeats: 1, 2 or 4 (default 4); BDJ_Length is a multiple of it */

/* brcm-dma client (BDMA_OpenClientTagList) */
#define BDC_Name            (BDMA_Dummy + 48)    /* STRPTR, kept for the tools that list the owners (copied) */
#define BDC_ReplyPort       (BDMA_Dummy + 49)    /* struct MsgPort *, the default reply port of the jobs (default: a port of its own, belonging to the task that opened the client) */
#define BDC_Priority        (BDMA_Dummy + 50)    /* ULONG, the default BDPRI_* of the jobs */

/* brcm-dma job info (BDMA_QueryJobTagList): the ULONG * that follows each tag receives the value */
#define BDJI_State          (BDMA_Dummy + 64)    /* BDJS_* */
#define BDJI_Error          (BDMA_Dummy + 65)    /* BDERR_*, once the job is over */
#define BDJI_BytesDone      (BDMA_Dummy + 66)    /* bytes of the job, once it is over */

/* brcm-dma job states */
#define BDJS_QUEUED        0
#define BDJS_RUNNING       1
#define BDJS_DONE          2
#define BDJS_ABORTED       3
#define BDJS_FAILED        4
#define BDJS_IDLE          5   /* described and not started: the states from BDJS_DONE on mean "not in flight" */

/* brcm-dma classes of channel (BDJ_Classes, BDI_Classes) */
#define BDCLASS_DMA40      (1 << 0)   /* the 40 bit channels of the BCM2711: 128 bit accesses, blocks of 1 GB */
#define BDCLASS_NORMAL     (1 << 1)   /* the historic channels of the BCM2835 family */
#define BDCLASS_LITE       (1 << 2)   /* the historic "lite" channels: blocks of 64 KB at most */
#define BDCLASS_ALL        (BDCLASS_DMA40 | BDCLASS_NORMAL | BDCLASS_LITE)

/* brcm-dma priorities */
#define BDPRI_LOW          0
#define BDPRI_NORMAL       1
#define BDPRI_HIGH         2

/* brcm-dma errors */
#define BDERR_OK           0
#define BDERR_UNAVAILABLE  1   /* no engine on this machine, or it did not prove itself */
#define BDERR_ARGS         2   /* a missing, wrong or incompatible tag, alignment, size, pitch */
#define BDERR_RANGE        3   /* outside the memory */
#define BDERR_OVERLAP      4
#define BDERR_TOOBIG       5
#define BDERR_BUSY         6
#define BDERR_ABORTED      7
#define BDERR_TIMEOUT      8
#define BDERR_HW           9

/* brcm-dma info tags */
#define BDI_Available       (BDMA_Dummy + 32)    /* ULONG *, TRUE when an engine runs and proved itself */
#define BDI_Version         (BDMA_Dummy + 33)    /* ULONG *, version of the resource: (version << 16) | revision */
#define BDI_Interrupt       (BDMA_Dummy + 34)    /* ULONG *, TRUE when the end comes by interrupt (BDMA_Wait() sleeps) */
#define BDI_Model           (BDMA_Dummy + 35)    /* ULONG *, BDM_* */
#define BDI_Features        (BDMA_Dummy + 36)    /* ULONG *, BDFF_* */
#define BDI_Channels        (BDMA_Dummy + 37)    /* ULONG *, bit mask of the channels managed */
#define BDI_MaxRows         (BDMA_Dummy + 38)    /* ULONG *, the limit: the most rows a rectangle job may have */
#define BDI_Jobs            (BDMA_Dummy + 39)    /* ULONG *, jobs done since the start */
#define BDI_MegaBytes       (BDMA_Dummy + 40)    /* ULONG *, megabytes moved since the start */
#define BDI_Failures        (BDMA_Dummy + 41)    /* ULONG *, jobs that failed */
#define BDI_Classes         (BDMA_Dummy + 42)    /* ULONG *, BDCLASS_*: the classes of the channels that the resource manages */
#define BDI_VideoBase       (BDMA_Dummy + 43)    /* ULONG *, the memory of the RTG board (the CPU does not cache it: no cache maintenance there); 0 if none */
#define BDI_VideoSize       (BDMA_Dummy + 44)    /* ULONG *, its size */
/* Statistics since the start of the OS (no reset; read twice and subtract) */
#define BDI_QueueMax        (BDMA_Dummy + 45)    /* ULONG *, the most jobs that were ever waiting in the queue at the same time */
#define BDI_WaitMaxUs       (BDMA_Dummy + 46)    /* ULONG *, the longest stay in the queue before a slice went on a channel, in microseconds */
#define BDI_Aborts          (BDMA_Dummy + 47)    /* ULONG *, jobs aborted */
#define BDI_Timeouts        (BDMA_Dummy + 48)    /* ULONG *, slices given up by the watchdog (BDERR_TIMEOUT) */
#define BDI_Busy            (BDMA_Dummy + 49)    /* ULONG *, BDJ_NoWait jobs refused for lack of an idle channel (BDERR_BUSY) */
#define BDI_Slices          (BDMA_Dummy + 50)    /* ULONG *, times a big job went back to the queue for its next slice */
#define BDI_Deferred        (BDMA_Dummy + 51)    /* ULONG *, jobs that held back because of a conflict (order of the footprints) while a channel was idle */
#define BDI_BusyMs0         (BDMA_Dummy + 52)    /* ULONG *, milliseconds the first channel of BDI_Channels ran slices */
#define BDI_BusyMs1         (BDMA_Dummy + 53)    /* ULONG *, the same for the second channel */
#define BDI_Size64          (BDMA_Dummy + 54)    /* ULONG *, jobs done of up to 64 bytes (bytes of a job = length x rows) */
#define BDI_Size4K          (BDMA_Dummy + 55)    /* ULONG *, of up to 4 KB */
#define BDI_Size32K         (BDMA_Dummy + 56)    /* ULONG *, of up to 32 KB */
#define BDI_Size1M          (BDMA_Dummy + 57)    /* ULONG *, of up to 1 MB */
#define BDI_SizeBig         (BDMA_Dummy + 58)    /* ULONG *, of more than 1 MB */
#define BDI_IrqCalls        (BDMA_Dummy + 59)    /* ULONG *, interrupts of the channels that ended a slice */
#define BDI_IrqMCycles      (BDMA_Dummy + 60)    /* ULONG *, what they cost the CPU: megacycles (2^20 cycles), from the counters of Emu68 */
#define BDI_IrqKInstr       (BDMA_Dummy + 61)    /* ULONG *, and 68k instructions in thousands (2^10) */

/* brcm-dma models */
#define BDM_UNKNOWN    0
#define BDM_BCM2711    1  /* Pi 4B, CM4, Pi 400 */
#define BDM_BCM2835    2  /* the family of the Zero, Pi 2, Pi 3 */

/* brcm-dma features bits */
#define BDFB_RECT          (0)
#define BDFB_BIGBLOCKS     (1)
#define BDFB_MOVE          (2)
#define BDFB_WIDE          (3)
#define BDFB_FILL          (4)

/* brcm-dma features flags */
#define BDFF_RECT          (1 << BDFB_RECT)      /* rectangles of more than one row in one job */
#define BDFF_BIGBLOCKS     (1 << BDFB_BIGBLOCKS) /* blocks of 1 GB */
#define BDFF_MOVE          (1 << BDFB_MOVE)      /* overlap by whole rows */
#define BDFF_WIDE          (1 << BDFB_WIDE)      /* 128 bit accesses */
#define BDFF_FILL          (1 << BDFB_FILL)      /* BDJ_FillValue */

#endif /* RESOURCES_BRCM_DMA_H */
