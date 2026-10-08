# Desktop benchmark: what the DMA gives to the Workbench

2026-10-08. Raspberry Pi 4B at 1.8 GHz (66 to 69 C), Emu68 1.1.0-beta.1, 68040 at 500 MHz, VideoCore.card (branch `brcm-dma-client`) as the client of the resource.
The scripts are in `test/bench/` (`bench.sh`, `matrix.sh`, `parse.py`); `brcm-dma-screen` opens the screen of the size under test, `vcdmaset` sets the threshold of the DMA at run time
and `vcwin` runs the workloads (`SCROLL FILL LMOVE LSIZE MIX`, 2 s each) on that screen. A cell is **operations a second (millions of CPU cycles an operation)**: the cycles are the active
cycles of the 68k read from the counters of Emu68, a measure of the CPU that is left to the others. One measure per cell, the noise between runs is not known (a few percent).

Window sizes: 900x600 on the 1280x720 screen, 1400x800 on 1920x1080, 1400x900 on 1920x1200 (and 300x200 on each: nothing differs there from one screen to another).
SCROLL = `ScrollRaster` (a scroll inside the window), FILL = `RectFill`, LMOVE = `MoveLayer`, LSIZE = `SizeLayer`, MIX = a busy desktop.

## 1. Threshold of the copies: DMA off, 8192, 32768 (engine before the two channel split)

| Screen | Window | Test | DMA off: ops/s (Mcycles/op) | 8192 | 32768 |
|---|---|---|---:|---:|---:|
| 1280x720 | 900x600 | ScrollRaster | 421 (4.27) | 527 (0.89) | 529 (0.88) |
| 1280x720 | 900x600 | RectFill | 635 (2.84) | 635 (2.83) | 634 (2.84) |
| 1280x720 | 900x600 | MoveLayer | 189 (9.49) | 174 (10.10) | 188 (9.55) |
| 1280x720 | 900x600 | SizeLayer | 2663 (0.68) | 1886 (0.80) | 3043 (0.59) |
| 1280x720 | 900x600 | Desktop mix | 346 (5.20) | 346 (4.86) | 355 (4.79) |
| 1280x720 | 300x200 | ScrollRaster | 2125 (0.85) | 2692 (0.42) | 2687 (0.42) |
| 1280x720 | 300x200 | RectFill | 5275 (0.34) | 5278 (0.34) | 5274 (0.34) |
| 1280x720 | 300x200 | MoveLayer | 759 (2.37) | 747 (2.41) | 746 (2.41) |
| 1280x720 | 300x200 | SizeLayer | 4794 (0.38) | 3109 (0.53) | 4806 (0.37) |
| 1280x720 | 300x200 | Desktop mix | 2104 (0.86) | 2107 (0.83) | 2104 (0.83) |
| 1920x1080 | 1400x800 | ScrollRaster | 278 (6.46) | 274 (1.31) | 275 (1.29) |
| 1920x1080 | 1400x800 | RectFill | 307 (5.87) | 307 (5.86) | 307 (5.86) |
| 1920x1080 | 1400x800 | MoveLayer | 96 (18.72) | 91 (19.41) | 95 (18.77) |
| 1920x1080 | 1400x800 | SizeLayer | 2519 (0.71) | 1656 (0.89) | 2821 (0.64) |
| 1920x1080 | 1400x800 | Desktop mix | 179 (10.06) | 175 (9.59) | 178 (9.43) |
| 1920x1080 | 300x200 | ScrollRaster | 2118 (0.85) | 2696 (0.41) | 2698 (0.41) |
| 1920x1080 | 300x200 | RectFill | 5266 (0.34) | 5273 (0.34) | 5257 (0.34) |
| 1920x1080 | 300x200 | MoveLayer | 759 (2.37) | 747 (2.41) | 749 (2.40) |
| 1920x1080 | 300x200 | SizeLayer | 4788 (0.38) | 3122 (0.53) | 4798 (0.38) |
| 1920x1080 | 300x200 | Desktop mix | 2104 (0.86) | 2107 (0.83) | 2110 (0.83) |
| 1920x1200 | 1400x900 | ScrollRaster | 248 (7.24) | 245 (1.40) | 245 (1.39) |
| 1920x1200 | 1400x900 | RectFill | 272 (6.60) | 272 (6.61) | 272 (6.60) |
| 1920x1200 | 1400x900 | MoveLayer | 85 (20.97) | 81 (21.69) | 85 (21.01) |
| 1920x1200 | 1400x900 | SizeLayer | 3126 (0.58) | 1723 (0.83) | 3215 (0.56) |
| 1920x1200 | 1400x900 | Desktop mix | 157 (11.46) | 155 (10.80) | 157 (10.69) |
| 1920x1200 | 300x200 | ScrollRaster | 2121 (0.85) | 2689 (0.42) | 2703 (0.41) |
| 1920x1200 | 300x200 | RectFill | 5278 (0.34) | 5279 (0.34) | 5277 (0.34) |
| 1920x1200 | 300x200 | MoveLayer | 761 (2.37) | 744 (2.42) | 748 (2.40) |
| 1920x1200 | 300x200 | SizeLayer | 4778 (0.38) | 3114 (0.53) | 4779 (0.38) |
| 1920x1200 | 300x200 | Desktop mix | 2104 (0.86) | 2109 (0.83) | 2107 (0.83) |

- **ScrollRaster**: the CPU needs 80% fewer cycles with the DMA (4.27M to 0.89M at 720p, 6.46M to 1.31M at 1080p, 7.24M to 1.40M at 1200p); the rate is +25% at 720p, unchanged at 1080p and 1200p.
  Both thresholds give the same: the scroll jobs are all big.
- **SizeLayer**: a threshold of **8192 is 29 to 35% slower than no DMA**; 32768 is neutral or better. **MoveLayer**: 8192 -8%, 32768 neutral. The jobs between 8 and 32 KB cost more by DMA than by the CPU.
- **32768 is better than 8192 in every test**: it is the default of the driver; the ToolType `VC6_DMA_THRESHOLD=8192` of the machine is not the best.
- **RectFill** (no DMA for the fills by default) is the CPU hog: 2.84M cycles an operation at 720p, 5.87M at 1080p, 6.60M at 1200p, the whole CPU at 270 to 640 operations a second.
- **MoveLayer** (9.5M to 21M cycles) is CPU work in the driver: a copy between two different bitmaps (`BlitRectNoMaskComplete`) does not go through the DMA.

## 2. With the two channel split, thresholds 8192 to 131072

| Screen | Window | Test | DMA 8192 | DMA 32768 | DMA 65536 | DMA 131072 |
|---|---|---|---:|---:|---:|---:|
| 1280x720 | 900x600 | ScrollRaster | 521 (0.93) | 522 (0.92) | 522 (0.92) | 521 (0.93) |
| 1280x720 | 900x600 | RectFill | 635 (2.83) | 635 (2.83) | 635 (2.83) | 635 (2.83) |
| 1280x720 | 900x600 | MoveLayer | 173 (10.17) | 188 (9.55) | 188 (9.54) | 188 (9.54) |
| 1280x720 | 900x600 | SizeLayer | 1809 (0.85) | 2965 (0.61) | 2610 (0.69) | 2609 (0.69) |
| 1280x720 | 900x600 | Desktop mix | 345 (4.87) | 353 (4.81) | 353 (4.80) | 353 (4.80) |
| 1920x1080 | 1400x800 | ScrollRaster | 272 (1.36) | 272 (1.36) | 273 (1.35) | 272 (1.37) |
| 1920x1080 | 1400x800 | RectFill | 307 (5.86) | 307 (5.86) | 306 (5.87) | 307 (5.86) |
| 1920x1080 | 1400x800 | MoveLayer | 91 (19.41) | 95 (18.76) | 95 (18.77) | 96 (18.75) |
| 1920x1080 | 1400x800 | SizeLayer | 1567 (0.95) | 2746 (0.65) | 2474 (0.73) | 2462 (0.73) |
| 1920x1080 | 1400x800 | Desktop mix | 174 (9.61) | 178 (9.43) | 178 (9.43) | 177 (9.50) |
| 1920x1200 | 1400x900 | ScrollRaster | 243 (1.46) | 243 (1.47) | 243 (1.47) | 242 (1.48) |
| 1920x1200 | 1400x900 | RectFill | 272 (6.62) | 272 (6.61) | 272 (6.60) | 272 (6.62) |
| 1920x1200 | 1400x900 | MoveLayer | 81 (21.72) | 85 (21.01) | 85 (21.00) | 85 (21.01) |
| 1920x1200 | 1400x900 | SizeLayer | 1630 (0.89) | 3120 (0.58) | 3057 (0.59) | 3067 (0.59) |
| 1920x1200 | 1400x900 | Desktop mix | 155 (10.77) | 157 (10.75) | 157 (10.69) | 157 (10.68) |

The scrolls are moves inside a bitmap (reads and writes overlap): they are not split, and the numbers do not change. 65536 and 131072 are worse than 32768 for `SizeLayer`.
The second channel works when the jobs are copies, fills or rectangles of 1 MB or more: `brcm-dma-stress BIG` (16 MB jobs) gives a copy of 1.67 GB/s (1.0 before), a fill of 1.98 GB/s (1.25),
a 1280 rows rectangle of 1.61 GB/s (1.1); `CopyMemQuick` does 1.64 GB/s.

## 3. The DMA does the fills too: `VC6_DMA_THRESHOLD_FILLRECT`

Threshold of the copies 32768, 32 bit fills by DMA from 8 KB (`=8192`) or from 512 KB (`=524288`). With 8192 the fills of the windows are slower than the CPU (`SizeLayer` 3043 to 1070
operations a second, `RectFill` 300x200 5275 to 2920) because the cost of a job (about 150 us, with the sleep and the wake up of the task) is more than the CPU needs for a small fill.

With **524288**:

| Screen | Window | Test | DMA 32768 |
|---|---|---|---:|
| 1280x720 | 900x600 | ScrollRaster | 530 (0.88) |
| 1280x720 | 900x600 | RectFill | 809 (0.68) |
| 1280x720 | 900x600 | MoveLayer | 188 (9.56) |
| 1280x720 | 900x600 | SizeLayer | 3004 (0.60) |
| 1280x720 | 900x600 | Desktop mix | 357 (4.55) |
| 1920x1080 | 1400x800 | ScrollRaster | 275 (1.29) |
| 1920x1080 | 1400x800 | RectFill | 433 (1.09) |
| 1920x1080 | 1400x800 | MoveLayer | 95 (18.78) |
| 1920x1080 | 1400x800 | SizeLayer | 2791 (0.64) |
| 1920x1080 | 1400x800 | Desktop mix | 182 (8.83) |
| 1920x1200 | 1400x900 | ScrollRaster | 245 (1.41) |
| 1920x1200 | 1400x900 | RectFill | 364 (1.21) |
| 1920x1200 | 1400x900 | MoveLayer | 85 (21.01) |
| 1920x1200 | 1400x900 | SizeLayer | 3135 (0.57) |
| 1920x1200 | 1400x900 | Desktop mix | 160 (10.03) |

RectFill: **+27% (720p), +41% (1080p), +34% (1200p)** and 76 to 82% fewer CPU cycles (2.83M to 0.68M, 5.86M to 1.09M, 6.62M to 1.21M); `SizeLayer` is back to its value without DMA. Both channels work (1.5 to 1.8 s of the second one).

## 4. What it says

- Threshold of the copies: **32768**. Threshold of the fills: **524288** (about 512 KB). The ToolTypes of the machine read `VC6_DMA_THRESHOLD=8192` and no fill threshold.
- The CPU is left free where the jobs are big: scrolls (-80% of cycles), big fills (-80%). The copies between two bitmaps (`MoveLayer`, the main CPU cost left at 9 to 21M cycles) are the next target, in the driver.
