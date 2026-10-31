#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <gpu/d3d11_renderer.h>
#include <gpu_state.h>
#include <gpu/draw_state.h>
#include <gpu/shader_compiler.h>
#include <host_window.h>
#include <input.h>
#include <runtime.h>

static std::atomic<HWND> g_hwnd;
static std::atomic<uint32_t> g_resizeW, g_resizeH;
static bool g_mouseLook;   // capture the cursor and feed raw motion to the right stick
static bool g_cursorHeld;  // cursor hidden and clipped to the client area now

// Mouse look while the window is in the foreground: the cursor is hidden and kept inside the
// client area; Alt+Tab (focus loss) releases it.
static void HoldCursor(HWND hwnd, bool hold)
{
    if (hold)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);
    }
    else
        ClipCursor(nullptr);
    if (hold != g_cursorHeld)
        ShowCursor(hold ? FALSE : TRUE);
    g_cursorHeld = hold;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        g_resizeH = HIWORD(lp);
        g_resizeW = LOWORD(lp);
        if (g_cursorHeld)
            HoldCursor(hwnd, true);
        return 0;
    case WM_MOVE:
        if (g_cursorHeld)
            HoldCursor(hwnd, true);
        return 0;
    case WM_ACTIVATE:
        if (g_mouseLook)
            HoldCursor(hwnd, LOWORD(wp) != WA_INACTIVE);
        break; // DefWindowProc gives the window keyboard focus
    case WM_INPUT:
    {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            MouseAccumulate(raw.data.mouse.lLastX, raw.data.mouse.lLastY, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_MOUSEWHEEL:
        MouseAccumulate(0, 0, GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
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
    // Scripted runs (SCDA_AUTOPRESS) never take the mouse: an unattended window must not trap the cursor.
    g_mouseLook = LoadInputSettings(SettingsPath()).mouseLook && !getenv("SCDA_AUTOPRESS");
    if (g_mouseLook)
    {
        RAWINPUTDEVICE rid{ 0x01 /* generic desktop */, 0x02 /* mouse */, 0, hwnd };
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        if (GetForegroundWindow() == hwnd)
            HoldCursor(hwnd, true);
    }
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
    RuntimeLog("[thread] renderer host %lu\n", GetCurrentThreadId());
    while (!g_hwnd.load())
        Sleep(1);
    HWND hwnd = g_hwnd.load();
    RECT rc;
    GetClientRect(hwnd, &rc);
    D3D11Renderer renderer;
    renderer.SetCleanGoggles(s.cleanGoggles);
    if (!renderer.Init(hwnd, rc.right, rc.bottom, s.vsync, s.scale, s.anisotropy))
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
            // The title may be waiting for a query result before it swaps: run the queries it has
            // issued so far (partial frame), then collect results.
            Frames().FlushOpenIfWaiting();
            renderer.PollQueries();
            Sleep(1);
            continue;
        }
        // SCDA_TRACE_FRAME=<n>: log every draw and resolve of the first frame >= n (renderer development).
        static const long long traceFrame = getenv("SCDA_TRACE_FRAME") ? atoll(getenv("SCDA_TRACE_FRAME")) : -1;
        // First frame at or after n: the renderer may drop frame n itself.
        // Every Frame of that guest frame: its partial Frames hold most draws (each replays its log on
        // the renderer's state after the previous Frame).
        static long long tracedFrame = -1;
        if (tracedFrame < 0 && traceFrame >= 0 && (long long)frame.stats.frame >= traceFrame)
            tracedFrame = (long long)frame.stats.frame;
        const bool tracing = tracedFrame >= 0 && (long long)frame.stats.frame == tracedFrame;
        if (tracing)
        {
            // Register state per draw: the renderer's (end of the last frame) plus this frame's log.
            RegisterFile regs = renderer.Registers();
            size_t applied = 0;
            auto replay = [&](uint32_t end)
            {
                for (; applied < end && applied < frame.regWrites.size(); applied++)
                    if (frame.regWrites[applied].index == kRegClearAll)
                        regs.Clear();
                    else
                        regs.Set(frame.regWrites[applied].index, frame.regWrites[applied].value);
            };
            for (auto& d : frame.draws)
            {
                replay(d.regWritesEnd);
                RuntimeLog("[frame] draw prim %u src %u idx32 %d count %u base 0x%08X vs %016llx ps %016llx rt 0x%08X/0x%08X mode %u zctl 0x%08X zinfo 0x%08X\n",
                    d.primType, uint32_t(d.source), d.index32, d.indexCount, d.indexBase,
                    (unsigned long long)(d.vs ? d.vs->hash : 0), (unsigned long long)(d.ps ? d.ps->hash : 0),
                    regs[kRegRbSurfaceInfo], regs[kRegRbColorInfo], regs[kRegRbModeControl] & 7, regs[0x2200], regs[kRegRbDepthInfo]);
            }
            regs = renderer.Registers();
            applied = 0;
            for (auto& d : frame.draws)
            {
                replay(d.regWritesEnd);
                const RegisterFile& r = regs;
                RuntimeLog("[frame]  vte 0x%08X vp %08X %08X %08X %08X %08X %08X winoff 0x%08X scis 0x%08X 0x%08X blend 0x%08X mask 0x%08X\n",
                    r[0x2206], r[0x210F], r[0x2110], r[0x2111], r[0x2112], r[0x2113], r[0x2114], r[0x2080], r[0x2081], r[0x2082], r[0x2201], r[0x2104]);
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
                RuntimeLog("[frame] resolve @draw %u ctrl 0x%08X dest 0x%08X pitch 0x%08X info 0x%08X surf 0x%08X color 0x%08X depth 0x%08X clear 0x%08X zclear 0x%08X rect %d %d %d %d (%d)\n",
                    r.drawIndex, r.copyControl, r.destBase, r.destPitch, r.destInfo, r.surfaceInfo, r.colorInfo, r.depthInfo, r.colorClear, r.depthClear,
                    r.rect[0], r.rect[1], r.rect[2], r.rect[3], int(r.hasRect));
            RuntimeLog("[frame] frontbuffer 0x%08X\n", frame.frontbuffer);
        }
        // SCDA_DUMP_FRAME=<n>[,<n>...] (ascending): write the presented image of the first frame at or
        // after each n (the renderer may drop frames) to scda_frame_<frame>.bmp.
        static std::vector<uint64_t> dumpFrames = []
        {
            std::vector<uint64_t> v;
            for (const char* p = getenv("SCDA_DUMP_FRAME"); p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] ? 1 : 0))
                v.push_back(strtoull(p, nullptr, 10));
            return v;
        }();
        static size_t nextDump = 0;
        // Partial Frames of the dumped guest frame arm the dump too (their resolves and draws feed
        // SCDA_DUMP_RESOLVES / SCDA_DUMP_DRAWS); the image is written by the rest of the frame.
        bool dumping = nextDump < dumpFrames.size() && frame.stats.frame >= dumpFrames[nextDump];
        if (dumping)
        {
            renderer.DumpNextFrame("scda_frame_" + std::to_string(frame.stats.frame) + ".bmp");
            if (!frame.partial)
                while (nextDump < dumpFrames.size() && dumpFrames[nextDump] <= frame.stats.frame)
                    nextDump++;
        }
        dumping &= !frame.partial;
        renderer.Present(frame);
        if (frame.partial)
            continue;
        static uint64_t presents = 0;
        static const ULONGLONG startTick = GetTickCount64();
        presents++;
        // SCDA_TRACE_PRESENT=1: one line per present (frame, draws, frontbuffer address, fallback).
        static const bool tracePresent = getenv("SCDA_TRACE_PRESENT") != nullptr;
        if (tracePresent)
            RuntimeLog("[present] %llu draws %u drawn %u skipped %u resolves %u fb %08X %s\n", (unsigned long long)frame.stats.frame, frame.stats.draws,
                renderer.LastDrawn(), renderer.LastSkipped(), frame.stats.resolves, frame.frontbuffer,
                renderer.LastPresentedFrontbuffer() ? "frontbuffer" : "target");
        if (dumping || tracing)
            RuntimeLog("[frame] %llu: drawn %u skipped %u resolved %u presented %s (present #%llu, %.1f s)\n", (unsigned long long)frame.stats.frame,
                renderer.LastDrawn(), renderer.LastSkipped(), renderer.LastResolved(), renderer.LastPresentedFrontbuffer() ? "frontbuffer" : "target",
                (unsigned long long)presents, (GetTickCount64() - startTick) / 1000.0);
        if (GetTickCount() - lastTitle > 500)
        {
            lastTitle = GetTickCount();
            // SCDA_TRACE_GAMMA=1: display-controller registers (0x1800-0x1FFF, CPU MMIO writes land in
            // the register file in guest memory) logged when they change.
            static const bool traceGamma = getenv("SCDA_TRACE_GAMMA") != nullptr;
            if (traceGamma)
            {
                static uint32_t last[0x800];
                for (uint32_t i = 0; i < 0x800; i++)
                    if (uint32_t v = GuestLoad32(kGpuRegBase + (0x1800 + i) * 4); v != last[i])
                    {
                        RuntimeLog("[gamma] frame %llu dc reg %04X = %08X (was %08X)\n", (unsigned long long)frame.stats.frame, 0x1800 + i, v, last[i]);
                        last[i] = v;
                    }
            }
            wchar_t title[200];
            swprintf(title, 200, L"Splinter Cell: Double Agent Recomp - frame %llu draws %u (drawn %u skipped %u) resolves %u shaders %u compiled %u failed %u",
                (unsigned long long)frame.stats.frame, frame.stats.draws, renderer.LastDrawn(), renderer.LastSkipped(),
                frame.stats.resolves, frame.stats.shaders,
                ShaderCompilerReadyCount(), ShaderCompilerFailedCount());
            SetWindowTextW(hwnd, title);
            // Leak watch: host memory and handles once a minute.
            static ULONGLONG lastMem = 0;
            if (GetTickCount64() - lastMem > 60000)
            {
                lastMem = GetTickCount64();
                PROCESS_MEMORY_COUNTERS_EX pmc{ sizeof(pmc) };
                DWORD handles = 0;
                GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
                GetProcessHandleCount(GetCurrentProcess(), &handles);
                RuntimeLog("[mem] %.0f s: working set %zu MB, private %zu MB, handles %lu\n", (GetTickCount64() - startTick) / 1000.0,
                    size_t(pmc.WorkingSetSize >> 20), size_t(pmc.PrivateUsage >> 20), handles);
            }
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
