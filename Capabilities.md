# DMA hardware of the Raspberry Pi 4B, as seen from AmigaOS / Emu68

What the DMA channels of the BCM2711 are, what they were measured to do, and what is not known. It is the base of the tag list (see `docs/architecture.md`, section 0).
Every line carries its status: **measured** (a run on the machine, with the tool and the date), **read** (in a source, not run), **memory** (written from memory, to verify).

Machine of the measures: Raspberry Pi 4B, ARM at 1.8 GHz (`vcmailbox 0x30002`: `0x6B49D200`), SoC 69.6 to 70.6 C, Emu68 with `bcmdma.resource` 0.1 in its ROM (engine not started),
`VC6_DMA_ENABLE=No`, no AHI driver installed. Tool: `bcmdma_lab.v1` (`test/lab.c`), 2026-10-05, reports in `build/bcmdma_lab.v1_report_*.txt` (not versioned). It drives the registers
itself, 32 bit and 128 bit accesses only, one copy of 4 KB and one of 256 KB (64 KB on the lite channels) per variant, compared with a model, polled (no interrupt).

## 1. The channels

`DMA_ENABLE` = `0x7FFF` (channels 0 to 14 on), mask of the firmware for the ARM (`GET_DMA_CHANNELS`) = `0x37F5`. Registers at `0xf2007000 + 0x100 * n` (little endian). All **measured** (list, read only).

| Channel | Class (DEBUG register) | In the ARM mask | State seen | Use |
|---|---|---|---|---|
| 0, 2, 4, 5 | normal (bit LITE clear) | yes | idle | free: tested |
| 1, 3 | normal | no | idle, DISDEBUG | the firmware's |
| 6 | normal | yes | **active**, DISDEBUG, CB 0 | busy with something of the firmware (not AHI: not installed); skipped, not understood |
| 7, 8, 9, 10 | lite (bit LITE set) | yes | idle | free: tested |
| 11 | dma40 | no | active, DISDEBUG | the firmware's |
| 12, 13 | dma40 | yes | idle | the pool of `bcmdma.resource` |
| 14 | dma40 | no | idle, DISDEBUG | the firmware's |

DEBUG of the legacy channels reads `0x04018x00` (normal) and `0x10018x00` (lite), the channel number in bits 11:8. Channel 15 (PCIe, memory) was not looked at.

## 2. Copy speed, memory to memory (measured, 256 KB unless said)

**DMA40 (12, 13)**, same result on both:

| Accesses | Burst (beats) | MB/s |
|---|---|---|
| 32 bit | 1 | 27 |
| 32 bit | 8 | 96 |
| 128 bit | 1 | 108 |
| 128 bit | 8 | 824 |
| 128 bit | 16 | **1175** (4 KB: 819, the start of a job costs about 5 us) |

**Legacy normal (0, 2, 4, 5)**, 32 bit, bus alias `0xC0000000`; the four channels agree within 2 %:

| Burst field (TI 15:12) | MB/s |
|---|---|
| 0 | 27 |
| 3 | 62 |
| 7 | **78 to 80** |
| 15 | 27 |

Wait for the write responses (TI bit 3) changes nothing (27 and 78 MB/s). **This corrects the laboratory notes, which said that these channels do 35 ns a byte whatever is tried**: with the burst field at 7 they
do 12.7 ns a byte, about three times better (the burst field was not varied in that series, as far as the notes say).

**Legacy lite (7 to 10)**, 32 bit, 65532 bytes (the length is 16 bits): burst field 3 gives **54 MB/s**; 0, 7 and 15 give 25 MB/s; the four channels agree.

A fixed cost per job is visible in the 4 KB lines (DMA40, 128 bit, burst 16: 5 us): the 300 us of a job through `bcmdma.resource` is therefore not the hardware.

## 3. Facts from the earlier laboratory (CLAUDE.md 4, `docs/lab-notes-VideoCore.card.md`; measured then, at 2.2 GHz and 1.8 GHz)

- DMA40: control block of 8 longwords (`ti, src, srci, dst, dsti, len, next >> 5, reserved`); `srci` / `dsti` = upper address bits | burst (11:8) | INC (12) | size (14:13: 0 = 32, 1 = 64, 2 = 128 bit, 3 = 256 bit);
  `CB` register = physical address >> 5; the addresses are physical (a `0xC0000000` alias gave an empty control block); CS: ACTIVE 0, END 1, INT 2, PROT 9:8 in every write, ERROR **10**, TRANSACTIONS 25, WAIT_FOR_WRITES 28, DISDEBUG 29, ABORT 30.
- A burst of 16 beats of 128 bits reads 256 consecutive bytes even with a source that does not increment: a fill needs a constant of 256 bytes.
- A 128 bit write to a destination that is not 16 byte aligned froze the machine once (at 2.2 GHz): head and tail of 32 bit accesses around a 128 bit body. 256 bit accesses timed out (the channel was aborted cleanly).
- Overlap by whole rows is correct in both directions; a shift left by 8 bytes inside a row froze the machine twice at 2.2 GHz and passed 6 times at 1.8 GHz.
- The hardware 2D mode (TDMODE) of the legacy channels froze the machine twice (2.2 GHz); not tried since.
- Two DMA40 channels on halves of one job: 1.35 to 1.6 times faster, total flattening near 1.7 GB/s (the memory, not the channel, is the ceiling).
- Interrupt: GIC id = 110 + channel for the DMA40 (122, 123), level high, through `gic400.library`; the interrupt adds about 13 us to a job. A handler that does not acknowledge is a storm.
- The firmware leaves END set after every block of a chain; `CS.END` + `CB == 0` is the end of a chain.

## 4. Not known, not tried (to measure, one at a time)

- 128 bit accesses on the legacy channels (`WIDE` in the tool): froze the machine once at 2.2 GHz, never replayed at 1.8 GHz.
- Other bus aliases for the legacy channels (`ALIAS 0`, `4`, `8`): the tool has the option, no run yet.
- DREQ pacing and the peripheral numbers (memory): not in the sources read so far; to take from the device tree `dmas` properties and the manual. Not run.
- Cyclic transfers, interleave, source or destination ignored, wait cycles, priorities (the bits of the legacy TI other than width, burst and wait for response): not exercised.
- Channel 6 (active with DISDEBUG although in the ARM mask), channel 15, the interrupts of the legacy channels (lines in the device tree to read).
- Two channels of different classes at the same time, and the CPU's cost while a channel runs (measured only for DMA40, see CLAUDE.md 4).
- The behaviour at other clocks and with another memory load: all of the above at 1.8 GHz and about 70 C only.

## 5. Cache maintenance around a transfer (measured 2026-10-05, `bcmdma_cost`, same machine)

Emu68 emulates a 68040 with FPU and no MMU; the instructions `CPUSHL`, `CINVL` (privileged in the manual, `docs/MC68040UM.pdf`) run in user mode on Emu68, as `68040.library` and `brcm-emmc.device` rely on.
The line is 64 bytes.

| Range | one call of `CacheClearE`, `CachePreDMA` or `CachePostDMA` | `CPUSHL` on every line | `CINVL` on every line |
|---|---|---|---|
| 64 bytes | 41 us | 0.4 us | 0.2 us |
| 1 KB | 40 us | 1.1 us | 1.2 us |
| 4 KB | 42 us | 4.3 us | 5.7 us |
| 64 KB | 41 us | 68 us | 74 us |
| 256 KB | 41 us | 270 us | 295 us |

The call of the system costs a fixed 41 us whatever the range; the line by line loop costs about 1 us a KB: they cross near 40 KB. Correctness of the instructions alone: 300 jobs with `BDJ_NoCache`, 4 to 64 KB, offsets
at random, source and destination written by the CPU just before (dirty lines), guard words in the same lines around the destination: 0 wrong, 0 guard word touched. Other building blocks (us): `AllocSignal` + `FreeSignal` 1,
`Disable` + `Enable` 6, `Signal()` to self 6, `AllocVec` + `FreeVec` 7.
