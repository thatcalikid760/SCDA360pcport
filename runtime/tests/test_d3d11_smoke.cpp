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
    Frame f;
    r.Present(f);
    r.Resize(640, 360);
    r.Present(f);
    CHECK_EQ(r.PresentedFrames(), 2ull);
    DestroyWindow(hwnd);
}
