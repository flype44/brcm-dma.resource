/*
    Copyright © 2025 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/* The resource's own part of struct BDMAJob: included by 
   <resources/brcm-dma.h> inside the structure, only when BDMA_PRIVATE is 
   defined (src/brcm-dma.h defines it). A client sees the message, the error 
   and the bytes done, and nothing after them.
*/
    struct MinNode      bj_Node;            /* in the queue, then in the list of the jobs that are over and wait for their reply */
    struct MinNode      bj_Link;            /* in the list of the jobs of the client */
    struct BDMARequest  bj_Request;
    struct BDMAClient * bj_Client;
    ULONG               bj_Unit;            /* the next unit to run (a piece of a row, see BDMA_SLICE_BYTES) and how many there are */
    ULONG               bj_Units;
    ULONG               bj_Sequence;        /* the order of submission: the contract of the order between jobs */
    struct Task *       bj_Waiter;          /* a task asleep in BDMA_WaitJob() that does not own the reply port: signalled at the reply */
    ULONG               bj_WaitMask;
    ULONG               bj_Queued;          /* the timer when it entered the queue (submission, or back after a slice), 0 until BDMA_Stamp(), and for a job that never waited: the wait statistic */
    UBYTE               bj_Deferred;        /* already counted in bdb_Deferred for this stay in the queue */
    volatile UBYTE      bj_State;           /* BJS_*: changed by the interrupt */
    volatile UBYTE      bj_Active;          /* slices of the job on channels now (0 to the number of channels): a job that can be split has two at a time */
    volatile UBYTE      bj_InQueue;         /* in the queue: while it still has slices to give to a channel (running or not) */
    volatile LONG       bj_Stop;            /* 0, or the error (BDERR_ABORTED, BDERR_TIMEOUT, BDERR_HW) that ended the job early: no more slices, the starter of a slice settles it */
    UBYTE               bj_Bounce;          /* a move whose rows overlap themselves: through the buffer of the channel, a strip of rows at a time */
    UBYTE               bj_Reverse;         /* a move whose destination is above the source: the rows go from the last to the first */
    UBYTE               bj_Test;            /* the self test: no statistics, no reply */
    UBYTE               bj_Auto;            /* made by BDMA_AddJobTagList(): freed by the call that ends it */
    UBYTE               bj_Detached;        /* BDJ_Detached: nobody waits for it, BDMA_WaitClient() (or a later BDMA_AddJobTagList()) frees it */
    UBYTE               bj_Taken;           /* BDMA_WaitJob() gave the result: a second call gives it again */
    UBYTE               bj_ReadUncached;    /* the CPU does not cache what it reads / writes (RTG memory, range of the client): no cache maintenance; decided when the job was checked */
    UBYTE               bj_WriteUncached;
    UBYTE               bj_Stale;           /* a range that the job was checked against was removed since: BDMA_StartJob() refuses it (BDERR_RANGE) */
    ULONG               bj_Width;           /* the geometry that the engine runs, made by BDMA_CheckRequest(): the one of the request, or, when the rows follow each other, one row of all the bytes.
                                               The request is left as the client wrote it (BDMA_SetJobTagList() changes what the tags name, the rest stays) */
    ULONG               bj_Height;
    ULONG               bj_SrcStep;         /* bytes between two rows of the source / of the destination */
    ULONG               bj_DstStep;
    ULONG               bj_ReadLo;          /* the footprint: [lo, hi) read and [lo, hi) written, hi excluded; an empty read for a fill */
    ULONG               bj_ReadHi;
    ULONG               bj_WriteLo;
    ULONG               bj_WriteHi;
