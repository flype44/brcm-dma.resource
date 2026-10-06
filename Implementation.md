# brcm-dma.resource — functions

Everything goes through TagLists.

A **client** (`BDMA_OpenClientTagList`) keeps the default reply port and priority of its jobs;

A **job** (`BDMA_AllocJobTagList`) is described and checked once, belongs to the client, and is started again as often as wanted (`BDMA_StartJob`).

A job is an exec message: its end is a `ReplyMsg()` to its reply port, a signal port for a task or a software interrupt port for a driver.

`BDMA_WaitJob`, `BDMA_CheckJob` and `BDMA_AbortJob` keep the meaning of `WaitIO`, `CheckIO` and `AbortIO`.

The resource does not know what a transfer is for:
a copy, a rectangle (rows and pitches), a fill or a move by whole rows, with a priority, a reply port and a few switches. The graphics driver, `CopyMem`, an audio ring, ... build their own functions on top of it.

The functions: `Autodocs/brcm-dma.doc`.

The Architecture and the reasons: `Architecture.md`;

"Tested" is a run of the tools of `test/` on a Raspberry Pi 4B (BCM2711) at 1.8 GHz with the resource in the ROM of Emu68: `brcm-dma-test` (steps 1 to 9), `brcm-dma-stress` (every path, the queue, big jobs, a soak) and `brcm-dma-cost`.

| Function (the vectors of the table follow this order) | Implemented | Since | Tested | What it does |
|---|---|---|---|---|
| BDMA_AbortJob | Yes | V0.1 | Yes | stops a job in the queue or on its channel and replies it with BDERR_ABORTED |
| BDMA_AddJobTagList | Yes | V0.1 | Yes | the shortcut: Alloc and Start, freed by the call that ends the job |
| BDMA_AddJobTags | Yes | V0.1 | Yes | inline variant |
| BDMA_AllocJobTagList | Yes | V0.1 | Yes | parses and checks the tags once; the engine starts with the first job |
| BDMA_AllocJobTags | Yes | V0.1 | Yes | inline variant |
| BDMA_CheckJob | Yes | V0.1 | Yes | TRUE when the job was replied |
| BDMA_CloseClient | Yes | V0.1 | Yes | aborts what is in flight, frees the jobs and the port of its own |
| BDMA_FreeJob | Yes | V0.1 | Yes | gives a job back (not one in flight) |
| BDMA_OpenClientTagList | Yes | V0.1 | Yes | a session: name, default reply port (or one of its own), default priority |
| BDMA_OpenClientTags | Yes | V0.1 | Yes | inline variant |
| BDMA_QueryInfoTagList | Yes | V0.1 | Yes | writes the answers of the attributes asked for (availability, version, model, features, channels, classes, rows, counters); a tag it does not know leaves its place alone |
| BDMA_QueryInfoTags | Yes | V0.1 | Yes | inline variant generated from the SFD (not in the table of the resource) |
| BDMA_QueryJobTagList | Yes | V0.1 | Yes | state, error and bytes done of a job |
| BDMA_QueryJobTags | Yes | V0.1 | Yes | inline variant |
| BDMA_SetJobTagList | Yes | V0.1 | Yes | changes a job that is not in flight; a refused change leaves it as it was |
| BDMA_SetJobTags | Yes | V0.1 | Yes | inline variant |
| BDMA_StartJob | Yes | V0.1 | Yes | queues the job (no allocation, no parsing: a software interrupt may call it); channels, order of the jobs, priorities, slices of big jobs, cache |
| BDMA_WaitJob | Yes | V0.1 | Yes | sleeps until the job is replied and gives the result; any task may wait for any job, a second call gives it again |
