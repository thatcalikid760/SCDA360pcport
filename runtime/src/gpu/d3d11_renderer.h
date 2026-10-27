// Direct3D 11 renderer. 2b-1: clears and presents once per frame; draws come in 2b-3.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <gpu/frame_queue.h>

class D3D11Renderer : public Renderer
{
public:
    ~D3D11Renderer() override;
    bool Init(HWND hwnd, uint32_t width, uint32_t height, bool vsync);
    void Present(const Frame& frame) override;
    void Resize(uint32_t width, uint32_t height);
    uint64_t PresentedFrames() const { return m_presented; }

private:
    void CreateBackBufferView();
    void ReleaseAll();
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_backBuffer = nullptr;
    bool m_vsync = true;
    uint64_t m_presented = 0;
};
