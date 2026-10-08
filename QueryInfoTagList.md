# BDMA_QueryInfoTagList — information and statistics

`BDMA_QueryInfoTagList()` (and its inline variant `BDMA_QueryInfoTags()`) tells a client what the engine is and what it has done.
The reference for the call is `Autodocs/brcm-dma.doc`; this document describes the measures and how to read them.

## How to query

Each tag names an attribute; its value is a `ULONG *` that receives the answer, as `GetDTAttrs()` does. The result is the number of values written.
A tag this version does not know is skipped and its place left untouched, so a client may ask for newer attributes than the resource has.
Task context only (the first call starts the engine and opens utility.library).

```c
ULONG jobs = 0, queuemax = 0, waitmax = 0;

BDMA_QueryInfoTags(
    BDI_Jobs,      (ULONG)&jobs,
    BDI_QueueMax,  (ULONG)&queuemax,
    BDI_WaitMaxUs, (ULONG)&waitmax,
    TAG_DONE);
```

## The tool `brcm-dma-info`

Reads everything and writes nothing; safe to run at any time, also while other clients work.

```
brcm-dma-info                       one report
brcm-dma-info WATCH 5               the report, then a line every 5 seconds
brcm-dma-info WATCH 5 COUNT 12      the same, 12 intervals, then it stops
brcm-dma-info WATCH 5 LOG           a new pair of lines per interval, no escape codes
brcm-dma-info RESET                 the report, then the counters back to zero
```

In a watch the two lines of an interval are drawn over the previous ones (console sequences: two lines up, erase to the end of the display), so the output stays short and shows the latest interval; `LOG` keeps every interval as a new pair of lines, without escape codes, for a redirection to a file or a capture through a remote tool.
Ctrl-C stops a watch. Exit code 0, 5 when `WATCH` is asked and no engine runs, 10 when the resource is missing.

**The report** (one run, counters since the start of the OS):

```
brcm-dma.resource 0.1: available, BCM2711, interrupt yes
channels: 12 13; classes: dma40; features: rect move wide fill; max rows 65535
RTG memory: 3C800000, 24576 KB
jobs 27397 (3605 MB), failed 0, aborted 2, timeouts 0, busy 3, slices 40, deferred 5
queue max 4, longest wait 312 us; busy: channel 12 41200 ms, channel 13 3900 ms
sizes: <=64B 120 <=4K 9000 <=32K 17000 <=1M 1200 >1M 77
interrupts: 16099, 596 Mcycles, 24625 kinstructions (37888 cycles, 1566 instructions each)
```

(The figures above only show the layout.) The two busy times are those of the first two channels managed, in ascending order of channel number. The last line is what the interrupts of the channels
cost the CPU (the counters of Emu68, `BDI_IrqCalls`, `BDI_IrqMCycles`, `BDI_IrqKInstr`) with the mean of one interrupt.

**The watch lines** give the activity of each interval, not the totals:

```
5s 812 jobs/s 94.3 MB/s ch12 37% ch13 5% wait 312 us q 4 +2 busy
   sizes: <=64B 100 <=4K 2900 <=32K 1000 <=1M 60 >1M 0
```

- `jobs/s`, `MB/s`: jobs done and megabytes moved in the interval, divided by its real duration (measured with `DateStamp`, resolution 20 ms; use 1 s or more).
- `chN %`: load, the share of the interval during which that channel ran slices (`delta(BusyMs) / interval`). Two channels can add up to 200%.
- `wait`, `q`: the longest wait and the largest queue, since the start of the OS, not of the interval (a maximum cannot be subtracted).
- `+N fail / abort / tmo / busy / defer`: shown only when the counter moved during the interval.
- `sizes`: the histogram of the interval.

A useful use is to start `brcm-dma-info WATCH 2` in a shell and work on the desktop (scrolling, moving windows, a benchmark) to see what reaches the DMA, and in which sizes.

## The engine

| Tag | Meaning |
|---|---|
| `BDI_Available` | TRUE when an engine runs and proved itself |
| `BDI_Version` | `(version << 16) | revision` of the resource |
| `BDI_Interrupt` | TRUE when the end of a job comes by interrupt |
| `BDI_Model` | `BDM_*`: the SoC family (from the board revision) |
| `BDI_Features` | `BDFF_*` bits: rectangle, move, wide, fill |
| `BDI_Channels` | mask of the DMA channels managed |
| `BDI_Classes` | `BDCLASS_*` of the channels managed |
| `BDI_MaxRows` | a limit, not a measure: the most rows a rectangle job may have (65535) |
| `BDI_VideoBase`, `BDI_VideoSize` | the memory of the RTG board (not cached by the CPU; 0 if none) |

## The statistics

All counters count from the start of the OS, or from the last `BDMA_ResetStatistics()` (`brcm-dma-info RESET` calls it after printing the report): read twice and subtract to measure an interval, or reset first.
They are shared by every client: whoever resets them resets them for all, there is no check.
They are plain counters or maxima updated where the event happens (interrupt, or interrupts off), so reading them costs nothing and disturbs nothing.

| Tag | Meaning | What it tells |
|---|---|---|
| `BDI_Jobs` | jobs done | the volume of work |
| `BDI_MegaBytes` | megabytes moved (length × rows) | throughput over an interval |
| `BDI_Failures` | jobs that failed (hardware error, timeout) | should stay 0 |
| `BDI_Aborts` | jobs aborted | clients that cancel (scrolling interrupted, program closed) |
| `BDI_Timeouts` | slices given up by the watchdog | a stuck channel; also included in `BDI_Failures` |
| `BDI_Busy` | `BDJ_NoWait` jobs refused for lack of an idle channel | how often a client had to fall back to the CPU |
| `BDI_QueueMax` | the most jobs ever waiting in the queue at once | whether two channels are enough |
| `BDI_WaitMaxUs` | the longest stay in the queue before a slice went on a channel, in microseconds | the worst latency a client suffered |
| `BDI_Slices` | times a big job went back to the queue for its next slice | whether slicing is used (big jobs exist) |
| `BDI_Deferred` | jobs held back by a conflict of footprints while a channel was idle | what the ordering rule costs |
| `BDI_BusyMs0`, `BDI_BusyMs1` | milliseconds the first and second channel (in the order of `BDI_Channels`) ran slices | utilisation and balance of the channels |
| `BDI_IrqCalls` | interrupts of the channels that ended a slice | how many times the service woke the CPU |
| `BDI_IrqMCycles`, `BDI_IrqKInstr` | CPU cycles (units of 2^20) and 68k instructions (units of 2^10) spent in those interrupts, from the counters of Emu68 (`MOVEC`, read in the interrupt: it runs in supervisor mode) | what the DMA service costs the CPU: the building of the next slice, the reply, the cache of the small jobs; `brcm-dma-info` prints the average of one interrupt |
| `BDI_Size64`, `BDI_Size4K`, `BDI_Size32K`, `BDI_Size1M`, `BDI_SizeBig` | jobs done of up to 64 bytes, 4 KB, 32 KB, 1 MB, and more (bytes = length × rows) | the real distribution of job sizes |

Definitions that matter when reading them:
- **Waiting** means in the queue, not on a channel: it counts a job just submitted and a big job between two of its slices.
- **Wait** is measured at each stay in the queue (submission, or return after a slice), so a sliced job has several waits; the maximum is kept.
- **Deferred** is counted once per stay in the queue, and only when a channel was idle (a job waiting for a busy channel is not deferred).
- **Busy time** is the time slices were on a channel; utilisation of a channel over an interval is `delta(BusyMs) / elapsed ms`, with the interval timed by the caller.

## Using them

- Choosing a threshold below which the CPU does the copy (for example `VC6_DMA_THRESHOLD` of VideoCore.card): look at the size histogram of a real session.
- Latency problems: `BDI_WaitMaxUs` high with `BDI_QueueMax` small means a long job blocks a channel (check `BDI_Slices`); a large `BDI_QueueMax` means the two channels are saturated.
- Ordering cost: `BDI_Deferred` close to `BDI_Jobs` means clients submit overlapping jobs that serialise.
- Health: `BDI_Failures`, `BDI_Timeouts` should be 0 in normal use.
- What the service costs the CPU: `BDI_IrqMCycles * 2^20 / BDI_IrqCalls` cycles for one interrupt (about 38000, 21 us at 1.8 GHz, for a small job; about 55000 to 65000 for a slice of a big job, which builds the next chain in the interrupt).
- Unbalanced `BDI_BusyMs0` / `BDI_BusyMs1` means one channel does most of the work (the first idle channel is always taken first, so a small imbalance is normal).

## Notes

- `BDMA_ResetStatistics()` sets to zero every measure (not the state of the queue and of the channels); `BDI_QueueMax` starts again at what is waiting at that moment.
- The counters are 32 bits: `BDI_Jobs` wraps after about 4 billion jobs, `BDI_BusyMs0/1` after about 49 days of busy time.
- When the engine is not available, `BDI_Available` is FALSE and the statistics are zero; the reason is kept in the base for the test tools.
- Statistics are part of the information interface: a new one is a new `BDI_*` tag and an entry in `Autodocs/brcm-dma.doc`.
