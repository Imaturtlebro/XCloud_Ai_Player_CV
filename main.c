/*
 * screencolor-trigger.c
 *
 * A screen-color trigger automation tool.
 *
 *   1. Capture a screen region with GDI BitBlt into a single DIBSection.
 *   2. Downsample it to a small buffer (default 160x120).
 *   3. Classify every downsampled pixel in O(1) via an RGB555 color LUT
 *      that is precomputed once at startup.
 *   4. Scan in blocks; when a block exceeds a hit threshold, emit input.
 *
 * Win32 API only. No GPU, no ML, no third-party libraries. All buffers are
 * statically allocated so total allocation is known at compile time.
 *
 * Build:  gcc -O2 -o trigger.exe main.c -lgdi32 -luser32
 *         cl  /O2 main.c user32.lib gdi32.lib
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Compile-time configuration                                          */
/* ------------------------------------------------------------------ */

#define MAX_COLORS   8      /* distinct trigger colors supported at once */
#define DEFAULT_W    160    /* downsampled buffer width  */
#define DEFAULT_H    120    /* downsampled buffer height */
#define MAX_BLOCK    64     /* max block edge, in downsampled pixels */
#define HOTKEY_ID    1
#define HOTKEY_VK    VK_F8

/* The LUT is indexed by an RGB555 value: 32 values per channel, so 2^15
 * entries, 32KB of table. */
#define LUT_SIZE     32768

/* ------------------------------------------------------------------ */
/* Global state (static storage: no malloc in the hot loop)             */
/* ------------------------------------------------------------------ */

typedef struct { int r, g, b; } RGB;

static RGB        g_colors[MAX_COLORS];
static int        g_colorCount = 0;
static int        g_tol        = 40;      /* per-channel radius, for display  */
static int        g_tol2       = 0;       /* squared tolerance, avoids sqrt() */
static uint8_t    g_lut[LUT_SIZE];        /* 0 = no match, else color index+1 */

/* Capture region, in virtual-desktop pixels. */
static int  g_regX, g_regY, g_regW, g_regH;
static int  g_srcW, g_srcH;               /* region size, in pixels          */

/* Downsampled working buffer. */
static int        g_dstW = DEFAULT_W;
static int        g_dstH = DEFAULT_H;
static uint32_t  *g_frame = NULL;         /* g_dstW * g_dstH pixels          */

/* Detection parameters. */
static int  g_blockPx   = 8;              /* block edge in downsampled pixels */
static int  g_threshold = 8;              /* hit percentage required per block */
static int  g_cooldownMs   = 400;         /* min ms between emitted triggers  */
static int  g_rearmFrames  = 10;          /* clear frames needed before re-arm */
static int  g_fps        = 60;            /* target capture rate             */
static int  g_moveOnly   = 0;             /* move cursor, never click        */
static int  g_best       = 0;             /* 0 = early-exit, 1 = scan all    */
static int  g_quiet      = 0;

static volatile LONG g_quit = 0;

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static void die(const char *msg)
{
    fprintf(stderr, "error: %s (win32=%lu)\n", msg, (unsigned long)GetLastError());
    exit(1);
}

static int64_t now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (t.QuadPart * 1000) / freq.QuadPart;
}

/* ------------------------------------------------------------------ */
/* Color LUT                                                           */
/*                                                                     */
/* Naive per-pixel classification would compare against every target    */
/* color with a squared-distance test -- up to MAX_COLORS multiply-adds */
/* per pixel. Instead we precompute the entire answer: for each of the  */
/* 32768 possible RGB555 inputs, store which target color it matches (or */
/* 0 for none). At runtime one array index replaces all of that.        */
/*                                                                     */
/* The cost moves to startup: 32768 entries x MAX_COLORS comparisons,   */
/* done once.                                                           */
/* ------------------------------------------------------------------ */

static int dist2(int r, int g, int b, const RGB *c)
{
    int dr = r - c->r;
    int dg = g - c->g;
    int db = b - c->b;
    return dr * dr + dg * dg + db * db;
}

static void build_lut(void)
{
    int i, k;

    for (i = 0; i < LUT_SIZE; i++) {
        int r = ((i >> 10) & 0x1F) << 3;   /* expand 5 bits -> 8 */
        int g = ((i >>  5) & 0x1F) << 3;
        int b = ( i        & 0x1F) << 3;

        g_lut[i] = 0;
        for (k = 0; k < g_colorCount; k++) {
            if (dist2(r, g, b, &g_colors[k]) <= g_tol2) {
                g_lut[i] = (uint8_t)(k + 1);
                break;
            }
        }
    }
}

/* 32bpp BI_RGB pixels are 0x00RRGGBB. Packing to RGB555 keeps the LUT
 * small; note this quantizes to 5 bits per channel, which is deliberate:
 * real captures are full of compression noise and blending, so extra
 * precision below 5 bits would only match near-exact pixels. */
static inline int rgb555(uint32_t px)
{
    return (int)(((px >> 19) & 0x1F) << 10)
         | (int)(((px >> 11) & 0x1F) <<  5)
         | (int)(( px >>  3) & 0x1F);
}

/* ------------------------------------------------------------------ */
/* Capture + downsample                                                */
/* ------------------------------------------------------------------ */

static HDC        g_screenDC = NULL;
static HDC        g_memDC    = NULL;
static HBITMAP  g_dib      = NULL;
static void    *g_bits     = NULL;
static BITMAPINFO g_bmi;

/* Captures the region, then keeps only g_dstW x g_dstH of it.
 *
 * GDI has no cheap "capture downscaled" path -- StretchBlt would still
 * touch every source pixel and its color management costs more than the
 * sampling does. Stride sampling reads exactly dstW*dstH pixels instead,
 * so the work scales with the output buffer, not the region size. */
static void capture_frame(void)
{
    int dx, dy;
    uint32_t *fb = (uint32_t *)g_bits;

    BitBlt(g_memDC, 0, 0, g_srcW, g_srcH,
           g_screenDC, g_regX, g_regY, SRCCOPY);

    for (dy = 0; dy < g_dstH; dy++) {
        /* Integer mapping to avoid cumulative drift across the scan. */
        int sy = (int)((int64_t)dy * g_srcH / g_dstH);
        const uint32_t *row = fb + (size_t)sy * g_srcW;
        for (dx = 0; dx < g_dstW; dx++) {
            int sx = (int)((int64_t)dx * g_srcW / g_dstW);
            g_frame[(size_t)dy * g_dstW + dx] = row[sx];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Block scan                                                          */
/*                                                                     */
/* Whole-frame scanning wastes work on the ~95% of pixels that are      */
/* background. Scanning in blocks and testing a ratio lets us bail out  */
/* on the first block that crosses the threshold. Worst case (never      */
/* triggers) we scan everything, but the common case exits early.       */
/* ------------------------------------------------------------------ */

typedef struct {
    int matched;   /* 1 if a block crossed the threshold */
    int bx, by;    /* block coords in downsampled space  */
    int hits;      /* matched pixels in that block       */
} ScanResult;

static ScanResult scan_frame(void)
{
    ScanResult best = {0, -1, -1, 0};
    int nbx = (g_dstW + g_blockPx - 1) / g_blockPx;
    int nby = (g_dstH + g_blockPx - 1) / g_blockPx;
    int bx, by;

    for (by = 0; by < nby; by++) {
        for (bx = 0; bx < nbx; bx++) {
            int x0 = bx * g_blockPx, y0 = by * g_blockPx;
            int x1 = x0 + g_blockPx, y1 = y0 + g_blockPx;
            int hits = 0, x, y;

            if (x1 > g_dstW) x1 = g_dstW;
            if (y1 > g_dstH) y1 = g_dstH;

            for (y = y0; y < y1; y++) {
                const uint32_t *row = g_frame + (size_t)y * g_dstW;
                for (x = x0; x < x1; x++) {
                    if (g_lut[rgb555(row[x])] != 0) hits++;
                }
            }

            /* Edge blocks are smaller, so compare against actual area. */
            if (hits * 100 >= g_threshold * ((x1 - x0) * (y1 - y0))) {
                if (!g_best) {
                    best.matched = 1; best.bx = bx; best.by = by;
                    best.hits = hits;
                    return best;              /* early exit */
                }
                if (hits > best.hits) {
                    best.matched = 1; best.bx = bx; best.by = by;
                    best.hits = hits;
                }
            }
        }
    }
    return best;
}

/* Maps a downsampled coordinate back to a virtual-desktop coordinate. */
static void dst_to_screen(int bx, int by, int *outX, int *outY)
{
    int cx = bx * g_blockPx + g_blockPx / 2;
    int cy = by * g_blockPx + g_blockPx / 2;
    *outX = g_regX + (int)((int64_t)cx * g_srcW / g_dstW);
    *outY = g_regY + (int)((int64_t)cy * g_srcH / g_dstH);
}

/* ------------------------------------------------------------------ */
/* Input emission                                                      */
/*                                                                     */
/* SendInput is the current input-injection API and supersedes          */
/* mouse_event. Its real advantage here is batching: a move plus a      */
/* click goes out as one atomic call, so nothing the user does can      */
/* interleave between them.                                             */
/*                                                                     */
/* Note this does NOT bypass UIPI. Injection into a window running at  */
/* a higher integrity level (an elevated app, or UAC prompts) is        */
/* blocked, so an elevated process is needed to drive one.             */
/* ------------------------------------------------------------------ */

static void emit(int x, int y)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in[3];
    UINT n = 0;

    if (vw <= 1) vw = 2;
    if (vh <= 1) vh = 2;

    memset(in, 0, sizeof(in));

    /* ABSOLUTE coordinates are normalized to 0..65535 across the virtual
     * desktop, so a multi-monitor layout needs the desktop origin offset
     * added back rather than using the primary monitor's bounds. */
    in[n].type       = INPUT_MOUSE;
    in[n].mi.dx      = MulDiv(x - vx, 65535, vw - 1);
    in[n].mi.dy      = MulDiv(y - vy, 65535, vh - 1);
    in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
                     | MOUSEEVENTF_VIRTUALDESK;
    n++;

    if (!g_moveOnly) {
        in[n] = in[n - 1];
        in[n].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        n++;
        in[n] = in[n - 1];
        in[n].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        n++;
    }

    SendInput(n, in, sizeof(INPUT));
}

/* ------------------------------------------------------------------ */
/* Hotkey window                                                       */
/*                                                                     */
/* RegisterHotKey delivers through the message queue, so the key works  */
/* regardless of which window has focus. GetAsyncKeyState would need a  */
/* polling loop and would keep a core busy.                             */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK hotkey_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_HOTKEY && w == HOTKEY_ID) {
        InterlockedExchange(&g_quit, 1);
    } else if (m == WM_DESTROY) {
        PostQuitMessage(0);
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND make_hotkey_window(void)
{
    WNDCLASSEX wc;
    HWND h;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = hotkey_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SCTrigHotkey";
    if (!RegisterClassExW(&wc)) die("RegisterClassEx");

    /* Hidden and off-screen; it exists only to own a message pump. */
    h = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                        -32000, -32000, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!h) die("CreateWindowEx");

    if (!RegisterHotKey(h, HOTKEY_ID, MOD_NOREPEAT, HOTKEY_VK)) {
        fprintf(stderr, "warning: could not register F8 hotkey (win32=%lu)\n",
                (unsigned long)GetLastError());
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

static void usage(const char *exe)
{
    printf(
"usage: %s [options]\n"
"\n"
"  --color RRGGBB      trigger color, repeatable (max %d, default FF0000)\n"
"  --tolerance N      per-channel match radius, 0-255 (default 40)\n"
"  --region x,y,w,h   capture region, virtual-desktop pixels\n"
"                      (default: 480x360 centered on the primary monitor)\n"
"  --size WxH         downsampled buffer size (default %dx%d)\n"
"  --block N          block edge in downsampled pixels (default 8)\n"
"  --threshold PCT    hit percent per block to trigger (default 8)\n"
"  --cooldown MS      minimum gap between emissions (default 400)\n"
"  --rearm N          clear frames required before re-arming (default 10)\n"
"  --fps N            target capture rate (default 60)\n"
"  --best             scan every block, trigger on the strongest\n"
"  --move-only        move the cursor, never click\n"
"  --quiet            suppress per-trigger output\n"
"  --help             this text\n"
"\n"
"Press F8 to stop.\n",
        exe, MAX_COLORS, DEFAULT_W, DEFAULT_H);
}

static int arg_int(const char *s)
{
    return (int)strtol(s, NULL, 0);
}

static int parse_args(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        }
        else if (!strcmp(a, "--color") && i + 1 < argc) {
            unsigned v;
            if (sscanf(argv[++i], "%x", &v) != 1) return -1;
            if (g_colorCount >= MAX_COLORS) return -1;
            g_colors[g_colorCount].r = (v >> 16) & 0xFF;
            g_colors[g_colorCount].g = (v >>  8) & 0xFF;
            g_colors[g_colorCount].b =  v        & 0xFF;
            g_colorCount++;
        }
        else if (!strcmp(a, "--tolerance") && i + 1 < argc) {
            int t = arg_int(argv[++i]);
            if (t < 0 || t > 255) return -1;
            g_tol  = t;
            g_tol2 = t * t;                 /* square once, not per pixel */
        }
        else if (!strcmp(a, "--region") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d,%d",
                       &g_regX, &g_regY, &g_regW, &g_regH) != 4) return -1;
        }
        else if (!strcmp(a, "--size") && i + 1 < argc) {
            int w, h;
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) return -1;
            if (w < 16 || h < 16) return -1;
            g_dstW = w; g_dstH = h;
        }
        else if (!strcmp(a, "--block") && i + 1 < argc) {
            int b = arg_int(argv[++i]);
            if (b < 1 || b > MAX_BLOCK) return -1;
            g_blockPx = b;
        }
        else if (!strcmp(a, "--threshold") && i + 1 < argc) {
            int p = arg_int(argv[++i]);
            if (p < 0 || p > 100) return -1;
            g_threshold = p;
        }
        else if (!strcmp(a, "--cooldown") && i + 1 < argc) {
            g_cooldownMs = arg_int(argv[++i]);
        }
        else if (!strcmp(a, "--rearm") && i + 1 < argc) {
            g_rearmFrames = arg_int(argv[++i]);
        }
        else if (!strcmp(a, "--fps") && i + 1 < argc) {
            g_fps = arg_int(argv[++i]);
        }
        else if (!strcmp(a, "--best"))      g_best = 1;
        else if (!strcmp(a, "--move-only")) g_moveOnly = 1;
        else if (!strcmp(a, "--quiet"))     g_quiet = 1;
        else return -1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    HWND hwnd;
    MSG msg;
    ScanResult sr;
    int armed = 1, clearFrames = 0;
    int64_t lastEmit = 0, nextFrame;
    int64_t frameBudget;
    int triggers = 0, frames = 0;
    int k, rc;
    int vx, vy, vw, vh;

    /* Region defaults: centered box on the primary monitor. */
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_regX = vx + (vw - 480) / 2;
    g_regY = vy + (vh - 360) / 2;
    g_regW = 480;
    g_regH = 360;

    if (g_colorCount == 0) {              /* default trigger color */
        g_colors[0].r = 255; g_colors[0].g = 0; g_colors[0].b = 0;
        g_colorCount = 1;
    }

    rc = parse_args(argc, argv);
    if (rc <= 0) { usage(argv[0]); return rc == 0 ? 0 : 2; }

    if (g_regW < 1 || g_regH < 1) { fprintf(stderr, "error: bad region\n"); return 2; }

    g_srcW = g_regW;
    g_srcH = g_regH;

    /* Without this, a DPI-scaled desktop reports virtual screen coords in
     * 96-dpi units while BitBlt works in physical pixels -- the two
     * disagree and the capture lands in the wrong place. */
    SetProcessDPIAware();

    build_lut();

    g_frame = (uint32_t *)malloc((size_t)g_dstW * g_dstH * sizeof(uint32_t));
    if (!g_frame) { fprintf(stderr, "error: out of memory\n"); return 1; }

    /* One top-down 32bpp DIB. Negative height means row 0 is the top,
     * matching the buffer's scan order and avoiding a flip. */
    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize       = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth      = g_srcW;
    g_bmi.bmiHeader.biHeight     = -g_srcH;
    g_bmi.bmiHeader.biPlanes     = 1;
    g_bmi.bmiHeader.biBitCount   = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    g_screenDC = GetDC(NULL);
    if (!g_screenDC) die("GetDC");
    g_memDC = CreateCompatibleDC(g_screenDC);
    if (!g_memDC) die("CreateCompatibleDC");

    g_dib = CreateDIBSection(g_screenDC, &g_bmi, DIB_RGB_COLORS,
                             &g_bits, NULL, 0);
    if (!g_dib || !g_bits) die("CreateDIBSection");
    SelectObject(g_memDC, g_dib);

    hwnd = make_hotkey_window();

    printf("region %d,%d %dx%d  buffer %dx%d  block %d  threshold %d%%\n",
           g_regX, g_regY, g_srcW, g_srcH, g_dstW, g_dstH,
           g_blockPx, g_threshold);
    printf("colors:");
    for (k = 0; k < g_colorCount; k++)
        printf(" %02X%02X%02X", g_colors[k].r, g_colors[k].g, g_colors[k].b);
    printf("  tol %d  mode %s\n", g_tol, g_moveOnly ? "move-only" : "click");
    printf("running -- press F8 to stop\n\n");

    frameBudget = (g_fps > 0) ? 1000 / g_fps : 0;
    if (frameBudget < 1) frameBudget = 1;
    nextFrame = now_ms();

    while (!g_quit) {

        /* Drain the queue so WM_HOTKEY is seen without blocking. */
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) InterlockedExchange(&g_quit, 1);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        capture_frame();
        frames++;

        sr = scan_frame();

        if (sr.matched) {
            clearFrames = 0;
            if (armed) {
                int64_t t = now_ms();
                if (t - lastEmit >= g_cooldownMs) {
                    int sx, sy;
                    dst_to_screen(sr.bx, sr.by, &sx, &sy);
                    emit(sx, sy);
                    lastEmit = t;
                    triggers++;
                    armed = 0;             /* one emission per trigger */
                    if (!g_quiet)
                        printf("[%6lld ms] trigger %d -> click %d,%d\n",
                               (long long)t, triggers, sx, sy);
                }
            }
        } else {
            /* Hysteresis: a trigger stays on screen for many frames. Without
             * this the tool re-fires on every frame of a single event. */
            if (++clearFrames >= g_rearmFrames) armed = 1;
        }

        /* Pace to the target rate instead of spinning at 100% CPU. */
        nextFrame += frameBudget;
        {
            int64_t sleep = nextFrame - now_ms();
            if (sleep > 0) Sleep((DWORD)sleep);
            else nextFrame = now_ms();
        }
    }

    printf("\n%d frames, %d triggers\n", frames, triggers);

    UnregisterHotKey(hwnd, HOTKEY_ID);
    SelectObject(g_memDC, GetStockObject(SYSTEM_BRUSH));
    DeleteObject(g_dib);
    DeleteDC(g_memDC);
    ReleaseDC(NULL, g_screenDC);
    free(g_frame);
    return 0;
}
