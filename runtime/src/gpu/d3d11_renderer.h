// Direct3D 11 renderer: executes each frame's draws (translated shaders, guest vertex memory,
// textures) into host render targets standing in for EDRAM surfaces, then blits to the window.
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
    // Size of every host EDRAM surface (colour and depth targets, one per EdramColorKey /
    // EdramDepthKey): the widest guest pitch, tall enough for square shadow maps.
    // Guest units: the host textures are scale times larger (Init).
    static constexpr uint32_t kTargetWidth = 1280, kTargetHeight = 1280;

    D3D11Renderer();
    ~D3D11Renderer() override;
    // scale: internal resolution factor (settings.ini [Display] Scale, 1-4) for EDRAM targets and resolves;
    // anisotropy: minimum for linear-filtered textures ([Display] Anisotropy, 1-16).
    bool Init(HWND hwnd, uint32_t width, uint32_t height, bool vsync, uint32_t scale = 1, uint32_t anisotropy = 1);
    void Present(const Frame& frame) override;
    void Resize(uint32_t width, uint32_t height);
    uint64_t PresentedFrames() const { return m_presented; }
    // Draws executed / skipped (unsupported primitive, index source or shader) in the last frame.
    uint32_t LastDrawn() const { return m_lastDrawn; }
    uint32_t LastSkipped() const { return m_lastSkipped; }
    // Resolves that copied the target in the last frame; whether it presented the swap's frontbuffer
    // resolve (else the render target itself).
    uint32_t LastResolved() const { return m_lastResolved; }
    bool LastPresentedFrontbuffer() const { return m_lastFrontbuffer; }
    // Writes the presented image (frontbuffer resolve, else the render target) as a 32-bit BMP
    // after the next frame (SCDA_DUMP_FRAME, tests).
    void DumpNextFrame(std::string path) { m_dumpPath = std::move(path); }
    // Title relaunch (any thread): the next Present drops EDRAM targets and resolve copies first.
    static void RequestReset();
    // Register state after the last frame executed (SCDA_TRACE_FRAME replays the next frame's log on it).
    const RegisterFile& Registers() const;
    // Reads one texel (as RGBA8) of the last RT0 drawn or resolved (tests); 0 when unavailable.
    uint32_t ReadTargetTexel(uint32_t x, uint32_t y);

private:
    void CreateBackBufferView();
    void ReleaseAll();
    bool CreateDrawResources();
    void ExecuteDraw(const DrawCall& d, const Frame& frame);
    void ExecuteResolve(const Resolve& r);
    void ExecuteQuery(const OcclusionQuery& q);
    void PollQueries();
    void BindRegisterState(const RegisterFile& regs, uint32_t texSlots);
    void ApplyRegisterWrites(const Frame& frame, size_t end);
    void UploadVertexMemory(const DrawCall& d, const Frame& frame);
    void Blit(ID3D11ShaderResourceView* source);
    void Dump(const std::string& path, ID3D11Texture2D* source);
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_backBuffer = nullptr;
    std::unique_ptr<DrawResources> m_draw;
    bool m_vsync = true;
    uint32_t m_scale = 1, m_anisotropy = 1;
    uint64_t m_presented = 0;
    uint32_t m_lastDrawn = 0, m_lastSkipped = 0, m_lastResolved = 0;
    bool m_lastFrontbuffer = false;
    std::string m_dumpPath;
};
