# XCloud_Ai_Player_CV

A Win32 screen-color trigger automation tool, built and compiled via GitHub Actions.

## What it does

1. Captures a screen region with GDI `BitBlt` into a single 32bpp `DIBSection`.
2. Downsamples that region to a small buffer (default 160x120).
3. Classifies each downsampled pixel in O(1) through a precomputed RGB555 color LUT.
4. Scans the buffer in blocks; when a block's hit ratio crosses a threshold, emits a
   mouse move and click via `SendInput`.

`F8` stops the loop via a `RegisterHotKey` message handler.

Win32 API only. No GPU, no ML, no third-party libraries.

## Build

Actions builds on every push to `main`; the binary is attached as the
`trigger-win64` artifact.

Local build with MinGW:

```
gcc -O2 -o trigger.exe main.c -lgdi32 -luser32
```

MSVC:

```
cl /O2 main.c user32.lib gdi32.lib
```

## Usage

```
trigger.exe [options]

  --color RRGGBB      trigger color, repeatable (max 8, default FF0000)
  --tolerance N      per-channel match radius, 0-255 (default 40)
  --region x,y,w,h   capture region, virtual-desktop pixels
                      (default: 480x360 centered on the primary monitor)
  --size WxH         downsampled buffer size (default 160x120)
  --block N          block edge in downsampled pixels (default 8)
  --threshold PCT    hit percent per block to trigger (default 8)
  --cooldown MS      minimum gap between emissions (default 400)
  --rearm N          clear frames required before re-arming (default 10)
  --fps N            target capture rate (default 60)
  --best             scan every block, trigger on the strongest
  --move-only        move the cursor, never click
  --quiet            suppress per-trigger output
  --help             this text
```

Press `F8` to stop.

Example: watch for a red pixel cluster in the top-left quadrant and move the cursor to it.

```
trigger.exe --color FF2020 --tolerance 60 --region 0,0,640,360 --threshold 5
```

## Optimization notes

**Color LUT.** Naive classification compares every pixel against every target color
with a squared-distance test. Instead, all 32768 possible RGB555 values are
classified once at startup, so the inner loop is a single array index. Tolerance is
squared once at startup so the hot path needs no multiply, and never a `sqrt`.

**Stride downsampling.** GDI has no cheap downscaled capture path; `StretchBlt` still
touches every source pixel and pays for color management. The tool reads exactly
`dstW * dstH` pixels instead, so per-frame work scales with the output buffer rather
than the region size. Integer index mapping avoids cumulative drift.

**Block scan with early exit.** Scanning the whole frame wastes work on the ~95% of
pixels that are background. Blocks are tested against a hit *ratio* (not a raw count),
so partial edge blocks behave correctly, and the scan returns on the first block that
crosses the threshold. `--best` disables the early exit to scan every block.

**Hysteresis.** A trigger usually stays on screen for many consecutive frames. The
tool emits once, then requires `--rearm` clear frames before re-arming, so a single
visual event produces a single action rather than one per frame.

**Memory.** There is exactly one large allocation: the capture DIB at
`region_w * region_h * 4` bytes. A 480x360 region is 691KB; a full 1920x1080 region is
8.3MB. Shrinking the region is by far the biggest memory lever. The LUT is 32KB and
the downsampled buffer is 76KB.

## Scope

This is a generic local automation tool built on documented Win32 APIs. Using it to
gain an advantage in an online game is cheating and breaches those games' terms of
service. It is intended for automating your own software, UI testing, and
accessibility work.

The build artifact is unsigned, so SmartScreen or antivirus may flag it.
