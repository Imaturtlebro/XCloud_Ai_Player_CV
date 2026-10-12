# XCloud_Ai_Player_CV

Two Win32 tools for sampling and reacting to on-screen colour, built automatically
by GitHub Actions.

- **`picker.exe`** — a magnifier that reads the exact RGB under your cursor.
- **`trigger.exe`** — watches a small circle at screen centre and clicks when a
  target colour appears there.

No GPU, no ML, no third-party libraries. Win32 API only.

## Build

Actions builds on every push to `main`; both binaries are attached as the
`win64-binaries` artifact and committed back to the repo root.

```
gcc -O2 -Wall -Wextra -o trigger.exe trigger.c -lgdi32 -luser32
gcc -O2 -Wall -Wextra -o picker.exe  picker.c  -lgdi32 -luser32
```

## 1. Finding the target colour — picker.exe

Run it, then move the mouse over whatever you want to sample. The window is a
16x16 pixel patch magnified 12x, drawn with `COLORONCOLOR` so each screen pixel
maps to a solid block — a smoothed or averaged magnifier would report colours
that are not actually on screen.

| key | action |
| --- | --- |
| move mouse | magnifier follows the cursor |
| left click | record the colour under the cursor |
| `C` | copy the recorded list to the clipboard |
| `S` | write the list to `picked-colors.txt` |
| `Esc` | quit |

The readout is taken from the same captured buffer that is drawn, so the number
in the title bar and the green-highlighted pixel cannot disagree.

Sample several pixels — the lit centre of a uniform and a shadowed edge are
different values. Multiple `color` lines are supported and matched in order.

## 2. trigger.exe

Watches a circle of radius `--radius` (default 30px) at screen centre. When the
matched-pixel ratio inside that circle crosses a threshold, it moves the cursor
to the centroid of the matched pixels and clicks.

| key | action |
| --- | --- |
| `F8` | stop |
| `F9` | force re-arm, overriding the hysteresis timer |

### Options

```
--config FILE       read settings from FILE (default: trigger.cfg if present)
--color RRGGBB      target colour, repeatable, max 8
--tolerance N       RGB match radius, 0-255 (default 40)
--radius N          detection circle radius in px (default 30)
--center X,Y        circle centre (default: screen centre)
--threshold PCT     percent of circle area that must match (default 5)
--min-hits N        absolute minimum matching pixels (default 3)
--rearm-ms N        target must be absent this long before re-arming (default 2500)
--cooldown-ms N     minimum gap between emissions (default 200)
--fps N             capture rate (default 60)
--move-only         move the cursor, never click
--quiet             suppress per-trigger output
--save-config FILE  write effective settings out and exit
```

### Config file

`trigger.cfg` in the working directory is read automatically if present. Command
line arguments override it.

```ini
# repeat for multiple targets
color       = E03A28
color       = F26B5B
tolerance   = 40
zone_radius = 30
threshold   = 5
min_hits    = 3
rearm_ms    = 2500
cooldown_ms = 200
fps         = 60
move_only   = 0
```

`trigger.exe --save-config out.cfg` writes the current effective settings, so you
can capture a known-good state and edit from there.

### Compression tolerance

Over a compressed video stream, a flat colour is not a single RGB value — block
artefacts and chroma subsampling shift individual pixels by a surprising amount.
The default tolerance of 40 is deliberately wide. Start wide and tighten until
you start missing the target; a tolerance that is too tight on a compressed feed
will drop frames and look intermittent.

If matching is too easy, lower it. If it is intermittent, raise it.

## Design notes

**Circular zone, no down-sampling.** The capture is exactly the bounding box of
the circle — 61x61 pixels at the default radius, 14KB. Cost is O(radius²) instead
of O(screen area), so the detector can run at 60fps while touching far fewer
pixels than a single full-screen grab. Because the region is already small, it is
scanned 1:1; reducing a large capture to a small buffer and hoping the stride
lands on the target is strictly worse here.

**Colour LUT.** Comparing every pixel against every target colour is the obvious
approach and costs up to 8 distance computations per pixel. Instead all 32768
possible RGB555 values are classified once at startup, so the inner loop is a
single array index. Tolerance is squared at startup so the hot path needs no
multiplication and never a `sqrt`.

The match region is a sphere in RGB space, not a per-channel box. Both are common;
the sphere is rounder and less likely to accept colours that are close in one
channel but far in the others.

**Strict hysteresis.** A target stays on screen for many consecutive frames. The
tool fires once, then requires the target to be *continuously* absent for the full
`rearm_ms` window before re-arming — so one visual event produces exactly one
action instead of one per frame. `F9` re-arms immediately if you want to override
the timer.

**Centroid targeting.** The click lands on the centroid of the matched pixels,
not the geometric centre of the circle, so it tracks where the target actually
is inside the zone.

**UIPI.** `SendInput` does not bypass User Interface Privilege Isolation. Injecting
into an elevated window or a UAC prompt is blocked; driving one requires an
elevated process. Registration of the hotkeys can also fail if something else
already owns F8 or F9 — the tool warns rather than dying.

## Scope

These are generic local automation tools built on documented Win32 APIs. Using
them to gain an advantage in an online game is cheating and breaches those games'
terms of service. Automating your own software, UI testing, and accessibility work
are the intended uses.

Both binaries are unsigned, so SmartScreen or antivirus may flag them.
