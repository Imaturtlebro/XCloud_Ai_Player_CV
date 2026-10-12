/*
 * trigger.c -- screen-colour trigger tool (Win32 API only)
 *
 * Pipeline:
 *   1. Capture a small circular zone at screen centre with GDI BitBlt.
 *   2. Classify every captured pixel in O(1) through a precomputed RGB555
 *      colour lookup table.
 *   3. If the matched-pixel ratio inside the circle crosses a threshold,
 *      emit a mouse move + click via SendInput.
 *
 * F8 stops the loop. F9 re-arms it manually.
 *
 * No down-sampling: the zone is a ~30px radius circle, so the capture is
 * only ~61x61 pixels and can be scanned 1:1. Sampling every pixel at full
 * resolution is both cheaper and more accurate than reducing a large
 * capture down.
 *
 * Build:  gcc -O2 -Wall -Wextra -o trigger.exe trigger.c -lgdi32 -luser32
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_COLORS   8
#define LUT_SIZE     32768      /* 32 values per channel = 2^15 */
#define MAX_RADIUS   2000

#define HOTKEY_STOP   1
#define HOTKEY_REARM  2

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct { int r, g, b; } RGB;

static RGB     g_colors[MAX_COLORS];
static int     g_colorCount = 0;
static int     g_tol  = 40;    /* per-channel radius; wide by default    */
static int     g_tol2 = 0;     /* squared, so the hot loop never multiplies */
static uint8_t g_lut[LUT_SIZE];

static int  g_zoneCx = -1;     /* -1 => derive from screen centre        */
static int  g_zoneCy = -1;
static int  g_zoneR  = 30;
static int  g_box    = 61;     /* 2*radius+1, the capture side           */

static int  g_threshold = 5;   /* percent of circle area that must match */
static int  g_minHits   = 3;   /* absolute floor, guards tiny circles    */
static int  g_rearmMs   = 2500;/* target must be absent this long        */
static int  g_cooldownMs= 200;
static int  g_fps       = 60;
static int  g_moveOnly  = 0;
static int  g_quiet     = 0;

static uint32_t *g_buf  = NULL;   /* g_box * g_box captured pixels        */

static HDC        g_screenDC = NULL;
static HDC        g_memDC    = NULL;
static HBITMAP    g_dib      = NULL;
static void      *g_bits     = NULL;
static HGDIOBJ    g_oldObj   = NULL;
static BITMAPINFO g_bmi;

static volatile LONG g_quit = 0;
static volatile LONG g_manualRearm = 0;

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
/* Colour LUT                                                          */
/*                                                                     */
/* Per-pixel classification normally means comparing every pixel       */
/* against every target colour. Precomputing all 32768 possible RGB555 */
/* inputs collapses that into a single array index at runtime.         */
/*                                                                     */
/* Note this is a squared-euclidean radius in RGB space, not a         */
/* per-channel box. Both are in common use; euclidean gives a rounder  */
/* match region and is less likely to catch colours that are near in   */
/* one channel but far in another.                                     */
/* ------------------------------------------------------------------ */

static int dist2(int r, int g, int b, const RGB *c)
{
    int dr = r - c->r, dg = g - c->g, db = b - c->b;
    return dr * dr + dg * dg + db * db;
}

static void build_lut(void)
{
    int i, k;
    for (i = 0; i < LUT_SIZE; i++) {
        int r = ((i >> 10) & 0x1F) << 3;
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

/* 32bpp BI_RGB pixels are 0x00RRGGBB. */
static inline int rgb555(uint32_t px)
{
    return (int)(((px >> 19) & 0x1F) << 10)
         | (int)(((px >> 11) & 0x1F) <<  5)
         | (int)(( px >>  3) & 0x1F);
}

/* ------------------------------------------------------------------ */
/* Capture + circular scan                                             */
/*                                                                     */
/* The capture is exactly the bounding box of the circle, so cost is   */
/* O(radius^2) rather than O(screen area). At r=30 that is 3721 pixels */
/* per frame -- cheaper than one full-screen capture, let alone 60 of  */
/* them per second.                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int matched;
    int hits;
    int sx, sy;      /* centroid in screen coords */
} ScanResult;

static void capture_zone(void)
{
    int x0 = g_zoneCx - g_zoneR;
    int y0 = g_zoneCy - g_zoneR;

    /* Dest is the memory DC: we are reading the screen into our buffer,
     * so the region offset belongs to the source coords. */
    BitBlt(g_memDC, 0, 0, g_box, g_box, g_screenDC, x0, y0, SRCCOPY);

    memcpy(g_buf, g_bits, (size_t)g_box * g_box * sizeof(uint32_t));
}

static ScanResult scan_zone(void)
{
    ScanResult res = {0, 0, 0, 0};
    int r2 = g_zoneR * g_zoneR;
    int hits = 0, sumx = 0, sumy = 0, area = 0;
    int x, y;

    for (y = 0; y < g_box; y++) {
        const uint32_t *row = g_buf + (size_t)y * g_box;
        int dy = y - g_zoneR;
        for (x = 0; x < g_box; x++) {
            int dx = x - g_zoneR;
            if (dx * dx + dy * dy > r2) continue;   /* outside the circle */
            area++;
            if (g_lut[rgb555(row[x])] != 0) {
                hits++;
                sumx += x;
                sumy += y;
            }
        }
    }

    res.hits = hits;
    if (hits >= g_minHits && area > 0 && hits * 100 >= g_threshold * area) {
        res.matched = 1;
        /* Click the centroid of the matched pixels, not the geometric
         * centre of the circle -- that is where the target actually is. */
        res.sx = g_zoneCx - g_zoneR + (int)((int64_t)sumx / hits);
        res.sy = g_zoneCy - g_zoneR + (int)((int64_t)sumy / hits);
    }
    return res;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
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

    /* ABSOLUTE coords are normalised to 0..65535 across the virtual
     * desktop, so the desktop origin offset must be subtracted. */
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
/* Hotkeys                                                             */
/*                                                                     */
/* RegisterHotKey routes through the message queue, so the keys work   */
/* regardless of which window has focus -- necessary here because the  */
/* foreground window will be the game, not us.                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK hotkey_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_HOTKEY) {
        if (w == HOTKEY_STOP)   InterlockedExchange(&g_quit, 1);
        if (w == HOTKEY_REARM) InterlockedExchange(&g_manualRearm, 1);
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND make_hotkey_window(void)
{
    WNDCLASSEXW wc;
    HWND h;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = hotkey_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"TriggerHotkey";
    if (!RegisterClassExW(&wc)) die("RegisterClassEx");

    h = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                        -32000, -32000, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!h) die("CreateWindowEx");

    if (!RegisterHotKey(h, HOTKEY_STOP, MOD_NOREPEAT, VK_F8))
        fprintf(stderr, "warning: F8 not registered (win32=%lu)\n",
                (unsigned long)GetLastError());
    if (!RegisterHotKey(h, HOTKEY_REARM, MOD_NOREPEAT, VK_F9))
        fprintf(stderr, "warning: F9 not registered (win32=%lu)\n",
                (unsigned long)GetLastError());

    return h;
}

/* ------------------------------------------------------------------ */
/* Config file                                                         */
/*                                                                     */
/* key = value, '#' or ';' comments. Loaded before the command line so */
/* that command-line arguments always win.                             */
/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                     e[-1] == ' '  || e[-1] == '\t')) *--e = 0;
    return s;
}

static int add_color(const char *v)
{
    unsigned c;
    if (g_colorCount >= MAX_COLORS) return -1;
    if (sscanf(v, "%x", &c) != 1) return -1;
    g_colors[g_colorCount].r = (c >> 16) & 0xFF;
    g_colors[g_colorCount].g = (c >>  8) & 0xFF;
    g_colors[g_colorCount].b =  c        & 0xFF;
    g_colorCount++;
    return 0;
}

static void apply_tolerance(void) { g_tol2 = g_tol * g_tol; }

static int load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];

    if (!f) return -1;

    while (fgets(line, sizeof line, f)) {
        char *p = trim(line), *eq, *k, *v;
        if (*p == '#' || *p == ';' || *p == 0) continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        k = trim(p);
        v = trim(eq + 1);

        if      (!strcmp(k, "color"))        add_color(v);
        else if (!strcmp(k, "tolerance"))   { g_tol  = atoi(v); apply_tolerance(); }
        else if (!strcmp(k, "zone_radius"))  g_zoneR = atoi(v);
        else if (!strcmp(k, "zone_cx"))      g_zoneCx = atoi(v);
        else if (!strcmp(k, "zone_cy"))      g_zoneCy = atoi(v);
        else if (!strcmp(k, "threshold"))    g_threshold   = atoi(v);
        else if (!strcmp(k, "min_hits"))     g_minHits     = atoi(v);
        else if (!strcmp(k, "rearm_ms"))     g_rearmMs     = atoi(v);
        else if (!strcmp(k, "cooldown_ms"))  g_cooldownMs  = atoi(v);
        else if (!strcmp(k, "fps"))          g_fps         = atoi(v);
        else if (!strcmp(k, "move_only"))    g_moveOnly    = atoi(v);
        else if (!strcmp(k, "quiet"))        g_quiet       = atoi(v);
        else fprintf(stderr, "warning: unknown config key '%s'\n", k);
    }
    fclose(f);
    return 0;
}

static void usage(const char *exe)
{
    printf(
"usage: %s [options]\n"
"\n"
"  --config FILE      read settings from FILE (default: trigger.cfg if present)\n"
"  --color RRGGBB     target colour, repeatable, max %d\n"
"  --tolerance N      RGB match radius, 0-255 (default 40, wide for video)\n"
"  --radius N         detection circle radius in px at screen centre (default 30)\n"
"  --center X,Y       circle centre, default screen centre\n"
"  --threshold PCT    percent of circle area that must match (default 5)\n"
"  --min-hits N       absolute minimum matching pixels (default 3)\n"
"  --rearm-ms N       target must be absent this long before re-arming (default 2500)\n"
"  --cooldown-ms N    minimum gap between emissions (default 200)\n"
"  --fps N            capture rate (default 60)\n"
"  --move-only        move the cursor, never click\n"
"  --quiet            suppress per-trigger output\n"
"  --save-config FILE write the effective settings out and exit\n"
"  --help             this text\n"
"\n"
"F8 stop.  F9 force re-arm.\n"
"\n"
"Use picker.exe to sample target colours off the screen.\n",
        exe, MAX_COLORS);
}

static int parse_args(int argc, char **argv, const char **saveTo)
{
    const char *cfg = "trigger.cfg";
    int i;

    /* Config first, so the command line overrides it. */
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg = argv[++i];
    load_config(cfg);                       /* absence is not an error */

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--config") && i + 1 < argc) ++i;
        else if (!strcmp(a, "--color") && i + 1 < argc) {
            if (add_color(argv[++i]) != 0) return -1;
        }
        else if (!strcmp(a, "--tolerance") && i + 1 < argc) {
            g_tol = atoi(argv[++i]); apply_tolerance();
        }
        else if (!strcmp(a, "--radius") && i + 1 < argc)       g_zoneR      = atoi(argv[++i]);
        else if (!strcmp(a, "--center") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d", &g_zoneCx, &g_zoneCy) != 2) return -1;
        }
        else if (!strcmp(a, "--threshold") && i + 1 < argc)    g_threshold   = atoi(argv[++i]);
        else if (!strcmp(a, "--min-hits") && i + 1 < argc)     g_minHits     = atoi(argv[++i]);
        else if (!strcmp(a, "--rearm-ms") && i + 1 < argc)     g_rearmMs     = atoi(argv[++i]);
        else if (!strcmp(a, "--cooldown-ms") && i + 1 < argc)  g_cooldownMs  = atoi(argv[++i]);
        else if (!strcmp(a, "--fps") && i + 1 < argc)          g_fps         = atoi(argv[++i]);
        else if (!strcmp(a, "--move-only")) g_moveOnly = 1;
        else if (!strcmp(a, "--quiet"))     g_quiet    = 1;
        else if (!strcmp(a, "--save-config") && i + 1 < argc) *saveTo = argv[++i];
        else return -1;
    }
    return 1;
}

static int save_config(const char *path)
{
    FILE *f = fopen(path, "w");
    int k;
    if (!f) return -1;
    fprintf(f, "# written by trigger --save-config\n");
    for (k = 0; k < g_colorCount; k++)
        fprintf(f, "color = %02X%02X%02X\n",
                g_colors[k].r, g_colors[k].g, g_colors[k].b);
    fprintf(f, "tolerance   = %d\n", g_tol);
    fprintf(f, "zone_radius = %d\n", g_zoneR);
    fprintf(f, "zone_cx     = %d\n", g_zoneCx);
    fprintf(f, "zone_cy     = %d\n", g_zoneCy);
    fprintf(f, "threshold   = %d\n", g_threshold);
    fprintf(f, "min_hits    = %d\n", g_minHits);
    fprintf(f, "rearm_ms    = %d\n", g_rearmMs);
    fprintf(f, "cooldown_ms = %d\n", g_cooldownMs);
    fprintf(f, "fps         = %d\n", g_fps);
    fprintf(f, "move_only   = %d\n", g_moveOnly);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    HWND hwnd;
    MSG msg;
    ScanResult sr;
    const char *saveTo = NULL;
    int armed = 1;
    int64_t absentSince = 0;      /* when the target last went absent */
    int64_t lastEmit = 0, nextFrame, budget;
    int triggers = 0, frames = 0;
    int k, rc;
    int vx, vy, vw, vh;

    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    rc = parse_args(argc, argv, &saveTo);
    if (rc < 0) { usage(argv[0]); return 2; }   /* bad args */
    if (rc == 0) return 0;                     /* --help already printed */

    if (g_colorCount == 0) {                 /* default target colour */
        g_colors[0].r = 255; g_colors[0].g = 0; g_colors[0].b = 0;
        g_colorCount = 1;
    }

    /* Resolve the zone against the real desktop bounds. */
    if (g_zoneCx < 0) g_zoneCx = vx + vw / 2;
    if (g_zoneCy < 0) g_zoneCy = vy + vh / 2;

    if (g_zoneR < 1 || g_zoneR > MAX_RADIUS) {
        fprintf(stderr, "error: radius must be 1..%d\n", MAX_RADIUS);
        return 2;
    }
    if (g_threshold < 0 || g_threshold > 100) {
        fprintf(stderr, "error: threshold must be 0..100\n");
        return 2;
    }
    g_box = g_zoneR * 2 + 1;

    if (saveTo) {
        if (save_config(saveTo) == 0) printf("wrote %s\n", saveTo);
        else { fprintf(stderr, "error: cannot write %s\n", saveTo); return 1; }
        return 0;
    }

    SetProcessDPIAware();   /* otherwise region coords are in 96-dpi units */

    build_lut();

    g_buf = (uint32_t *)malloc((size_t)g_box * g_box * sizeof(uint32_t));
    if (!g_buf) { fprintf(stderr, "error: out of memory\n"); return 1; }

    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize       = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth      = g_box;
    g_bmi.bmiHeader.biHeight     = -g_box;  /* top-down: row 0 is the top */
    g_bmi.bmiHeader.biPlanes     = 1;
    g_bmi.bmiHeader.biBitCount   = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    g_screenDC = GetDC(NULL);
    if (!g_screenDC) die("GetDC");
    g_memDC = CreateCompatibleDC(g_screenDC);
    if (!g_memDC) die("CreateCompatibleDC");

    g_dib = CreateDIBSection(g_screenDC, &g_bmi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    if (!g_dib || !g_bits) die("CreateDIBSection");

    /* Keep the stock bitmap so it can be restored before DeleteObject;
     * a memory DC cannot own a DIB section it did not select. */
    g_oldObj = SelectObject(g_memDC, g_dib);
    if (!g_oldObj || g_oldObj == HGDI_ERROR) die("SelectObject");

    hwnd = make_hotkey_window();

    printf("zone    centre %d,%d  radius %d  capture %dx%d (%.1f KB)\n",
           g_zoneCx, g_zoneCy, g_zoneR, g_box, g_box,
           g_box * (double)g_box * 4 / 1024.0);
    printf("target  ");
    for (k = 0; k < g_colorCount; k++)
        printf(" %02X%02X%02X", g_colors[k].r, g_colors[k].g, g_colors[k].b);
    printf("  tolerance %d  threshold %d%%  min_hits %d\n",
           g_tol, g_threshold, g_minHits);
    printf("policy  rearm %d ms  cooldown %d ms  fps %d  %s\n\n",
           g_rearmMs, g_cooldownMs, g_fps, g_moveOnly ? "move-only" : "click");
    printf("F8 stop   F9 re-arm\n\n");

    budget = 1000 / (g_fps > 0 ? g_fps : 60);
    if (budget < 1) budget = 1;
    nextFrame = now_ms();

    while (!g_quit) {

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) InterlockedExchange(&g_quit, 1);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        if (InterlockedExchange(&g_manualRearm, 0)) {
            armed = 1;
            absentSince = 0;
            if (!g_quiet) printf("[manual] re-armed\n");
        }

        capture_zone();
        frames++;
        sr = scan_zone();

        if (sr.matched) {
            absentSince = 0;                 /* target present again */
            if (armed) {
                int64_t t = now_ms();
                if (t - lastEmit >= g_cooldownMs) {
                    emit(sr.sx, sr.sy);
                    lastEmit = t;
                    triggers++;
                    armed = 0;
                    if (!g_quiet)
                        printf("[%7lld ms] fire %d -> %d,%d  (%d px)\n",
                               (long long)t, triggers, sr.sx, sr.sy, sr.hits);
                }
            }
        } else {
            /* Strict hysteresis. The target must be continuously absent
             * for the full rearm window before we fire again, so a target
             * lingering anywhere in the circle produces exactly one shot. */
            int64_t t = now_ms();
            if (absentSince == 0) absentSince = t;
            else if (t - absentSince >= g_rearmMs && !armed) {
                armed = 1;
                if (!g_quiet)
                    printf("[%7lld ms] re-armed after %d ms clear\n",
                           (long long)t, g_rearmMs);
            }
        }

        nextFrame += budget;
        {
            int64_t s = nextFrame - now_ms();
            if (s > 0) Sleep((DWORD)s);
            else nextFrame = now_ms();
        }
    }

    printf("\n%d frames, %d triggers\n", frames, triggers);

    UnregisterHotKey(hwnd, HOTKEY_STOP);
    UnregisterHotKey(hwnd, HOTKEY_REARM);
    SelectObject(g_memDC, g_oldObj);   /* restore before deleting the DIB */
    DeleteObject(g_dib);
    DeleteDC(g_memDC);
    ReleaseDC(NULL, g_screenDC);
    free(g_buf);
    return 0;
}
