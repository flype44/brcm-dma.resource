# brcm-dma.resource

PiStorm / Emu68 AmigaOS resource to manage Raspberry Pi DMA jobs.

This module is intended to be embedded in the **Emu68 ROM**.

## What it is

An AmigaOS resource for PiStorm / Emu68 that owns the DMA engine of the Broadcom SoC of the Raspberry Pi. It offers it to any program as a safe, asynchronous memory transfer, and it hides the model of the Pi from its clients.

- **Jobs, described by TagList:** a copy, a rectangle with pitches, a fill, a move by whole rows. A job is described once and started as often as wanted.
- **Scheduling:** jobs are queued by priority and order of submission onto a pool of channels (the client never picks one). A job never overtakes an older one it conflicts with, and big jobs run in slices so that an urgent job does not wait for them.
- **Completion:** by interrupt, and by a reply to a message port (a task asleep, or a software interrupt for a driver). A timeout watchdog and `BDMA_AbortJob()` are there for the rest.
- **The hardware rules are the resource's business:** physical addresses, control blocks, alignment, cache maintenance, and a clean hardware state at every start of the OS.

## What it aims at maturity

A general purpose DMA service for the Amiga side of the Pi, not tied to graphics or to one client:

- every channel class the hardware offers (40 bit, normal, lite) arbitrated by the resource, and one backend per SoC family;
- the same TagList interface growing to scatter-gather, cyclic and dependent jobs, and peripheral transfers (DREQ), over a raw chain interface and under a client library of conveniences;
- drivers (VideoCore.card first) using it instead of carrying their own DMA code, so that the 68k stays free; the DMA is not meant to beat the CPU on raw speed;
- a stable, versioned ABI, ready for Michal to adopt in the Emu68 ROM as it is.

See [Architecture](Architecture.md) (design and what is still to do) and [Capabilities](Capabilities.md) (hardware benchmarks).

## Status

Version **0.1**, tested and running on a Raspberry Pi 4B (BCM2711) in the ROM of Emu68, on the two 40 bit channels 12 and 13. The specifications and development are still Work In Progress.

1.0 GB/s for a copy, 1.25 GB/s for a fill, a job described once and started again in 49 us, an urgent 4 KB job behind two big ones in 1.8 ms, no failure in the test tools (13536 path cases, the queue, big jobs up to 64 MB, a soak).

The BCM2835 family (Zero, Pi 2, Pi 3) and the historic channels are described and measured, **not yet written**.

## Function set

- See [Autodocs](Autodocs/brcm-dma.doc)
- See [Implementation](Implementation.md)

## A quick usage example

```c
#include <proto/exec.h>
#include <proto/brcm-dma.h>
#include <resources/brcm-dma.h>

struct Library *BDMABase;

if ((BDMABase = OpenResource(BRCMDMANAME)) != NULL)
{
	struct BDMAClient *client;
	struct BDMAJob *job;
	ULONG error;
	
	/* Create a DMA client */
	if ((client = BDMA_OpenClientTags(
		BDC_Name, (ULONG)"MyProgram", 
		TAG_DONE)) != NULL)
	{
		/* Allocate a DMA job */
		job = BDMA_AllocJobTags(client, 
			BDJ_Src,      frame, 
			BDJ_Dst,      screen, 
			BDJ_Length,   width * 4, 
			BDJ_Rows,     height, 
			BDJ_SrcPitch, srcPitch, 
			BDJ_DstPitch, dstPitch, 
			TAG_DONE);
		
		if (job != NULL)
		{
			BDMA_StartJob(job);         /* returns at once */
			DoSomething();              /* the 68k works meanwhile */
			error = BDMA_WaitJob(job);  /* asleep until the job is over */
			BDMA_FreeJob(job);
		}
		
		BDMA_CloseClient(client);
	}
}
```

## Licence

MPL-2.0, as Emu68, see [LICENSE](LICENSE).

It is built on the model of [mailbox.resource](https://github.com/michalsc/mailbox.resource).

Same layout, same build, same documentation and ABI versioning.
