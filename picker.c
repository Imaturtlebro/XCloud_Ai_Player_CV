/*
 * picker.c -- magnifier colour sampler (Win32 API only)
 *
 * Shows a live magnified view of the pixels under the cursor, so you can
 * read the exact RGB of something on screen instead of guessing.
 *
 *   move mouse   -- magnifier follows the cursor
 *   left click   -- record the colour under the cursor
 *   C            -- copy the recorded list to the clipboard
 *   S            -- write the recorded list to picked-colors.txt
 *   Esc          -- quit
 *
 * The magnifier is drawn from a real screen capture, so what you see is
 * the composited desktop -- exactly what a capture-based detector would
 * see, including video compression artefacts.
 *
 * Build: gcc -O2 -Wall -Wextra -o picker.exe picker.c -lgdi32 -luser32
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* IDC_CROSS is the ANSI resource macro; LoadCursorW needs the wide form. */
#define IDC_CROSSW  MAKEINTRESOURCEW(32512)

#define MAG        12          /* magnify factor: 1 -> 12x */
#define WIN_SIDE   (16 * MAG)  /* window is a 16x16 pixel patch, magnified */
#define GRID       1           /* grid line width, in pixels */

static HWND     g_hwnd;
static HDC      g_screenDC;
static HDC      g_memDC;
static HBITMAP  g_dib;
static void    *g_bits;
static HGDIOBJ  g_oldObj;
static BITMAPINFO g_bmi;

static int  g_zoom  = MAG;
static int  g_side  = WIN_SIDE / MAG;      /* source pixels across */
static int  g_curX, g_curY;                /* cursor in screen coords */

static int  g_picked[16];
static int  g_pickCount;

static void add_pick(int rgb)
{
    if (g_pickCount < (int)(sizeof(g_picked) / sizeof(g_picked[0])))
        g_picked[g_pickCount++] = rgb;
}

static void copy_to_clipboard(HWND h)
{
    char buf[512];
    int i, n = 0;

    buf[0] = 0;
    for (i = 0; i < g_pickCount; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%06X",
                      i ? " " : "", g_picked[i]);

    {
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, strlen(buf) + 1);
        if (!hg) return;
        memcpy(GlobalLock(hg), buf, strlen(buf) + 1);
        GlobalUnlock(hg);
        OpenClipboard(h);
        EmptyClipboard();
        SetClipboardData(CF_TEXT, hg);
        CloseClipboard();
    }
}

static void write_list(void)
{
    FILE *f = fopen("picked-colors.txt", "w");
    int i;
    if (!f) {
        MessageBoxA(NULL, "cannot write picked-colors.txt", "picker", MB_OK);
        return;
    }
    fprintf(f, "# sampled with picker.exe\n");
    for (i = 0; i < g_pickCount; i++)
        fprintf(f, "color = %06X\n", g_picked[i]);
    fclose(f);
    MessageBoxA(NULL, "wrote picked-colors.txt", "picker", MB_OK);
}

/* Reads the colour under the cursor directly from the captured patch, so
 * the readout and the magnified pixels can never disagree. */
static int color_under_cursor(void)
{
    int cx = g_side / 2, cy = g_side / 2;
    uint32_t px;
    memcpy(&px, (const uint8_t *)g_bits + ((size_t)cy * g_side + cx) * 4, 4);
    return (int)(px & 0x00FFFFFF);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_TIMER: {
        POINT pt;
        RECT rc;
        int rgb;
        char title[160];

        GetCursorPos(&pt);
        g_curX = pt.x;
        g_curY = pt.y;

        /* Capture a patch centred on the cursor. */
        BitBlt(g_memDC, 0, 0, g_side, g_side,
               g_screenDC, pt.x - g_side / 2, pt.y - g_side / 2, SRCCOPY);

        rgb = color_under_cursor();

        GetClientRect(h, &rc);
        {
            HDC dc = GetDC(h);
            HPEN grid = CreatePen(PS_SOLID, GRID, RGB(255, 255, 255));
            int px, py;

            /* StretchBlt with COLORONCOLOR is nearest-neighbour, which is
             * what we want here: we need to see individual pixel values,
             * not a smoothed blend that would lie about the colour. */
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, 0, 0, rc.right, rc.bottom,
                       g_memDC, 0, 0, g_side, g_side, SRCCOPY);

            SelectObject(dc, grid);
            for (px = g_zoom; px < rc.right; px += g_zoom) {
                MoveToEx(dc, px, 0, NULL); LineTo(dc, px, rc.bottom);
            }
            for (py = g_zoom; py < rc.bottom; py += g_zoom) {
                MoveToEx(dc, 0, py, NULL); LineTo(dc, rc.right, py);
            }

            /* Mark the centre pixel -- that is the one being sampled. */
            {
                RECT c;
                HPEN mark = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
                SelectObject(dc, mark);
                c.left   = rc.right  / 2 - g_zoom;
                c.top    = rc.bottom / 2 - g_zoom;
                c.right  = rc.right  / 2 + g_zoom;
                c.bottom = rc.bottom / 2 + g_zoom;
                Rectangle(dc, c.left, c.top, c.right, c.bottom);
                DeleteObject(mark);
            }

            DeleteObject(grid);
            ReleaseDC(h, dc);
        }

        snprintf(title, sizeof(title),
                 "%02X%02X%02X  (%d,%d)   picked:%d   C=copy S=save Esc=quit",
                 (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                 g_curX, g_curY, g_pickCount);
        SetWindowTextA(h, title);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt;
        GetCursorPos(&pt);
        BitBlt(g_memDC, 0, 0, g_side, g_side,
               g_screenDC, pt.x - g_side / 2, pt.y - g_side / 2, SRCCOPY);
        add_pick(color_under_cursor());
        return 0;
    }

    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(h);
        else if (w == 'C')  copy_to_clipboard(h);
        else if (w == 'S')  write_list();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(void)
{
    WNDCLASSEXW wc;
    MSG msg;

    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize       = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth      = g_side;
    g_bmi.bmiHeader.biHeight     = -g_side;
    g_bmi.bmiHeader.biPlanes     = 1;
    g_bmi.bmiHeader.biBitCount   = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    g_screenDC = GetDC(NULL);
    if (!g_screenDC) { fprintf(stderr, "GetDC failed\n"); return 1; }
    g_memDC = CreateCompatibleDC(g_screenDC);
    if (!g_memDC)  { fprintf(stderr, "CreateCompatibleDC failed\n"); return 1; }
    g_dib = CreateDIBSection(g_screenDC, &g_bmi, DIB_RGB_COLORS,
                             &g_bits, NULL, 0);
    if (!g_dib || !g_bits) { fprintf(stderr, "CreateDIBSection failed\n"); return 1; }
    g_oldObj = SelectObject(g_memDC, g_dib);
    if (!g_oldObj || g_oldObj == HGDI_ERROR) {
        fprintf(stderr, "SelectObject failed\n");
        return 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_CROSSW);
    wc.lpszClassName = L"ColourPicker";
    if (!RegisterClassExW(&wc)) { fprintf(stderr, "RegisterClassEx failed\n"); return 1; }

    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
                             L"picker", WS_POPUPWINDOW,
                             100, 100, WIN_SIDE, WIN_SIDE,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) { fprintf(stderr, "CreateWindowEx failed\n"); return 1; }

    ShowWindow(g_hwnd, SW_SHOW);
    SetTimer(g_hwnd, 1, 33, NULL);   /* ~30fps is plenty for a magnifier */

    printf("picker: left click samples, C copies, S saves, Esc quits\n");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(g_hwnd, 1);
    SelectObject(g_memDC, g_oldObj);
    DeleteObject(g_dib);
    DeleteDC(g_memDC);
    ReleaseDC(NULL, g_screenDC);
    return 0;
}
