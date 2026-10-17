# P96Speed results

P96Speed 1.2 at 640x480, one run per depth, results in operations per second. The screenshots are in `p96speed/<date>/`.
One measure per cell, the noise between runs is not known: a difference of a few percent means nothing.

## 2026-10-08: VideoCore.card as a client of brcm-dma.resource

Machine: Amiga 1200, Pi 4B at 1.8 GHz, 68040 at 500 MHz (Emu68). Driver `VideoCore.card.brcm-dma-client3`, DMA through the resource (`VC6_DMA_RESOURCE=Yes`, threshold 8192),
image `Emu68-pistorm.gz.brcm-dma-stats`. Counters of the resource at the end of the four runs and the desktop use before: 360962 jobs, 71.8 GB, 0 failed, 0 aborted,
0 timeouts, queue max 1, longest wait 818 us; sizes `<=32K` 228579, `<=1M` 103741, `>1M` 28642. No restart, no visible defect.

| Test | 8 bit | 16 bit | 24 bit | 32 bit |
|---|---:|---:|---:|---:|
| RectFill() | 13843 | 10946 | 9301 | 7351 |
| RectFill() Pattern | 2353 | 2211 | 1459 | 2414 |
| WritePixel() | 942922 | 1007525 | 978261 | 1015711 |
| WriteChunkyPixels() | 27893 | 1320 | 1658 | 1609 |
| WritePixelArray8() | 28067 | 1752 | 1640 | 1370 |
| WritePixelLine8() | 46826 | 36542 | 37044 | 37048 |
| DrawEllipse() | 42320 | 42095 | 40539 | 42222 |
| DrawCircle() | 43385 | 43226 | 40544 | 42943 |
| Draw() | 22747 | 23418 | 29446 | 28086 |
| Draw() Hor/Ver | 29481 | 33464 | 32196 | 36573 |
| **ScrollRaster() X** | **19** | **19** | **19** | **255** |
| ScrollRaster() Y | 1561 | 1110 | 924 | 750 |
| Text() | 18552 | 18447 | 15130 | 28261 |
| BltBitMap() | 14719 | 5723 | 4958 | 3566 |
| BitMapScaling() | 1177 | 1028 | 907 | 1024 |
| BltBitMapRastPort() | 13703 | 5752 | 5091 | 3543 |
| OpenWindow() | 179 | 156 | 157 | 177 |
| MoveWindow() | 7566 | 10341 | 5408 | 1524 |
| SizeWindow() | 817 | 118063 | 197180 | 858 |
| CON-Output | 937 | 820 | 647 | 806 |
| ScreenToFront() | 53 | 53 | 52 | 53 |

### What stands out

- **ScrollRaster() X is 19 op/s in 8, 16 and 24 bit, and 255 in 32 bit** (13 times faster). The value 19 is the same in three depths whose pixels are 1, 2 and 3 bytes, so the time does not depend on the amount of data:
  it looks like a slow generic path. It is not the DMA: the scroll is a shift by less than the width inside a row, which the resource refuses by design (`BDERR_OVERLAP`), and the same 19 was measured on 2026-10-06 with the DMA off.
  With `NOBLITTER=Yes` (acceleration of the card off) the 8 bit value was 76, so the acceleration of the card makes it slower than the software path. To investigate in VideoCore.card (the hook that handles a scroll inside a row, and why only the 32 bit has a fast path).
- **SizeWindow() gives absurd values** (118063, 197180 op/s) in 16 and 24 bit, as it did in 8 and 24 bit on other days, also without the blitter or the DMA: a test that does almost no work, independent of the card.
- **BltBitMap and BltBitMapRastPort fall with the depth** (14719 in 8 bit, 3566 in 32 bit): the amount of data grows with the depth, as expected.
- **ScrollRaster() Y is probably limited by the DMA**: 750 op/s in 32 bit is about 1.3 ms an operation for a 1.2 MB surface at about 1 GB/s.
- **MoveWindow() is irregular** between depths (10341 in 16 bit, 1524 in 32 bit): to explain, it depends on the sizes of the rectangles the DMA takes (threshold 8192).

### Against 2026-10-06 (same card code, other build of the resource and of the tools)

| Test | Depth | 2026-10-06 | 2026-10-08 | Change |
|---|---|---:|---:|---:|
| BltBitMap() | 16 / 24 / 32 | 8076 / 6662 / 5595 | 5723 / 4958 / 3566 | -29% / -26% / -36% |
| CON-Output | 16 / 24 / 32 | 1101 / 925 / 1010 | 820 / 647 / 806 | -26% / -30% / -20% |
| ScrollRaster() Y | 8 / 16 / 24 / 32 | 1934 / 1360 / 1054 / 878 | 1561 / 1110 / 924 / 750 | -19% / -18% / -12% / -15% |
| MoveWindow() | 8 / 16 / 24 / 32 | 5935 / 5449 / 3013 / 2056 | 7566 / 10341 / 5408 / 1524 | +27% / +90% / +80% / -26% |
| ScrollRaster() X | 8 / 16 / 24 / 32 | 19 / 19 / 19 / 257 | 19 / 19 / 19 / 255 | none |
| RectFill() | 8 / 16 / 24 / 32 | 13876 / 10879 / 9715 / 7420 | 13843 / 10946 / 9301 / 7351 | none |
| ScreenToFront() | all | 58 | 52 to 53 | -9% |

The drops are not explained by the changes to the resource of the day (statistics: two reads of a timer a job, about 10 us): they would need about 100 us an operation.
To tell the DMA from the rest: the same driver with `VC6_DMA_ENABLE=No` then `Yes` in the same session, on ScrollRaster Y, BltBitMap, MoveWindow and CON-Output in 32 bit.

## 2026-10-08: desktop use at 1080p, 32 bit (a few minutes, the card as a client, after `BDMA_ResetStatistics` was installed)

Counters of the resource read afterwards (the duration of the session was not timed): 2432 jobs, 785 MB (323 KB a job on average), 0 failed, 0 aborted, 0 timeouts, queue max 1, longest wait 579 us,
channel 12 busy 926 ms, channel 13 busy 0 ms.

| Size of the job | Jobs | Share |
|---|---:|---:|
| up to 64 B / 4 KB | 0 | 0% |
| up to 32 KB | 884 | 36% |
| up to 1 MB | 1297 | 53% |
| more than 1 MB | 251 | 10% |

The big jobs carry nearly all the bytes. The channel is busy about 850 MB/s when it works. The queue never holds more than one job: the card waits for each job, so the second channel is never used.
