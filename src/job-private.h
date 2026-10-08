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
    ULONG               bj_Queued;          /* the timer when it entered the queue (submission, or back after a slice): the wait statistic */
    UBYTE               bj_Deferred;        /* already counted in bdb_Deferred for this stay in the queue */
    ULONG               bj_Start;           /* the timer when the slice that runs went on its channel */
    volatile UBYTE      bj_State;           /* BJS_*: changed by the interrupt */
    volatile UBYTE      bj_Starting;        /* picked, on its channel (BJS_RUNNING), but the chain is not armed yet: the watchdog leaves it alone, an abort is deferred */
    volatile UBYTE      bj_AbortReq;        /* an abort came while it was starting: the starter ends the job instead of arming the channel */
    BYTE                bj_Channel;         /* index in bdb_Channel while it runs */
    UBYTE               bj_Reverse;         /* a move whose destination is above the source: the rows go from the last to the first */
    UBYTE               bj_Test;            /* the self test: no statistics, no reply */
    UBYTE               bj_Auto;            /* made by BDMA_AddJobTagList(): freed by the call that ends it */
    UBYTE               bj_Taken;           /* BDMA_WaitJob() gave the result: a second call gives it again */
    ULONG               bj_ReadLo;          /* the footprint: [lo, hi) read and [lo, hi) written, hi excluded; an empty read for a fill */
    ULONG               bj_ReadHi;
    ULONG               bj_WriteLo;
    ULONG               bj_WriteHi;
