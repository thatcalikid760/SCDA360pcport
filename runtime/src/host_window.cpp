#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <gpu/d3d11_renderer.h>
#include <gpu/draw_state.h>
#include <gpu/shader_compiler.h>
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
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT frame{ 0, 0, 0, 0 };
    AdjustWindowRect(&frame, style, FALSE);
    auto [cw, ch] = WindowClientSize(uint32_t(work.right - work.left - (frame.right - frame.left)),
                                     uint32_t(work.bottom - work.top - (frame.bottom - frame.top)));
    RECT r{ 0, 0, LONG(cw), LONG(ch) };
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
        // SCDA_TRACE_FRAME=<n>: log every draw and resolve of frame n (renderer development).
        static const long long traceFrame = getenv("SCDA_TRACE_FRAME") ? atoll(getenv("SCDA_TRACE_FRAME")) : -1;
        if ((long long)frame.stats.frame == traceFrame)
        {
            for (auto& d : frame.draws)
                RuntimeLog("[frame] draw prim %u src %u idx32 %d count %u base 0x%08X vs %016llx ps %016llx rt 0x%08X/0x%08X mode %u\n",
                    d.primType, uint32_t(d.source), d.index32, d.indexCount, d.indexBase,
                    (unsigned long long)(d.vs ? d.vs->hash : 0), (unsigned long long)(d.ps ? d.ps->hash : 0),
                    (*d.regs)[kRegRbSurfaceInfo], (*d.regs)[kRegRbColorInfo], (*d.regs)[kRegRbModeControl] & 7);
            for (auto& d : frame.draws)
            {
                const RegisterFile& r = *d.regs;
                RuntimeLog("[frame]  vte 0x%08X vp %08X %08X %08X %08X %08X %08X winoff 0x%08X scis 0x%08X 0x%08X\n",
                    r[0x2206], r[0x210F], r[0x2110], r[0x2111], r[0x2112], r[0x2113], r[0x2114], r[0x2080], r[0x2081], r[0x2082]);
                for (uint32_t i = 0; i < 18; i += 6) // fetch dwords 0-17: texture slots 0-2 / vertex slots 0-8
                    RuntimeLog("[frame]  fetch %2u: %08X %08X %08X %08X %08X %08X\n", i, r[0x4800 + i], r[0x4801 + i], r[0x4802 + i],
                        r[0x4803 + i], r[0x4804 + i], r[0x4805 + i]);
                if (d.vs)
                {
                    std::bitset<96> slots = ShaderVertexFetchSlots(*d.vs);
                    for (uint32_t slot = 0; slot < 96; slot++)
                    {
                        MemoryRange m;
                        if (slots[slot] && VertexFetchRange(r, slot, m))
                            RuntimeLog("[frame]  vertex slot %u: 0x%08X + %u\n", slot, m.physical, m.bytes);
                    }
                    for (uint32_t s : d.memory)
                        RuntimeLog("[frame]  snapshot %u\n", s);
                }
                for (uint32_t c = 20; c < 30; c++) // VS constants of the UI shaders (glyph rects)
                {
                    auto f = [&](uint32_t i) { return std::bit_cast<float>(r[0x4000 + c * 4 + i]); };
                    RuntimeLog("[frame]  c%u: %g %g %g %g\n", c, f(0), f(1), f(2), f(3));
                }
            }
            for (auto& r : frame.resolves)
                RuntimeLog("[frame] resolve @draw %u ctrl 0x%08X dest 0x%08X pitch 0x%08X info 0x%08X surf 0x%08X color 0x%08X depth 0x%08X clear 0x%08X\n",
                    r.drawIndex, r.copyControl, r.destBase, r.destPitch, r.destInfo, r.surfaceInfo, r.colorInfo, r.depthInfo, r.colorClear);
            RuntimeLog("[frame] frontbuffer 0x%08X\n", frame.frontbuffer);
        }
        // SCDA_DUMP_FRAME=<n>: write frame n's render target to scda_frame_<n>.bmp.
        static const long long dumpFrame = getenv("SCDA_DUMP_FRAME") ? atoll(getenv("SCDA_DUMP_FRAME")) : -1;
        if ((long long)frame.stats.frame == dumpFrame)
            renderer.DumpNextFrame("scda_frame_" + std::to_string(dumpFrame) + ".bmp");
        renderer.Present(frame);
        if ((long long)frame.stats.frame == dumpFrame || (long long)frame.stats.frame == traceFrame)
            RuntimeLog("[frame] %llu: drawn %u skipped %u resolved %u presented %s\n", (unsigned long long)frame.stats.frame, renderer.LastDrawn(),
                renderer.LastSkipped(), renderer.LastResolved(), renderer.LastPresentedFrontbuffer() ? "frontbuffer" : "target");
        if (GetTickCount() - lastTitle > 500)
        {
            lastTitle = GetTickCount();
            wchar_t title[200];
            swprintf(title, 200, L"Splinter Cell: Double Agent Recomp - frame %llu draws %u (drawn %u skipped %u) resolves %u shaders %u compiled %u failed %u",
                (unsigned long long)frame.stats.frame, frame.stats.draws, renderer.LastDrawn(), renderer.LastSkipped(),
                frame.stats.resolves, frame.stats.shaders,
                ShaderCompilerReadyCount(), ShaderCompilerFailedCount());
            SetWindowTextW(hwnd, title);
        }
    }
}

std::pair<uint32_t, uint32_t> WindowClientSize(uint32_t workW, uint32_t workH)
{
    uint32_t w = 1280, h = 720;
    if (w > workW || h > workH)
    {
        w = std::min(workW, workH * 16 / 9) / 16 * 16;
        h = w * 9 / 16;
    }
    return { w, h };
}

void HostWindowStart(const DisplaySettings& s)
{
    std::thread(WindowThread, s).detach();
    std::thread(RenderThread, s).detach();
}
