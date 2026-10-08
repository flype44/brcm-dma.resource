/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _BCMDMA_H
#define _BCMDMA_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/interrupts.h>
#include <exec/lists.h>
#include <utility/tagitem.h>
#include <common/compiler.h>


#define BDMA_PRIORITY    117     /* after devicetree.resource (127), gic400.library (126), mailbox.resource (119) and unicam.resource (118) */
#define BDMA_VERSION     0       /* 0.1 stays the version until a program really uses the resource and confirms that it can be used as the Autodocs say */
#define BDMA_REVISION    1

/* The most rows of one rectangle */
#define BDMA_MAX_ROWS    65535

/* The channels the resource keeps: the 40 bit ones that the ARM may use (device tree mask 0x3000: 12 and 13) */
#define BDMA_MAX_CHANNELS 2
#define BDMA_SIZE_BUCKETS 5      /* the histogram of the jobs: up to 64 bytes, 4 KB, 32 KB, 1 MB, more */

/* Up to three control blocks of 32 bytes a row: a head and a tail of 32 bit accesses around a body of 128 bit accesses */
/* A job runs in slices: units of at most BDMA_SLICE_BYTES (a piece of a row), about a megabyte, which is about a millisecond of the channel, and at most
   BDMA_SLICE_UNITS of them in one chain. Between two slices the job waits its turn again in the queue: a more urgent job goes first. */
#define BDMA_DEFAULT_TIMEOUT 1000000UL     /* a slice is a megabyte: a millisecond, thirty at the slowest channel; a second is a channel that is stuck */
#define BDMA_SLICE_BYTES  0x100000UL
#define BDMA_SLICE_UNITS  1280
#define BDMA_MAX_BLOCKS   (3 * BDMA_SLICE_UNITS)
#define BDMA_CHAIN_SIZE   (32 * BDMA_MAX_BLOCKS)

/* A move whose rows overlap themselves (a shift to the side by less than the width) goes through a buffer of the channel: strips of rows are
   copied into it and then out of it. A row must fit (BDMA_BOUNCE_SIZE), a strip is at most BDMA_BOUNCE_ROWS rows (five blocks a row). */
#define BDMA_BOUNCE_SIZE  0x40000UL
#define BDMA_BOUNCE_ROWS  700

/* State of a job */
#define BJS_QUEUED       0
#define BJS_RUNNING      1
#define BJS_DONE         2        /* from here on the job is over: BJS_DONE, BJS_ABORTED, BJS_FAILED */
#define BJS_ABORTED      3
#define BJS_FAILED       4
#define BJS_IDLE         5        /* described and not started (BDJS_IDLE): the states from BJS_DONE on mean "not in flight" */

/* Engine state */
#define BES_NOTSTARTED   0        /* nothing touched yet: the engine starts at the first call that needs it, never in Init() */
#define BES_RUNNING      1
#define BES_FAILED       2

/* Why the engine is not available (bdb_Reason), and what became of each channel (bc_Why): there is no serial line, the test tool prints them */
#define BDW_NONE         0
#define BDW_RESOURCES    1        /* devicetree.resource or mailbox.resource is missing */
#define BDW_DEVICETREE   2        /* no 40 bit DMA node of the BCM2711 in the device tree */
#define BDW_NOCHANNEL    3        /* no channel that the ARM may use, with an interrupt, and not the firmware's */
#define BDW_NOGIC        4        /* gic400.library is missing */
#define BDW_GICOLD       5        /* gic400.library older than 1.3 */
#define BDW_ALLOC        6        /* no GPU memory for the control blocks */
#define BDW_LOCK         7        /* the GPU memory could not be locked, or is not 32 byte aligned */
#define BDW_CHANNELS     8        /* no channel proved itself, see bc_Why */
#define BDW_NOTIMPLEMENTED 9      /* the model is recognised (BCM2835 family) and has no backend yet */

#define BCW_OK           0
#define BCW_ADDINT       1        /* gic400.library refused the interrupt, bc_Error is its answer */
#define BCW_NOINTERRUPT  2        /* the self test did not raise the interrupt */
#define BCW_DATA         3        /* the self test copied wrong data */

/* Which tags were given */
#define BDRF_SRC         (1 << 0)
#define BDRF_DST         (1 << 1)
#define BDRF_FILL        (1 << 2)
#define BDRF_LENGTH      (1 << 3)
#define BDRF_ROWS        (1 << 4)
#define BDRF_SRCPITCH    (1 << 5)
#define BDRF_DSTPITCH    (1 << 6)
#define BDRF_MOVE        (1 << 7)
#define BDRF_NOCACHE     (1 << 8)
#define BDRF_NOWAIT      (1 << 9)
#define BDRF_STRICT      (1 << 10)
#define BDRF_CLASSES     (1 << 11)

/* A job as the tags describe it, once parsed and checked. */
struct BDMARequest
{
    ULONG           bdr_Given;       /* BDRF_* */
    ULONG           bdr_Src;
    ULONG           bdr_Dst;
    ULONG           bdr_Fill;
    ULONG           bdr_Length;
    ULONG           bdr_Rows;
    ULONG           bdr_SrcPitch;
    ULONG           bdr_DstPitch;
    ULONG           bdr_Priority;
    struct MsgPort *bdr_ReplyPort;   /* NULL: the port of the client */
    ULONG *         bdr_ErrorCode;
    ULONG           bdr_Classes;     /* BDCLASS_*: where the job may run */
    ULONG           bdr_Timeout;     /* microseconds a slice may stay on a channel, 0 = no limit */
    ULONG           bdr_Unknown;     /* tags that this version does not know */
};

/* struct BDMAJob and the tags: the public header, with the private part of the job (job-private.h) inside the structure */
#define BDMA_PRIVATE
#include <resources/brcm-dma.h>

/* A client: what BDMA_OpenClientTagList() gives back until BDMA_CloseClient() */
struct BDMAClient
{
    struct Node             bcl_Node;           /* ln_Name is bcl_Name */
    struct BDMABase *       bcl_Base;
    struct MsgPort *        bcl_Port;           /* the default reply port of its jobs */
    BOOL                    bcl_OwnPort;        /* made by BDMA_OpenClientTagList(), deleted by BDMA_CloseClient() */
    ULONG                   bcl_Priority;       /* the default BDPRI_* of its jobs */
    struct MinList          bcl_Jobs;           /* the jobs it owns (bj_Link) */
    char                    bcl_Name[32];
};

/* What the engine knows about itself, for BDMA_QueryInfoTagList() */
struct BDMAStatus
{
    ULONG           bds_Available;
    ULONG           bds_Version;
    ULONG           bds_Interrupt;
    ULONG           bds_Model;
    ULONG           bds_Features;
    ULONG           bds_Channels;
    ULONG           bds_MaxRows;
    ULONG           bds_Jobs;
    ULONG           bds_MegaBytes;
    ULONG           bds_Failures;
    ULONG           bds_Classes;
    ULONG           bds_VideoBase;
    ULONG           bds_VideoSize;
    ULONG           bds_QueueMax;
    ULONG           bds_WaitMaxUs;
    ULONG           bds_Aborts;
    ULONG           bds_Timeouts;
    ULONG           bds_Busy;
    ULONG           bds_Slices;
    ULONG           bds_Deferred;
    ULONG           bds_BusyMs[BDMA_MAX_CHANNELS];
    ULONG           bds_Size[BDMA_SIZE_BUCKETS];
};

struct BDMABase;



/* One of the channels of the pool */
struct BDMAChannel
{
    struct BDMABase *   bc_Base;
    struct Interrupt    bc_Interrupt;       /* is_Data is this structure */
    struct BDMAJob *    bc_Job;             /* the job it runs, NULL when idle */
    ULONG *             bc_Chain;           /* its control blocks (coherent GPU memory, physical address) */
    ULONG *             bc_Bounce;          /* the buffer of the moves that overlap inside a row (BDMA_BOUNCE_SIZE bytes) */
    ULONG *             bc_Constant;        /* the 256 bytes that a fill reads */
    ULONG               bc_Number;          /* the number of the channel: the registers are at 0xf2007000 + 0x100 * number */
    ULONG               bc_Interruptid;     /* the GIC id */
    BOOL                bc_Registered;
    ULONG               bc_Why;             /* BCW_* */
    LONG                bc_Error;
    volatile ULONG      bc_Calls;           /* how many times the interrupt handler was called (diagnostic) */
    ULONG               bc_TestCS;          /* the registers of the channel when the self test gave up, and whether the data arrived */
    ULONG               bc_TestCB;
    ULONG               bc_TestCopied;
    ULONG               bc_Fill;            /* the value that bc_Constant holds (a fill with the same value does not write it again) */
    BOOL                bc_FillSet;
    volatile UBYTE      bc_Starting;        /* picked, but the chain is not armed yet: the watchdog leaves it alone, a stop is settled by the starter */
    ULONG               bc_Start;           /* the timer when the slice that runs went on the channel */
    ULONG               bc_BusyUs;          /* the time its slices ran: milliseconds and the microseconds that do not make one yet */
    ULONG               bc_BusyMs;
};

struct BDMABackend;

struct BDMABase
{
    struct Library          bdb_Node;
    struct ExecBase *       bdb_ExecBase;
    struct SignalSemaphore  bdb_Lock;
    struct Library *        bdb_UtilityBase;     /* opened at the first call: utility.library starts after this resource */

    /* the engine: set up at the first call that needs it (BDMA_StartEngine), under bdb_Lock */
    ULONG                   bdb_State;           /* BES_* */
    ULONG                   bdb_Model;           /* BDM_* */
    const struct BDMABackend *bdb_Backend;       /* the code of the SoC family, chosen when the engine starts */
    ULONG                   bdb_Features;
    ULONG                   bdb_ChannelMask;     /* the device tree mask */
    ULONG                   bdb_Firmware;        /* the mask of the firmware (GET_DMA_CHANNELS) */
    ULONG                   bdb_Reason;          /* BDW_* */
    APTR                    bdb_DeviceTreeBase;
    struct Library *        bdb_MailboxBase;
    struct Library *        bdb_GicBase;
    ULONG                   bdb_Handle;          /* the GPU memory of the control blocks */
    ULONG                   bdb_VCBase;          /* /emu68 vc4-mem: the RTG memory, which exec does not know */
    ULONG                   bdb_VCSize;
    struct BDMAChannel      bdb_Channel[BDMA_MAX_CHANNELS];
    ULONG                   bdb_Channels;        /* how many are in use */

    /* the queue of the jobs that wait, in the order of submission (touched under Disable() and by the interrupt) */
    struct MinList          bdb_Queue;
    ULONG                   bdb_Sequence;
    ULONG                   bdb_Jobs;
    ULONG                   bdb_MegaBytes;
    ULONG                   bdb_ByteRemainder;
    ULONG                   bdb_Failures;
    ULONG                   bdb_Waiting;         /* jobs in the queue now, and the most there ever were */
    ULONG                   bdb_QueueMax;
    ULONG                   bdb_WaitMax;         /* the longest stay in the queue before a slice went on a channel, in microseconds */
    ULONG                   bdb_Aborts;
    ULONG                   bdb_Timeouts;
    ULONG                   bdb_Busy;            /* BDJ_NoWait jobs refused for lack of an idle channel */
    ULONG                   bdb_Slices;          /* times a job went back to the queue for its next slice */
    ULONG                   bdb_Deferred;        /* jobs that held back because of a conflict while a channel was idle */
    ULONG                   bdb_Size[BDMA_SIZE_BUCKETS];
    ULONG                   bdb_Classes;         /* BDCLASS_*: the classes of the channels it manages (set when the engine runs) */
    /* the clients (under bdb_Lock), the jobs that are over and wait for the cache and the reply, the software interrupt that does both */
    struct MinList          bdb_Clients;
    struct MinList          bdb_Done;
    struct Interrupt        bdb_DoneInt;
    struct Interrupt        bdb_TickInt;         /* the watchdog: a vertical blank server, BDJ_Timeout */
};

#define NUMBER_OF_FUNCTIONS 13
/* the table of jumps, rounded up to a multiple of 4: exec wants the library base on a longword boundary (V36 and later) */
#define BASE_NEG_SIZE ((NUMBER_OF_FUNCTIONS * 6 + 3) & ~3)
#define BASE_POS_SIZE ((sizeof(struct BDMABase)))

/* utility.library, opened the first time it is needed (from a task); NULL if it cannot be */
struct Library *BDMA_OpenUtility(struct BDMABase *BDMABase);

/* Walks the list with NextTagItem() of utility.library and fills the request; BDERR_OK if it is complete and consistent */
LONG BDMA_ParseTags(struct Library *UtilityBase, const struct TagItem *tags, struct BDMARequest *request, BOOL keep, ULONG defprio);

/* Writes each value that a query tag asks for through its ULONG *; gives the number of answers */
ULONG BDMA_FillQuery(struct Library *UtilityBase, const struct TagItem *tags, const struct BDMAStatus *status);

/* The queue and the done list hold the bj_Node of the jobs, which is no longer the first member */
#define BDMA_JOBNODE(job)  ((struct Node *)&(job)->bj_Node)
#define BDMA_NODEJOB(node) ((struct BDMAJob *)((UBYTE *)(node) - __builtin_offsetof(struct BDMAJob, bj_Node)))

/* The jobs (job.c) */
LONG BDMA_CheckRequest(struct BDMABase *BDMABase, struct BDMAJob *job);
VOID BDMA_ReplyJob(struct BDMABase *BDMABase, struct BDMAJob *job);
VOID BDMA_AbortInternal(struct BDMABase *BDMABase, struct BDMAJob *job);
VOID BDMA_Release(struct BDMABase *BDMABase, struct BDMAJob *job);
ULONG BDMA_DoneCode(REGARG(struct BDMABase *BDMABase, "a1"));

/* The engine (engine.c) */
BOOL BDMA_StartEngine(struct BDMABase *BDMABase);
VOID BDMA_Run(struct BDMABase *BDMABase);
/* A job enters the queue (the caller has the interrupts off) */
#define BDMA_ENQUEUED(base, job) \
    do { \
        (job)->bj_Queued = timer_now(); \
        (job)->bj_Deferred = 0; \
        if (++(base)->bdb_Waiting > (base)->bdb_QueueMax) (base)->bdb_QueueMax = (base)->bdb_Waiting; \
    } while (0)

BOOL BDMA_KillJob(struct BDMABase *BDMABase, struct BDMAJob *job, LONG error);
VOID BDMA_StartOnChannel(struct BDMABase *BDMABase, struct BDMAChannel *channel, struct BDMAJob *job);
VOID BDMA_ChannelEnded(struct BDMABase *BDMABase, struct BDMAChannel *channel, LONG error);

/* A backend is the code of one SoC family (vc6.c: BCM2711, vc4.c: BCM2835 family, not written). The common engine (engine.c)
   chooses one from the board revision when it starts and calls it through this table.
   Start:  everything the hardware needs (channels, interrupts, memory, self test); FALSE with bdb_Reason when it cannot
   Build:  the chain of the next slice of the job, in the buffer of its channel
   Arm:    starts the channel on that chain
   Stop:   stops a channel and cleans it (abort, watchdog, sweep)
   The backend calls back BDMA_ChannelEnded() at the end of a slice. */
struct BDMABackend
{
    BOOL (*Start)(struct BDMABase *BDMABase);
    VOID (*Build)(struct BDMABase *BDMABase, struct BDMAChannel *channel, struct BDMAJob *job);
    VOID (*Arm)(struct BDMAChannel *channel);
    VOID (*Stop)(struct BDMAChannel *channel);
};

extern const struct BDMABackend BDMA_VC6Backend;
extern const struct BDMABackend BDMA_VC4Backend;
BOOL BDMA_InMemory(struct BDMABase *BDMABase, ULONG address, ULONG bytes);
BOOL BDMA_InVideoMemory(struct BDMABase *BDMABase, ULONG lo, ULONG hi);

VOID kprintf(REGARG(const char * msg, "a0"), REGARG(VOID * args, "a1"));

#define bug(string, ...) \
    do { ULONG args[] = {0, __VA_ARGS__}; kprintf(string, &args[1]); } while(0)

VOID L_BDMA_ResetStatistics(REGARG(struct BDMABase *BDMABase, "a6"));
ULONG L_BDMA_QueryInfoTagList(REGARG(const struct TagItem *tagList, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));
struct BDMAClient * L_BDMA_OpenClientTagList(REGARG(const struct TagItem *tagList, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));
VOID L_BDMA_CloseClient(REGARG(struct BDMAClient *client, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));
struct BDMAJob * L_BDMA_AllocJobTagList(REGARG(struct BDMAClient *client, "a0"), REGARG(const struct TagItem *tagList, "a1"), REGARG(struct BDMABase *BDMABase, "a6"));
LONG L_BDMA_SetJobTagList(REGARG(struct BDMAJob *job, "a0"), REGARG(const struct TagItem *tagList, "a1"), REGARG(struct BDMABase *BDMABase, "a6"));
VOID L_BDMA_StartJob(REGARG(struct BDMAJob *job, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));
VOID L_BDMA_FreeJob(REGARG(struct BDMAJob *job, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));
ULONG L_BDMA_QueryJobTagList(REGARG(struct BDMAJob *job, "a0"), REGARG(const struct TagItem *tagList, "a1"), REGARG(struct BDMABase *BDMABase, "a6"));
struct BDMAJob * L_BDMA_AddJobTagList(REGARG(struct BDMAClient *client, "a0"), REGARG(const struct TagItem *tagList, "a1"), REGARG(struct BDMABase *BDMABase, "a6"));
LONG L_BDMA_WaitJob(REGARG(struct BDMAJob *job, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));
BOOL L_BDMA_CheckJob(REGARG(struct BDMAJob *job, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));
VOID L_BDMA_AbortJob(REGARG(struct BDMAJob *job, "a0"), REGARG(struct BDMABase *BDMABase, "a6"));

#endif /* _BCMDMA_H */
