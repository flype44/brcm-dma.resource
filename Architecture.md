# brcm-dma.resource — Architecture specification

`brcm-dma.resource` is the shared owner of the DMA engine of the Raspberry Pi SoC under AmigaOS / Emu68. Clients describe jobs; the resource decides when and on which channel they run,
hides the hardware rules, and reports completion with normal Exec messages.

Status legend: **[done]** implemented and tested on a Pi 4B; **[planned]** designed, not written.
Functions: `Autodocs/brcm-dma.doc`. Hardware facts and measures: `Capabilities.md`. Vocabulary: `Glossary.md`.

## 1. Requirements

| # | Requirement |
|---|---|
| R1 | Cost of a small job of the order of a microsecond: nothing allocated or parsed on the start path; control blocks come from a prepared pool. |
| R2 | The CPU is left to Exec: completion by interrupt, the caller sleeps, no polling. |
| R3 | Clients are hardware independent: no channel number, register, interrupt controller or SoC model in the client code. |
| R4 | AmigaOS idiom: resource, TagLists, messages and reply ports, `WaitIO`/`CheckIO`/`AbortIO` semantics. |
| R5 | One owner arbitrates every channel the ARM may use; clients never claim a channel. |
| R6 | The resource performs the cache maintenance; the data is valid when the reply arrives. |
| R7 | Portable C (GCC 14+, other architectures): hardware access only inside the backends. |
| R8 | Additive evolution: new features are tags or functions at the END of the table; the client/job model does not change. |

## 2. Structure

```text
Clients
   |
Job API (TagLists, 12 functions)                [done]
   |
Common engine: jobs, queue, scheduler, conflict detection,
               channel arbitration, slicing, watchdog, completion    [done, inside engine.c]
   |
Platform backend (one per SoC family, chosen from the device tree)
   +-- DMA backend:  registers, control blocks, widths, bursts, channel classes, reset
   +-- IRQ backend:  delivery of "completed / failed" to the engine
   |
   +-- BCM2711 (Pi 4B, CM4, Pi 400): 40 bit channels 12, 13; IRQ through gic400.library    [done]
   +-- BCM2835 family (Zero, 2, 3): normal/lite channels; legacy interrupt controller      [planned]
```

Rules: the common engine knows jobs, never registers. The DMA backend knows the DMA hardware. The IRQ backend knows how an interrupt is delivered: gic400.library is a detail of the BCM2711 IRQ
backend, not of the generic design. Clients know none of it. BCM2712 (Pi 5) is out of scope.

Layers offered to clients:

| Layer | Content | Status |
|---|---|---|
| L0 raw | a channel on loan, or a job made of a control block chain built by the client; the resource checks addresses, starts, takes the interrupt, completes, cleans up | [planned] |
| L1 jobs | what the resource builds from tags: copy, rectangle, fill, move; hardware rules hidden | [done] |
| L2 conveniences | `Copy()`, `Fill()`... as functions of a client library, outside the ABI | not part of the resource |

Stay a **resource** (functions through LVOs), as `cia.resource` and `misc.resource`: a `.device` would add an I/O request layer without solving channel ownership.

## 3. Objects

- **Resource**: `OpenResource("brcm-dma.resource")` (`BRCMDMANAME`), RomTag priority 117, RTF_COLDSTART. Needs devicetree.resource, mailbox.resource, gic400.library >= 1.3.
  Init only checks and registers; the engine starts at the first query or job, from a task (a failure leaves it unavailable, never makes the machine unbootable).
- **Client** (`struct BDMAClient`, opaque): a session made by `BDMA_OpenClientTagList()`; carries a name (for tools), a default reply port, a default priority. `BDMA_CloseClient()` aborts its jobs in flight.
- **Job** (`struct BDMAJob`): an Exec message (`bj_Msg` first, then `bj_Error`, `bj_Done`); allocated by the resource, owned by the client until `BDMA_FreeJob()`; described and checked once, restarted as often as wanted.
  Its end is a `ReplyMsg()` to its reply port: a signal port wakes a task, a software interrupt port wakes a driver.

## 4. Functions (the order of the SFD; new ones only at the end)

| Function | Role |
|---|---|
| `BDMA_AbortJob` | `AbortIO`: stops a queued or running job, replied with `BDERR_ABORTED` |
| `BDMA_AddJobTagList` | shortcut: Alloc + Start, freed by the Wait/Abort that ends it |
| `BDMA_AllocJobTagList` | parses and checks the tags once (task context) |
| `BDMA_CheckJob` | `CheckIO` |
| `BDMA_CloseClient` | aborts what is in flight, frees the jobs and its own port |
| `BDMA_FreeJob` | gives back a job that is not in flight |
| `BDMA_OpenClientTagList` | opens a session (task context) |
| `BDMA_QueryInfoTagList` | attributes of the engine (`BDI_*`): availability, version, model, features, channels, classes, counters |
| `BDMA_QueryJobTagList` | state, error, bytes done of a job (`BDJI_*`) |
| `BDMA_SetJobTagList` | changes a job that is not in flight; a refused change leaves it as it was |
| `BDMA_StartJob` | queues the job: no allocation, no parsing, callable from a software interrupt |
| `BDMA_WaitJob` | `WaitIO`: sleeps until replied, any task, idempotent |

Each `...TagList` has an inline `...Tags` varargs variant generated by sfdc. Tags: job `BDJ_*` (`Src`, `Dst`, `FillValue`, `Length`, `Rows`, `SrcPitch`, `DstPitch`, `Move`, `NoCache`, `NoWait`,
`Priority`, `ReplyPort`, `Timeout`, `Classes`, `ErrorCode`, `Strict`), client `BDC_*`, query `BDI_*`/`BDJI_*`. Errors `BDERR_*`. Unknown tags are ignored unless `BDJ_Strict`.

## 5. Behaviour

**Jobs.** A job is a 1D copy, a rectangle (rows, pitches, up to 65535 rows), a fill, or a move by whole rows (overlap inside a row is refused with `BDERR_OVERLAP`).

**Scheduling.** One queue in submission order. A job starts when a channel is idle and it conflicts with no running job and no older waiting job (footprints: a job never overtakes an older one it
writes over or reads from; the result is that of running them one by one). Best priority first among the eligible. The client never chooses a channel. `BDJ_Classes` restricts the channel classes
(`BDCLASS_DMA40/NORMAL/LITE`); the default is every class able to do the job. [done for DMA40; NORMAL/LITE planned]

**Slicing.** A job is a series of units (a piece of a row, 1 MB at most), run in slices of about 1 MB (about 1 ms). Between two slices the job re-enters the queue at its submission rank, so an urgent
job takes the channel and the footprint rule still holds.

**Completion.** The channel interrupt acknowledges, ends or requeues the job, and starts the next ones without a semaphore. A small job (up to 32 KB written) does its cache work and replies in the
interrupt; a bigger one through a software interrupt (the system cache call is not for interrupt level).

**Cache (R6).** `CachePreDMA`-equivalent before the transfer, invalidate after it, before the reply; by cache line below 32 KB (a 64 byte job costs ~49 us instead of ~280), system calls above.
No maintenance for the RTG memory or with `BDJ_NoCache`.

**Errors and recovery.** A hardware error ends the job with `BDERR_HW`. A watchdog (vertical blank server, 20 ms resolution) gives up a slice older than `BDJ_Timeout` (default 1 s, 0 = none):
channel stopped and reset, job replied with `BDERR_TIMEOUT`, queue goes on. The start sweeps channels and GIC ids a previous OS session left behind (ColdReboot does not reset them).
A client that dies without `BDMA_CloseClient()` leaves its jobs running (same contract as an `IORequest`).

**Contexts.** Alloc, Set, Open, Query: tasks only (utility.library is opened at first use, it starts after this resource). Start, Check, Abort: also software interrupts. `BDMA_WaitJob`: tasks.

## 6. BCM2711 backend [done]

Channels 12 and 13 (device tree mask AND firmware `GET_DMA_CHANNELS`, never DISDEBUG), interrupts GIC id 110 + channel. A chain is up to three control blocks per row: 32 bit head and tail to bring the
destination to 16 bytes, a 128 bit body with bursts of 16. Fill uses a 256 byte constant. Control blocks live in coherent GPU memory (mailbox), one fixed area per channel. Self test (4 KB copy,
interrupt must arrive) before a channel is used. Details and limits: `Capabilities.md`.

## 7. Planned

| Item | Note |
|---|---|
| Split of `engine.c` into common engine and BCM2711 backend (DMA + IRQ parts) | prerequisite of any second backend |
| Normal and lite channel classes (BCM2711 channels 0-10) | peripherals, 64 KB limit on lite handled by the resource |
| Peripheral DMA (`BDJ_FromPeripheral`/`ToPeripheral`, ids owned by the resource from the device tree; no raw bus address accepted) | AHI, SD |
| Cyclic/ring job with period interrupt and position | AHI |
| Segments (scatter/gather), dependency (`BDJ_After`), width/burst hints | additive tags |
| Raw chain layer (L0) | `BDMA_AllocChain` or equivalent, at the end of the table |
| BCM2835 family backend | needs a Pi 3 to measure; legacy interrupt controller |
| Switches by dtoverlay (`/chosen/bootargs`) | channels left out, disable, verbosity |

## 8. Example

```c
struct Library *BDMABase = OpenResource(BRCMDMANAME);   /* NULL: copy with the CPU */
struct BDMAClient *client = BDMA_OpenClientTags(
    BDC_Name, (ULONG)"MyPlayer",
    TAG_DONE);
struct BDMAJob *job = BDMA_AllocJobTags(client,
    BDJ_Dst, (ULONG)screen,
    BDJ_DstPitch, dstPitch,
    BDJ_Src, (ULONG)frame,
    BDJ_SrcPitch, srcPitch,
    BDJ_Length, width * 4,
    BDJ_Rows, height,
    TAG_DONE);

BDMA_StartJob(job);          /* returns at once */
DoOtherWork();
BDMA_WaitJob(job);           /* sleeps; the cache is already right */

BDMA_FreeJob(job);
BDMA_CloseClient(client);
```

From a driver at interrupt level: give the client a `PA_SOFTINT` reply port; in the software interrupt `GetMsg(port)` returns the finished job, then `BDMA_SetJobTags()` + `BDMA_StartJob()`.
