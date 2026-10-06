# bcmdma.resource: architecture review

Status: the five decisions of 7 were **validated by the author on 2026-10-06**; section 9 is the ABI that follows, **implemented on 2026-10-06** (commit d3b04b7, checked on the machine; the Autodocs, the specification, README.md and Implementation.md still describe the old five functions). Sections 0 to 8 are the review of 2026-10-05. It challenges the current implementation and sketches the interface the resource
should offer to other components, in an AmigaOS idiom. `docs/bcmdma.doc` stays the specification of what is written today;
`include/resources/bcmdma.h` is the author's own file and is not changed by this document.

The review came out of a survey of the modules of the PiStorm / Emu68 ecosystem (see 1) and a reading of the submit, wait and
interrupt code (`src/bdma_addjobtaglist.c`, `src/bdma_waitjob.c`, `src/engine.c`). Facts about other drivers were read in their
sources by a helper session and were not all re-checked by hand; they are marked as such.

## 0. Scope: what the hardware offers, not what today's drivers use

Decided by the author (2026-10-05): the purpose of the resource is to offer to the developers **everything that the DMA hardware of the Raspberry Pi can do**, without a design limit
set by the existing drivers. They never used this resource because it did not exist; they do not define its scope. If it had existed, a driver should have found in it what it needs to drive
the Pi from AmigaOS / Emu68. So the test of a design is not "which client needs it today" but "does the interface leave out something the hardware can do".

Consequences for this review:
- section 1 lists clients as **illustrations of the hardware features**, not as the requirements; the absence of a client today is not a reason to leave a feature out of the design
  (it can be left out of the first implementation, never out of the shape of the interface);
- the features of the hardware are to be inventoried from authoritative sources (the BCM2711 peripherals manual, the Linux driver read for facts only, the laboratory notes, the device tree)
  before the tag list is frozen: an inventory is the next document to write (`docs/hardware-capabilities.md`);
- the interface must be layered so that nothing is out of reach (4, "Layers").

## 1. Who could be a client (illustrations of the hardware features, not the scope: see 0)

| Module | How it moves data today | What it would ask of the resource |
|---|---|---|
| `VideoCore.card` | own DMA code (`dma40.c`), 2D rectangle copies from 32 KB, fills off by default, completion by task signal | already covered; the first client (step 6 of the plan) |
| `emu68hdmi.audio` (AHI) | own legacy channel chosen among 0 to 7, two control blocks pointing at each other (a ring), DREQ 10, 32 bit, fixed destination `0x7ef2001c`, no interrupt: a process polls every 1 ms inside Forbid / Disable | a cyclic job, a software interrupt per period, the current position |
| `brcm-emmc.device` (Pi 4, CM4) | `main`: CPU PIO, 512 byte blocks, polled. Branch `DMA_mode` (`d67fa90`, `c052870`, "DMA WiP, still not very stable"): the **ADMA2 engine of the controller itself** (descriptor table, `EMMC_ADMA_SA`), interrupt through `gic400.library`, switched by the device tree property `use-dma` | **no natural need** (the controller is a bus master, see 8); a possible client only for the buffers that ADMA2 does not take (not 32 bit aligned, below `0x01000000` or from `0x80000000`: they fall back to PIO, `emmc.c`): bounce through an aligned buffer and an asynchronous copy by this resource, worth it only for big multi block transfers (fixed cost of a job); to measure how many real transfers fall back to PIO first |
| `brcm-sdhc.device` (Pi 3, Zero 2) | CPU PIO, 512 byte blocks, polled; FIFO at offset 0x40 (SDHOST); no `dma` parameter in `sdhc.dtbo`, nothing in the code | the only SD case that would need the SoC DMA (DREQ, fixed FIFO); but this resource has no backend for the BCM2835 family yet |
| `WiFiPi.device` | SDIO through the Arasan EMMC controller, PIO, small packets | little: the packets are small |
| `minigl.library` | prebuilt backend, sources not available | unknown |
| `genet`, `nvme`, `xhci`, `bcmpcie`, `unicam` | bus masters or controllers with their own DMA | not clients |
| `i2c.library` | transfers too small | not a client |

The DREQ numbers of the SD controllers are not in the sources and are to be verified (device tree `dmas` property, BCM2711 manual).
DREQ 10 with 32 bit accesses and wait for write response is already in use by the AHI driver on a legacy channel.

## 2. Findings in the current implementation

| # | Finding | Where | Consequence |
|---|---|---|---|
| 1 | The end of a job is notified with `Signal(struct Task *)` | `engine.c` (notification) | A task that is gone before the end makes the interrupt signal freed memory. No notification at software interrupt level, which the AHI and the SD drivers need. |
| 2 | The job is allocated by the resource at every submission (`AllocVec`) and freed only by `BDMA_WaitJob` / `BDMA_AbortJob` | `bdma_addjobtaglist.c` | A client that does not wait leaks. Nothing ties a job to its owner. |
| 3 | A job cannot be submitted outside a task | lazy `OpenLibrary("utility.library")` (`utility.c`), `AllocVec`, semaphore | No submission of the next job from a software interrupt. |
| 4 | The cache maintenance after the transfer is done by `BDMA_WaitJob` | `bdma_waitjob.c` | `BDMA_CheckJob` answers TRUE while the CPU may still read stale data; a client that completes by interrupt cannot do it. |
| 5 | One waiter per job (`bj_Waiter`), and a busy loop when `AllocSignal` fails | `bdma_waitjob.c` | Not system friendly; the failure shows late. |
| 6 | Priorities reorder the queue, not the running jobs | `engine.c` | A 64 MB job holds a channel for 66 ms (measured). With two channels a high priority refill can wait that long. Slicing of big jobs is a design, not code. |
| 7 | No central arbiter of the channels | the AHI driver picks a channel by itself among 0 to 7; `VideoCore.card` holds channel 12 | Collisions as soon as the resource manages more channels. |
| 8 | A fixed cost of about 300 us per job, never taken apart | tag parsing, `AllocVec`, cache over the whole footprint, `Disable`, the sleeping wait | Rules out small transfers (SD blocks of 512 bytes, network packets). |

Minor: the engine starts at the first call, so the first client pays for the self test and learns late that the engine is
unavailable. The address contract (the addresses are the ones the CPU uses, identity mapped under Emu68) should be written down.

## 3. What is hard to change later, and what is not

**To settle before the first client** (changing it afterwards breaks every client):
1. the name of the resource (the `OpenResource()` string is ABI);
2. the model of the end of a job;
3. who allocates and owns the job;
4. a session object per client;
5. the policy on the ownership of the channels.

**Additive, breaks nobody**: the individual tags (`BDJ_Segments`, `BDJ_After`, `BDJ_Timeout`, widths, bursts, strides...) and new functions at the end of the table. What is **not** additive is the shape that
carries them: the channel classes with their capability query, the raw chain layer next to the jobs, the peripheral id scheme, the ring with its interrupt and position (see 0 and "Layers").

## 4. Target architecture

Stay a **resource**, as `cia.resource` and `misc.resource` are (functions through LVOs). A `.device` would reinvent `OpenDevice` for no gain.

Three objects:
- **the resource** owns the hardware and arbitrates;
- **the client session** (`BDMA_OpenClientTagList`, `BDMA_CloseClient`) carries the default reply port, the default priority and a name for debugging
  (a tool can list who owns what). `BDMA_CloseClient` aborts the jobs still in flight;
- **the job** is an exec message: a `struct Message` first, allocated by the client, with a reply port. The end of a job is a `ReplyMsg()`, as for an
  `IORequest`. `PA_SIGNAL` gives the notification of a task, `PA_SOFTINT` the notification at software interrupt level: nothing has to be invented.

Functions (all by TagList, new ones at the end of the SFD):
- `BDMA_AllocJobTagList` / `BDMA_SetJobTagList` / `BDMA_StartJob` / `BDMA_FreeJob`: "describe" is separated from "submit"; a job is reused; the tags are parsed once and
  the start is a short path (finding 8). `BDMA_AddJobTagList` stays as the shortcut that allocates, starts and frees at the end.
- `BDMA_WaitJob`, `BDMA_CheckJob`, `BDMA_AbortJob` keep the meaning of `WaitIO`, `CheckIO`, `AbortIO`.
- `BDMA_QueryJobTagList`: position, error, bytes done (the AHI driver needs the position of a ring).

Rules of the resource:
- the cache maintenance after the transfer is done by the resource itself, **before** the reply message;
- nothing is allocated at submission: a prepared pool, so that a submission from a software interrupt is possible; the tags are resolved at `BDMA_OpenClientTagList`,
  in the context of a task, so that `utility.library` is opened there and never from an interrupt;
- big jobs are cut into slices (1 MB is about 1 ms, measured) so that the priorities act between two slices;
- the resource claims the channels that the ARM may use at the cold start (priority 117, before the drivers); a driver that insists on its own control
  blocks, as the AHI driver does today, gets a channel on loan. That allows a migration in two steps: the loan first, the jobs afterwards.

Limit to accept: AmigaOS has no hook at the end of a task. A task that dies without `BDMA_CloseClient` leaves its jobs running and the DMA may write into memory that was freed.
Devices have the same risk with an `IORequest`; the contract is documented rather than checked at a high cost.

### Layers

Without a design limit does not mean one huge tag list. The precedent in the family is `mailbox.resource`: convenience functions plus a generic `RawCommand()` that allows everything.
The same shape here:
- **L0, the hardware owner**: arbitration of every channel the ARM may use (legacy, lite and 40 bit classes, each with a queryable set of capabilities), the interrupts, the cache rules,
  the abort and the reset of a channel, the memory for control blocks (coherent, 32 byte aligned, their physical address hidden). On top of it a **raw path**: a channel on loan, or a job
  made of a control block chain that the client built itself (`BDMA_AllocChain`, the chain given to a job); the resource only checks the addresses against the memory map, starts, takes
  the interrupt, completes and cleans up. Nothing the hardware can do (a ring, an interleave, a source that is ignored, a wait count, the panic priority) is out of reach.
  (No memory protection exists on this platform: the check is a sanity check and the arbitration, not a protection.)
- **L1, the jobs by TagList**: what the resource builds itself from tags (copy, rectangle, fill, move, gather, cyclic, peripherals), with the routine hardware rules hidden
  (head / body / tail, row order, cache, slicing of big jobs, the length limits of the lite channels). Tags that name a hardware feature exist next to the abstract ones
  (width, burst, stride, wait cycles, priority) as hints that the resource honours when the chosen channel class can.
- **L2, conveniences** (copy, fill, rectangle as functions): a client library outside the ABI, built on L1.

What belongs to the **shape** and must be decided now: the channel classes and their capability query, the two layers (raw chain and jobs), the peripheral id scheme with the table owned by
the resource (from the device tree `dmas` properties), the ring with its period interrupt and position, the model detection that fills the capability matrix. The individual tags are additive.

Extensions for the clients of 1 (additive, to design when a client needs them): `BDJ_FromPeripheral` / `BDJ_ToPeripheral` with an id `BDPER_*` (the resource knows the FIFO
address and the DREQ of each from the device tree; no raw bus address is ever accepted, which is also what keeps arbitrary clients out of the hardware), `BDJ_SrcFixed` /
`BDJ_DstFixed` (the fill today is a source that does not advance), `BDJ_Width` / `BDJ_Burst` as hints for peripherals only, `BDJ_Segments`, `BDJ_Cyclic` with `BDJ_Period`,
`BDJ_Timeout`, `BDJ_After`, feature flags and `BDI_Peripherals` in the query.

## 5. The name

Proposal: **`brcm-dma.resource`**. The sibling drivers are `brcm-emmc.device` and `brcm-sdhc.device`, and the `OpenResource()` name is ABI. The prefix `BDMA_` stays valid (Broadcom DMA).
Today the cost is small (`BCMDMANAME`, file names, the CMake project, the documentation); later it breaks the clients.

## 6. To measure before deciding

- the 300 us taken apart: tag parsing, cache maintenance, the sleeping wait;
- the latency of a high priority job behind a job of 64 MB, with and without slices;
- the behaviour of `BDMA_AbortJob` on a job that is running (not re-read in this review, not verified complete);
- the SD driver's throughput in PIO today, before anything is built for it (no measure exists).

## 7. Decisions asked of the author (recommendation first)

1. Name: `brcm-dma.resource`.
2. End of a job: a message with a reply port, instead of `Signal(struct Task *)`.
3. The job is allocated by the client and reusable.
4. A client session with a cleanup at close.
5. The resource arbitrates every channel, with a loan for the AHI driver as it is.

Order of the work, once decided: `VideoCore.card` as the first client (it is the reason the resource exists), then the AHI driver (cheapest: it already has the ring and
DREQ 10; it needs the cyclic job, the software interrupt and the position). The SD side drops out of the list for the Pi 4: the EMMC driver uses the ADMA2 of its own controller (8). Only
`brcm-sdhc.device` (SDHOST, Pi 3) would need the SoC DMA, and that needs the BCM2835 family backend first.
The SD, EMMC and AHI drivers are not the author's: any trial is made in his own copies or as a patch, never on the repositories of their authors.

## 8. How the low level modules are configured: dtoverlays, not ToolTypes

The SD and EMMC drivers are configured by the dtoverlays of Emu68, set in the `config.txt` of the boot card (which includes `user.txt`), not by ToolTypes: that is the
convention for the modules that start with the Kickstart, below the level where `icon.library` exists. (`docs`: `Emu68/documentation/overlays.md`, `sdhc.dtbo`, `emmc.dtbo`.)
The mechanism, read in `brcm-emmc.device/src/init.c`: the overlay turns into tokens of `/chosen/bootargs` (`emmc.verbose=`, `emmc.unit0=`, `emmc.low_speed`, `emmc.clock=`),
which `Init` looks up with `FindToken()` on the command line of the device tree.

What follows for this resource:
- **its own switches should be dtoverlay parameters too**, not ToolTypes: a `brcm-dma.dtbo` (or parameters of an existing overlay) read from `/chosen/bootargs`, for example to leave
  channels out of the pool, to disable the resource, to change the verbosity. Nothing of the kind exists yet; `src/init.c` does not read `bootargs`. To design with the author of the overlays;
- **the SD drivers have a documented switch for DMA, and it is NOT this resource's DMA**: `overlays.md` lists `irq` ("Enables using of IRQs by the driver") and `dma` ("Enables using of DMA by the
  driver, auto-enables IRQs too") for `emmc.dtbo` (not for `sdhc.dtbo`). `main` of `brcm-emmc.device` does not parse them, but the branch `DMA_mode` (`d67fa90` "add gic400.library to emmc.device",
  `c052870` "DMA WiP, still not very stable") does: it enables **ADMA2, the DMA engine inside the SDHCI controller** (a table of descriptors in `emmc_ADMA2Table`, written to `EMMC_ADMA_SA`,
  `CachePreDMA` / `CachePostDMA` around the transfer), takes the completion through the GIC (`AddIntServerEx` of the single interrupt of the controller) and reads the device tree property `use-dma`
  (set by the overlay). The controller reads and writes memory by itself: the Pi's DMA channels, DREQ and control blocks are not involved. So on a Pi 4 **the EMMC driver has no natural need of this resource** for its main path (peripheral transfers are not involved). It stays a possible client for the buffers that ADMA2 does not take (`emmc.c`: the driver uses DMA only for a 32 bit aligned buffer in `0x01000000` to `0x80000000`, and falls back to PIO otherwise): a bounce buffer and an asynchronous copy, for big multi block transfers only; the earlier idea of an SD client (peripheral, DREQ, gather, timeout) applies, at most, to `brcm-sdhc.device` (SDHOST has no ADMA)
  on the BCM2835 family, for which this resource has no backend;
- the boot card is the place where a DMA experiment on the SD drivers is switched on or off: `emmc,unit=rw,irq,dma` in `user.txt`, with a second medium to test on.

## 9. Decided on 2026-10-06, and the ABI that follows (draft, to review)

Decided: (1) the name `brcm-dma.resource`, the macro of the header `BRCMDMANAME` (it replaces `BCMDMANAME`); (2) the end of a job is a message with a reply port; (3) the job belongs to the client (`Alloc` / `Free`, reusable); (4) a client session with a cleanup at close;
(5) the resource arbitrates every channel. The prefix `BDMA_` stays. Nothing below is written yet; `include/resources/bcmdma.h` is the author's file and changes only when he asks, line by line.

**Objects**
- `struct BDMAClient *`: opaque; made by `BDMA_OpenClientTagList()`. Tags `BDC_Name` (a string kept for the tools that list the owners), `BDC_ReplyPort` (default reply port of its jobs: a `struct MsgPort *`, `PA_SIGNAL` for a
  task or `PA_SOFTINT` for an interrupt handler), `BDC_Priority` (default `BDPRI_*`). `utility.library` is opened here, in the context of a task, so that nothing else has to be opened later.
- `struct BDMAJob`: public beginning, private end. `struct Message bj_Msg` first (replied to `bj_Msg.mn_ReplyPort` at the end of the job), then `ULONG bj_Error` (`BDERR_*`, valid once replied) and `ULONG bj_Done` (bytes done;
  the position of a ring). Made by `BDMA_AllocJobTagList()` (the resource allocates, in a task context; the **client owns it** until `BDMA_FreeJob()`).

**Functions** (the table is renumbered: no release exists yet; **the vectors of the SFD are in the alphabetical order of the names**, 2026-10-06: AbortJob, AddJobTagList, AllocJobTagList, CheckJob, CloseClient, FreeJob, OpenClientTagList, QueryJobTagList, QueryTagList, SetJobTagList, StartJob, WaitJob; the numbers below are the logical order of this document)
| # | Function | Notes |
|---|---|---|
| 1 | `BDMA_QueryInfoTagList(tags)` | as today |
| 2 | `BDMA_OpenClientTagList(tags)` | task context |
| 3 | `BDMA_CloseClient(client)` | aborts the jobs of the client still in flight, then frees what is its own |
| 4 | `BDMA_AllocJobTagList(client, tags)` | parses and checks the tags once (`BDJ_*`), a task context; `BDERR_*` through `BDJ_ErrorCode` |
| 5 | `BDMA_SetJobTagList(job, tags)` | changes the addresses, the length or the rows of a job that is not in flight, checks again |
| 6 | `BDMA_StartJob(job)` | queues it; **no allocation, no tag parsing**: callable from a software interrupt; the job is replied to its port when over |
| 7 | `BDMA_WaitJob(job)` | `WaitIO` for a job: sleeps until it is replied, takes the message off the port, returns `bj_Error`; no busy loop |
| 8 | `BDMA_CheckJob(job)` | `CheckIO` |
| 9 | `BDMA_AbortJob(job)` | `AbortIO`; the job is replied with `BDERR_ABORTED` |
| 10 | `BDMA_FreeJob(job)` | only for a job that is not in flight |
| 11 | `BDMA_QueryJobTagList(job, tags)` | `BDJI_State`, `BDJI_Error`, `BDJI_BytesDone`, `BDJI_Position` (a ring) |
| 12 | `BDMA_AddJobTagList(client, tags)` | the shortcut: 4 + 6; the job is freed by the `BDMA_WaitJob()` / `BDMA_AbortJob()` that ends it (a flag in the private part); `BDMA_AddJobTags()` and the other `Tags` variants as inline varargs |

**Tags**: those of the job stay (`BDJ_Src`, `BDJ_Dst`, `BDJ_FillValue`, `BDJ_Length`, `BDJ_Rows`, the pitches, `BDJ_Move`, `BDJ_NoCache`, `BDJ_NoWait`, `BDJ_Priority`, `BDJ_ErrorCode`, `BDJ_Strict`); `BDJ_NotifyTask` and
`BDJ_NotifySignals` go (the reply port replaces them); `BDJ_ReplyPort` overrides the port of the client for one job. The info tags `BDI_*` stay, `BDC_*` (client) and `BDJI_*` (job info) are new. **`BDJ_Classes`** (a mask of the channel classes the job may run on: `BDCLASS_DMA40`, `BDCLASS_NORMAL`, `BDCLASS_LITE`) is the only tag about the class: default = every class that can do the job (what the other tags of the job imply: a peripheral, a width, a length above 64 KB...), the fastest free one is taken, the job waits in the queue when none is free. "Fast" is therefore the default and needs nothing; "in the background" is `BDJ_Classes = BDCLASS_LITE | BDCLASS_NORMAL` (the job never takes a fast channel and waits for a slow one). There is no separate preference tag and no "require" tag: the needs of a job are implied by its tags. `BDI_Classes` (the mask of the classes present, and the number of channels of each) tells a client what it can ask. A fallback "slow if possible, else fast" can be added later without breaking the ABI.

**Rules of the resource**
- the cache maintenance after the transfer is done by the resource, before the reply (policy of `src/cache.h`);
- a job in flight is never freed by the resource; a client that dies without `BDMA_CloseClient()` leaves its jobs running (the contract is documented, as for an `IORequest`);
- a big job is run in slices (1 MB is about 1 ms) so that the priorities act between two slices;
- the resource claims, at the cold start (priority 117), the channels of the mask of the firmware that are idle and not DISDEBUG; a channel the firmware uses (observed: channel 6) is left alone; the pool of the memory
  copies stays on the DMA40 channels; the historic classes (normal and lite) take the jobs that name a peripheral or a hardware feature, and the jobs that restrict themselves to them with `BDJ_Classes`; a job on a lite channel is cut into blocks of 65532 bytes at most by the resource.

**Order of the work**, one commit each, each checked with `bcmdma_test`, `bcmdma_stress`, `bcmdma_cost` (the tools follow the new calls):
1. the rename `brcm-dma.resource` (name string, files, CMake project, documents);
2. client, job as a message, functions 2 to 12, the end by reply and the cache before it, no busy loop; the tests move to the new calls;
3. the arbitration of the channels at the cold start;
4. slices of big jobs; then the new tags (widths and bursts of the DMA40, `BDJ_Timeout`, `BDJ_After`), then the historic channels, the ring and the raw chain.

## 10. Example (the ABI of 9; a draft, names and types may still move)

A player copies the planes of every decoded frame into the screen with the DMA while the CPU decodes the next one. The client does not know what a channel is, and cannot tell a fast one from a slow one.

```c
#include <proto/exec.h>
#include <proto/bcmdma.h>          /* generated from the SFD, as the other protos */
#include <resources/bcmdma.h>

struct Library *BDMABase;          /* the base the inline stubs use */

BOOL ShowFrames(UBYTE **frames, ULONG count, UBYTE *screen, ULONG width, ULONG height, ULONG srcPitch, ULONG dstPitch)
{
    struct BDMAClient *client;
    struct BDMAJob *job;
    ULONG error, i;
    BOOL ok = FALSE;

    if ((BDMABase = OpenResource(BRCMDMANAME)) == NULL)
        return FALSE;                                   /* not there: copy with the CPU */

    /* one session: it names the program for the tools and cleans up if the program leaves in a hurry */
    if ((client = BDMA_OpenClientTags(BDC_Name, (ULONG)"MyPlayer", TAG_DONE)) != NULL)
    {
        /* the job is described and checked once; it belongs to the program until BDMA_FreeJob() */
        job = BDMA_AllocJobTags(client,
                                BDJ_Dst, (ULONG)screen,    BDJ_DstPitch, dstPitch,
                                BDJ_Src, (ULONG)frames[0], BDJ_SrcPitch, srcPitch,
                                BDJ_Length, width * 4,     BDJ_Rows, height,
                                BDJ_ErrorCode, (ULONG)&error, TAG_DONE);
        if (job != NULL)
        {
            ok = TRUE;
            for (i = 0; i < count && ok; i++)
            {
                BDMA_SetJobTags(job, BDJ_Src, (ULONG)frames[i], TAG_DONE);   /* only what changes */
                BDMA_StartJob(job);                                          /* returns at once */

                DecodeNextFrame();                                           /* the CPU works meanwhile */

                ok = BDMA_WaitJob(job) == BDERR_OK;                          /* sleeps until the job is over: no busy loop,
                                                                                the cache is already right for the CPU */
            }
            BDMA_FreeJob(job);
        }
        BDMA_CloseClient(client);
    }

    return ok;
}
```

What the resource did without being asked: it picked the fastest free channel, cut the rectangle into control blocks (head, body and tail around the 128 bit accesses), kept the order against the other clients' jobs
that touch the same memory, pushed and dropped the cache lines, took the interrupt and replied to the program's port.

**The same, from a driver, at interrupt level.** The reply port wakes a software interrupt instead of a task, and the next block is started from there (no allocation, no parsing: that was done by `BDMA_AllocJobTags()`).

```c
port->mp_Flags = PA_SOFTINT;                 /* a MsgPort made by CreateMsgPort(), then pointed at the interrupt */
port->mp_SigTask = (APTR)&myInterrupt;       /* ReplyMsg() will Cause() it */
client = BDMA_OpenClientTags(BDC_Name, (ULONG)"mydriver.device", BDC_ReplyPort, (ULONG)port, TAG_DONE);
/* ... job = BDMA_AllocJobTags(...); BDMA_StartJob(job); ... */

/* in the software interrupt */
job = (struct BDMAJob *)GetMsg(port);        /* the job that is over: job->bj_Error says how */
if (job->bj_Error == BDERR_OK && more) { BDMA_SetJobTags(job, BDJ_Src, next, TAG_DONE); BDMA_StartJob(job); }
```

**In the background**, so that a big copy never takes a fast channel that a latency sensitive client may need: one more tag, `BDJ_Classes, BDCLASS_LITE | BDCLASS_NORMAL`; the job then waits for a slow channel.
