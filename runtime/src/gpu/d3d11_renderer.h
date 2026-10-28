// Direct3D 11 renderer: executes each frame's draws (translated shaders, guest vertex memory,
// textures) into a host render target standing in for EDRAM, then blits it to the window.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <bitset>
#include <memory>
#include <string>
#include <gpu/frame_queue.h>

struct DrawResources;

class D3D11Renderer : public Renderer
{
public:
    // shortcut: one 1280x720 target for every draw; EDRAM surfaces and resolves come later.
    static constexpr uint32_t kTargetWidth = 1280, kTargetHeight = 720;

    D3D11Renderer();
    ~D3D11Renderer() override;
    bool Init(HWND hwnd, uint32_t width, uint32_t height, bool vsync);
    void Present(const Frame& frame) override;
    void Resize(uint32_t width, uint32_t height);
    uint64_t PresentedFrames() const { return m_presented; }
    // Draws executed / skipped (unsupported primitive, index source or shader) in the last frame.
    uint32_t LastDrawn() const { return m_lastDrawn; }
    uint32_t LastSkipped() const { return m_lastSkipped; }
    // Writes the render target as a 32-bit BMP after the next frame (SCDA_DUMP_FRAME, tests).
    void DumpNextFrame(std::string path) { m_dumpPath = std::move(path); }
    // Reads one RGBA8 texel of the render target (tests); 0 when unavailable.
    uint32_t ReadTargetTexel(uint32_t x, uint32_t y);

private:
    void CreateBackBufferView();
    void ReleaseAll();
    bool CreateDrawResources();
    void ExecuteDraw(const DrawCall& d, const Frame& frame);
    void BindRegisterState(const RegisterFile& regs);
    void UploadVertexMemory(const RegisterFile& regs, const std::bitset<96>& slots, const Frame& frame);
    void Blit();
    void Dump(const std::string& path);
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_backBuffer = nullptr;
    std::unique_ptr<DrawResources> m_draw;
    bool m_vsync = true;
    uint64_t m_presented = 0;
    uint32_t m_lastDrawn = 0, m_lastSkipped = 0;
    std::string m_dumpPath;
};
