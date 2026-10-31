#include "check.h"
#include <windows.h>
#include <gpu/d3d11_renderer.h>

TEST(d3d11_init_on_hidden_window)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SCDATestWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"t", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(hwnd != nullptr);
    D3D11Renderer r;
    if (!r.Init(hwnd, 320, 180, false))
    {
        printf("  (skipped: no D3D11 device)\n");
        DestroyWindow(hwnd);
        return;
    }
    CHECK(r.PostFxReady());
    Frame f;
    r.Present(f);
    r.Resize(640, 360);
    r.Present(f);
    CHECK_EQ(r.PresentedFrames(), 2ull);
    DestroyWindow(hwnd);
}

// A clear-only resolve fills the target with RB_COLOR_CLEAR; a copy resolve to the swap's
// frontbuffer address is what gets presented.
TEST(d3d11_resolve_clears_and_presents_frontbuffer)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SCDATestWindow2";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"t", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, wc.hInstance, nullptr);
    D3D11Renderer r;
    if (!hwnd || !r.Init(hwnd, 320, 180, false))
    {
        printf("  (skipped: no D3D11 device)\n");
        if (hwnd)
            DestroyWindow(hwnd);
        return;
    }
    Frame f;
    Resolve clear{};
    clear.copyControl = (3u << 20) | 0x100; // clear only, colour clear enabled
    clear.colorClear = 0xFF0000FF;          // opaque red (red in the low byte)
    Resolve copy{};
    copy.destBase = 0x00682000;
    copy.destPitch = 1280 | (720u << 16);
    f.resolves = { clear, copy };
    f.frontbuffer = 0x00682000;
    r.Present(f);
    CHECK_EQ(r.ReadTargetTexel(0, 0), 0xFF0000FFu);
    CHECK_EQ(r.LastResolved(), 1u);
    CHECK(r.LastPresentedFrontbuffer());
    DestroyWindow(hwnd);
}
