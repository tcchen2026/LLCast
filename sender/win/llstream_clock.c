/* llstream_clock: an on-screen clock for glass-to-glass latency.
 *
 * It draws at (0, 0) of the main screen, inside the streamed top-left region.
 * Photograph the monitor and the projector in one shot: both show the same
 * picture, so the difference between the two readings is the streamed path's
 * extra latency (stream + projector lag - monitor lag).
 *
 * Each composed frame carries a seconds.milliseconds readout and two strips
 * in which a single block moves: 10 ms per cell (100 cells, one second), and
 * 100 ms per cell (10 cells). The strips stay readable in blurred photos.
 * Esc quits.
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dwmapi.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef BOOL(WINAPI *SetDpiContext)(HANDLE);

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    switch (message) {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(window);
        return 0;
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

/* Physical pixels, so display scaling cannot move the clock out of the
 * captured 1920x1080 region. */
static void physical_pixels(void)
{
    SetDpiContext set = (SetDpiContext)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                              "SetProcessDpiAwarenessContext");
    if (!set || !set((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

/* Cells with one lit block. Every `group` cells gets a labelled tick above
 * the strip, so a photo can be read without counting cells. */
static void strip(HDC dc, RECT area, int cells, int group, int lit, HBRUSH on, HBRUSH off)
{
    int width = area.right - area.left, tick = (area.bottom - area.top) / 3;
    for (int i = 0; i < cells; ++i) {
        RECT cell = {area.left + width * i / cells + 1, area.top,
                     area.left + width * (i + 1) / cells - 1, area.bottom};
        FillRect(dc, &cell, i == lit ? on : off);
        if (i % group) continue;
        RECT mark = {cell.left - 1, area.top - tick, cell.left + 2, area.top};
        FillRect(dc, &mark, on);
        wchar_t number[8];
        swprintf(number, 8, L"%d", i);
        RECT where = {cell.left + 6, area.top - tick * 3, cell.left + width, area.top - 2};
        DrawTextW(dc, number, -1, &where, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    }
}

int main(int argc, char **argv)
{
    int width = 1280, height = 720;
    double seconds = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc &&
            sscanf(argv[++i], "%dx%d", &width, &height) == 2 &&
            width >= 320 && height >= 180 && width <= 1920 && height <= 1080) continue;
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc && (seconds = atof(argv[++i])) > 0) continue;
        fprintf(stderr, "Usage: %s [--size 1280x720] [--seconds N]\n"
                "Shows a millisecond clock at the top-left of the main screen. Esc quits.\n", argv[0]);
        return 2;
    }
    physical_pixels();
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    DWM_TIMING_INFO timing = {.cbSize = sizeof(timing)};
    double refresh = SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &timing)) && timing.rateRefresh.uiDenominator
        ? (double)timing.rateRefresh.uiNumerator / timing.rateRefresh.uiDenominator : 0;
    printf("llstream_clock: %dx%d at (0,0), QPC %lld Hz, composition %.3f Hz\n",
           width, height, (long long)frequency.QuadPart, refresh);
    fflush(stdout);

    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {.lpfnWndProc = window_proc, .hInstance = instance,
                              .lpszClassName = L"LLStreamClock"};
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"LLStream clock",
                                  WS_POPUP | WS_VISIBLE, 0, 0, width, height, NULL, NULL, instance, NULL);
    if (!window) { fprintf(stderr, "CreateWindow failed: %lu\n", GetLastError()); return 1; }
    SetForegroundWindow(window);

    HDC screen = GetDC(window), canvas = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    SelectObject(canvas, bitmap);
    HFONT digits = CreateFontW(-height * 3 / 10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                               FIXED_PITCH, L"Consolas");
    HFONT label = CreateFontW(-height / 20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                              FIXED_PITCH, L"Consolas");
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)), white = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH grey = CreateSolidBrush(RGB(48, 48, 48));
    SetBkMode(canvas, TRANSPARENT);
    SetTextColor(canvas, RGB(255, 255, 255));

    QueryPerformanceCounter(&start);
    unsigned long long frame = 0;
    MSG message;
    for (;;) {
        bool quit = false;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) quit = true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (quit) break;
        QueryPerformanceCounter(&now);
        long long ms = (now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;
        if (seconds > 0 && ms >= seconds * 1000) break;

        RECT all = {0, 0, width, height};
        FillRect(canvas, &all, black);
        wchar_t text[64];
        swprintf(text, 64, L"%lld.%03lld", ms / 1000, ms % 1000);
        SelectObject(canvas, digits);
        RECT readout = {0, height / 12, width, height * 1 / 2};
        DrawTextW(canvas, text, -1, &readout, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(canvas, label);
        swprintf(text, 64, L"LLStream clock   frame %llu   %.1f Hz", frame, refresh);
        RECT caption = {width / 40, height / 40, width, height / 10};
        DrawTextW(canvas, text, -1, &caption, DT_LEFT | DT_TOP | DT_SINGLELINE);
        RECT fine = {width / 40, height * 63 / 100, width - width / 40, height * 63 / 100 + height / 10};
        RECT coarse = {width / 40, height * 83 / 100, width - width / 40, height * 83 / 100 + height / 10};
        strip(canvas, fine, 100, 10, (int)(ms / 10 % 100), white, grey);    /* 10 ms per cell */
        strip(canvas, coarse, 10, 1, (int)(ms / 100 % 10), white, grey);    /* 100 ms per cell */

        BitBlt(screen, 0, 0, width, height, canvas, 0, 0, SRCCOPY);
        GdiFlush();
        /* One update per composition: wait for the compositor's next frame. */
        if (FAILED(DwmFlush())) Sleep(1);
        ++frame;
    }
    printf("llstream_clock: %llu frames\n", frame);
    DeleteObject(digits);
    DeleteObject(label);
    DeleteObject(black);
    DeleteObject(white);
    DeleteObject(grey);
    DeleteObject(bitmap);
    DeleteDC(canvas);
    ReleaseDC(window, screen);
    return 0;
}
