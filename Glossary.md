# Glossary

The following terms are used throughout `brcm-dma.resource` and its documentation.

> **Terminology note:** These definitions describe how the terms are used within `brcm-dma.resource`.

They are intended as a practical reference and do not necessarily constitute formal hardware definitions.

## Generic DMA vocabulary

| Term                     | Definition                                                                                                                                                                                                       |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1D transfer**          | A DMA transfer that processes a single **contiguous region** of memory. The transfer is described by a source address, destination address, and transfer length, with no row/stride semantics. |
| **2D transfer**          | A DMA transfer that processes a **rectangular region** made of multiple rows. Each row has a transfer length, while source and/or destination addresses can advance by a configurable **stride** between rows. This allows DMA to operate on surfaces, images, frames, or other strided data without requiring the CPU to process each row separately. |
| **ABI**                  | **Application Binary Interface**. The binary contract defining calling conventions, data structures, register usage and compatibility requirements.                                                              |
| **Arbitration**          | The process of deciding which DMA resource should execute which pending job when resources are shared.                                                                                                           |
| **Backend registry**     | The internal collection of DMA backends known to `dma.resource`.                                                                                                                                                 |
| **Burst**                | A sequence of consecutive data transfers performed as part of a single hardware transaction or transfer sequence.                                                                                                |
| **Bus address**          | An address as seen by a hardware bus or peripheral. It may differ from the CPU's physical or virtual address.                                                                                                    |
| **Cache coherency**      | The property that ensures CPU caches and DMA-visible memory contain mutually consistent data.                                                                                                                    |
| **Cache maintenance**    | Operations required to make memory contents safe for DMA and CPU access, such as cache cleaning or invalidation.                                                                                                 |
| **Capability**           | A feature supported by a DMA backend or engine, such as 40-bit addressing, cyclic transfers or scatter/gather.                                                                                                   |
| **Channel allocation**   | The process of selecting and reserving a suitable DMA channel for a job.                                                                                                                                         |
| **Client**               | An application, library, device driver or OS component using `dma.resource` to perform DMA operations.                                                                                                           |
| **Completion**           | The event or notification indicating that a DMA job has finished, failed or been aborted.                                                                                                                        |
| **Control Block**        | A DMA control structure containing the information required to execute an operation, such as source, destination, length and control parameters.                                                                 |
| **Cyclic transfer**      | A DMA transfer that automatically repeats, typically used for ring buffers and continuous data streams such as audio.                                                                                            |
| **DMA backend**          | The platform-specific component that translates generic DMA jobs into operations supported by a particular DMA engine.                                                                                           |
| **DMA channel**          | An execution resource of a DMA engine that can perform a DMA operation or sequence of operations.                                                                                                                |
| **DMA engine**           | A hardware block capable of performing DMA operations.                                                                                                                                                           |
| **DMA job**              | A request submitted to `dma.resource` describing an operation to be performed by a DMA engine.                                                                                                                   |
| **DMA**                  | **Direct Memory Access**. A mechanism allowing hardware to transfer data without the CPU handling each individual transfer.                                                                                      |
| **DREQ ID**              | A hardware-specific identifier selecting a peripheral DMA request source.                                                                                                                                        |
| **DREQ**                 | **DMA Request**. A hardware request mechanism used by a peripheral to pace DMA transfers.                                                                                                                        |
| **Dependency**           | A condition requiring one DMA job to wait for another operation or resource before it can execute.                                                                                                               |
| **Descriptor**           | A memory structure describing a DMA operation or a sequence of operations.                                                                                                                                       |
| **Endpoint requirement** | A job requirement describing the type and properties of the transfer endpoint, such as memory or a peripheral and, where applicable, its DREQ.                                                                   |
| **Endpoint**             | One end of a DMA transfer. An endpoint is either **memory** or a **peripheral I/O interface**. Memory endpoints are addressed directly; peripheral endpoints may require hardware request/pacing such as a DREQ. |
| **Hardware abstraction** | The separation between the generic DMA interface exposed to clients and the hardware-specific implementation provided by a backend.                                                                              |
| **Interrupt handler**    | Code executed in response to a hardware interrupt.                                                                                                                                                               |
| **Interrupt**            | A hardware notification used to signal events such as DMA completion or errors.                                                                                                                                  |
| **Job slicing**          | Splitting a large DMA job into smaller pieces so that it does not monopolize a DMA resource.                                                                                                                     |
| **Physical address**     | An address identifying a location in physical memory, as opposed to a CPU virtual address.                                                                                                                       |
| **Priority**             | A scheduling level used to determine the relative importance or ordering of DMA jobs.                                                                                                                            |
| **QoS**                  | **Quality of Service**. Constraints or preferences concerning properties such as latency, throughput or priority.                                                                                                |
| **Raw chain**            | A hardware-specific sequence of DMA descriptors or control blocks submitted with reduced abstraction, typically for advanced or backend-specific use.                                                            |
| **Recovery**             | The process of restoring a DMA engine or channel to a usable state after an error, timeout or other failure.                                                                                                     |
| **Requirement**          | A capability or property that a DMA job requires in order to be executed correctly.                                                                                                                              |
| **Scatter/Gather**       | A DMA operation involving multiple non-contiguous memory regions, allowing them to be processed as a single logical transfer.                                                                                    |
| **Scheduler**            | The component responsible for determining the order in which pending DMA jobs are executed.                                                                                                                      |
| **SoC**                  | **System on Chip**. An integrated chip containing CPU cores and various hardware peripherals and controllers.                                                                                                    |
| **Stride**               | The distance between successive rows or elements in memory. Strides allow DMA to operate on non-contiguous 2D data.                                                                                              |
| **TagList**              | The AmigaOS mechanism for passing extensible parameters as tag/value pairs.                                                                                                                                      |
| **Timeout**              | A condition in which a DMA operation does not complete within its allowed execution time.                                                                                                                        |
| **Watchdog**             | A mechanism used to detect DMA operations that have become stuck or exceeded an expected execution time.                                                                                                         |

## Raspberry Pi / Broadcom-specific vocabulary

| Term                         | Definition                                                                                                                                                                                                                                |
| ---------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **ARMCTRL**                  | The Broadcom-specific interrupt controller used by the BCM2835 family. On BCM2836/BCM2837, it is combined with the ARM-local per-CPU interrupt controller. |
| **BCM2711**                  | Broadcom SoC used in the Raspberry Pi 4 family. It contains four ARM Cortex-A72 CPU cores and an ARM GIC-400 (GICv2) interrupt controller. |
| **BCM2835**                  | Broadcom SoC used in the original Raspberry Pi and early Raspberry Pi Zero models. It uses the Broadcom ARMCTRL interrupt controller. |
| **BCM2836**                  | Broadcom SoC used in the original Raspberry Pi 2. It contains four ARM Cortex-A7 CPU cores and uses the BCM2835-style ARMCTRL together with an ARM-local per-CPU interrupt controller. |
| **BCM2837**                  | Broadcom SoC used in the Raspberry Pi 3 family and Compute Module 3. It contains four ARM Cortex-A53 CPU cores and uses the Broadcom interrupt architecture with the BCM2835-style ARMCTRL and an ARM-local per-CPU interrupt controller. |
| **Broadcom**                 | A semiconductor company that designs and develops SoCs, processors, networking devices and other hardware components. Broadcom develops the BCM28xx and BCM27xx SoCs used in various Raspberry Pi generations. |
| **DMA32**                    | A **BCM2711** DMA engine operating with **32-bit** addressing. |
| **DMA40**                    | A **BCM2711** DMA engine supporting **40-bit** addressing and additional DMA capabilities beyond the legacy DMA controllers. |
| **DREQ ID**                  | On Raspberry Pi DMA hardware, a numeric identifier selecting the peripheral request line used to pace a DMA transfer. |
| **Device Tree (DT)**         | A data structure that describes the hardware components, resources, and configuration of a platform to software. It allows platform-specific hardware information such as devices, memory regions, interrupts, clocks, and DMA capabilities to be described separately from the software implementing their drivers. |
| **Device Tree Blob (DTB)**   | The compiled binary representation of a Device Tree, typically provided to firmware or an operating system at boot. |
| **Device Tree Source (DTS)** | The human-readable source format used to describe a platform's hardware in a Device Tree. It is compiled into a **Device Tree Blob (DTB)**. |
| **GIC**                      | **Generic Interrupt Controller**. An ARM-standard interrupt controller used to route and manage hardware interrupts. The BCM2711 uses a GIC-400 (GICv2). |
| **GIC-400**                  | ARM's implementation of a GICv2 interrupt controller. It is used as the primary ARM interrupt controller in the BCM2711. |
| **Mailbox**                  | The BCM/Raspberry Pi inter-processor communication mechanism used to exchange messages between the ARM CPU and VideoCore firmware. |
| **Property Mailbox**         | A mailbox interface used to send structured property requests to the Raspberry Pi firmware, including information and configuration for displays, clocks and other hardware. |
| **VC4**                      | **VideoCore IV**, the GPU architecture used by BCM2835, BCM2836 and BCM2837. |
| **VC6**                      | **VideoCore VI**, the GPU architecture used by BCM2711. |
| **VPU**                      | **VideoCore Processing Unit**. A processor within the VideoCore architecture used for various system, multimedia and GPU-related tasks. |
| **VideoCore**                | Broadcom's GPU architecture used in Raspberry Pi SoCs. On the BCM2711, the VideoCore VI GPU provides graphics and multimedia functionality. |

## PiStorm / Emu68 specific vocabulary

| Term                    | Definition                                                                                                                                                                                                                                                   |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Emu68**               | A high-performance Motorola 68040 CPU+FPU **JIT emulator** developed for Raspberry Pi systems, notably used with PiStorm, but not only, providing the 68k execution environment. **Michal Schulz** is the author and principal developer of Emu68. |
| **PiStorm**              | An Amiga **hardware accelerator** project that uses a Raspberry Pi as a companion processor. The PiStorm interfaces bidirectionally with the Amiga bus and the Raspberry Pi GPIOs, and is powered from the Amiga. **Claude Schwarz** is the creator and original author of the PiStorm project. |
| **PiStorm / Emu68**      | An Amiga acceleration platform in which **PiStorm** provides the hardware interface between the Amiga bus and the Raspberry Pi, while **Emu68** provides the 68k CPU emulation/JIT environment running on the Raspberry Pi. |
| **gic400.library**       | An AmigaOS library providing an interface to the **ARM GIC-400 interrupt controller** used by the BCM2711. It abstracts interrupt-controller operations from clients such as device drivers and system components. |
| **devicetree.resource**  | An AmigaOS system resource providing access to the platform's **Device Tree**, allowing software to discover hardware devices and their resources without hard-coding platform-specific information. |
| **mailbox.resource**     | An AmigaOS system resource providing access to the **Raspberry Pi mailbox interface**, including the Property Mailbox used to communicate with the VideoCore firmware for platform configuration and hardware services. |
