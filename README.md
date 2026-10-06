# brcm-dma.resource

PiStorm/Emu68 AmigaOS resource to manage DMA jobs.

This module is intented to be embedded in the **Emu68 ROM**.

## Introduction

AmigaOS resource for PiStorm / Emu68 that owns the DMA engine of the Broadcom SoC of the Raspberry Pi and offers it to any program as a native, asynchronous memory copy: jobs (a copy, a rectangle with pitches, a fill, a move by whole rows) described once and started as often as wanted, run in slices so that an urgent job does not wait for a big one, completion by interrupt and by a reply to a message port (a task asleep, or a software interrupt for a driver), the cache looked after, a clean hardware state at every start of the OS.

It hides the model of the Pi from its clients.

## Status

Current status is version **0.1**.

It is running on a Raspberry Pi 4B (BCM2711) in the ROM of Emu68.

1.0 GB/s for a copy, 1.25 GB/s for a fill, a job described once and started again in 49 us, an urgent 4 KB job behind two big ones in 1.8 ms, no failure in the test tools (13536 path cases, the queue, big jobs up to 64 MB, a soak).

The BCM2835 family (Zero, Pi 2, Pi 3) and the historic channels are described and measured, **not yet written**.

## Function set

- See [Autodocs](Autodocs/brcm-dma.doc)
- See [Implementation.md](Implementation.md)

## Architecture and what is still to do:

- See [Architecture](Architecture.md) (specifications)
- See [Capabilities](Capabilities.md) (hardware benchmarks)

## A quick usage example

```c
client = BDMA_OpenClientTags(BDC_Name, "MyPlayer", TAG_DONE);

job = BDMA_AllocJobTags(client, 
	BDJ_Src,      frame, 
	BDJ_Dst,      screen, 
	BDJ_Length,   width * 4, 
	BDJ_Rows,     height, 
	BDJ_SrcPitch, srcPitch, 
	BDJ_DstPitch, dstPitch, 
	TAG_DONE);

BDMA_StartJob(job);         /* returns at once */
DoSomething();              /* the 68k works meanwhile */
error = BDMA_WaitJob(job);  /* asleep until the job is over; the cache is already right */

BDMA_FreeJob(job);
BDMA_CloseClient(client);
```

## Licence

MPL-2.0, as Emu68, see [LICENSE](LICENSE).

It is built on the model of [mailbox.resource](https://github.com/michalsc/mailbox.resource).

Same layout, same build, same documentation and ABI versioning.
