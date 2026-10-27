#include <windows.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <gpu/d3d11_renderer.h>
#include <host_window.h>
#include <runtime.h>

static std::atomic<HWND> g_hwnd;
static std::atomic<uint32_t> g_resizeW, g_resizeH;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        g_resizeH = HIWORD(lp);
        g_resizeW = LOWORD(lp);
        return 0;
    case WM_CLOSE:
        // Guest threads have no shutdown path: end the process.
        RuntimeLog("[window] closed by user\n");
        ExitProcess(0);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void WindowThread(DisplaySettings s)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"SCDARecompWindow";
    RegisterClassExW(&wc);

    DWORD style = s.mode == DisplaySettings::Windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    RECT r{ 0, 0, LONG(1280 * s.scale), LONG(720 * s.scale) };
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (s.mode != DisplaySettings::Windowed)
    {
        r = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        x = y = 0;
    }
    else
        AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Splinter Cell: Double Agent Recomp", style, x, y,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    g_hwnd = hwnd;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0))
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void RenderThread(DisplaySettings s)
{
    while (!g_hwnd.load())
        Sleep(1);
    HWND hwnd = g_hwnd.load();
    RECT rc;
    GetClientRect(hwnd, &rc);
    D3D11Renderer renderer;
    if (!renderer.Init(hwnd, rc.right, rc.bottom, s.vsync))
    {
        RuntimeLog("[window] no Direct3D 11 device\n");
        return;
    }
    Frame frame;
    DWORD lastTitle = 0;
    for (;;)
    {
        if (uint32_t w = g_resizeW.exchange(0))
            renderer.Resize(w, g_resizeH.load());
        if (!Frames().PopFrame(frame))
        {
            Sleep(1);
            continue;
        }
        renderer.Present(frame);
        if (GetTickCount() - lastTitle > 500)
        {
            lastTitle = GetTickCount();
            wchar_t title[160];
            swprintf(title, 160, L"Splinter Cell: Double Agent Recomp - frame %llu draws %u resolves %u shaders %u",
                (unsigned long long)frame.stats.frame, frame.stats.draws, frame.stats.resolves, frame.stats.shaders);
            SetWindowTextW(hwnd, title);
        }
    }
}

void HostWindowStart(const DisplaySettings& s)
{
    std::thread(WindowThread, s).detach();
    std::thread(RenderThread, s).detach();
}
