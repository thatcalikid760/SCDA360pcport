// Direct3D 11 renderer: executes each frame's draws (translated shaders, guest vertex memory,
// textures) into host render targets standing in for EDRAM surfaces, then blits to the window.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <bitset>
#include <functional>
#include <memory>
#include <string>
#include <gpu/frame_queue.h>

struct DrawResources;

class D3D11Renderer : public Renderer
{
public:
    // Largest resolve destination (guest pixels): the widest guest pitch, tall enough for square
    // shadow maps. Host EDRAM targets are sized per key (EdramSurfaceLayout: sample pitch x
    // EdramSampleRows); every host texture is scale times larger (Init).
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
    // The post-processing shader (FXAA, sharpening, ...) compiled; without it the PostFx settings do nothing.
    bool PostFxReady() const;
    // Writes the presented image (frontbuffer resolve, else the render target) as a 32-bit BMP
    // after the next frame (SCDA_DUMP_FRAME, tests).
    void DumpNextFrame(std::string path) { m_dumpPath = std::move(path); }
    // The window image of the next present (post-processing and menu included) as a BMP.
    void DumpNextBackBuffer(std::string path) { m_backDumpPath = std::move(path); }
    // Title relaunch (any thread): the next Present drops EDRAM targets and resolve copies first.
    static void RequestReset();
    // Register state after the last frame executed (SCDA_TRACE_FRAME replays the next frame's log on it).
    const RegisterFile& Registers() const;
    // [Display] CleanGoggles: drop the goggle scanline overlay and the noise displacement.
    void SetCleanGoggles(bool on) { m_cleanGoggles = on; }
    // Reads one texel (as RGBA8) of the last RT0 drawn or resolved (tests); 0 when unavailable.
    uint32_t ReadTargetTexel(uint32_t x, uint32_t y);
    // Post-processing of the presented frame (F1 menu): FXAA at the internal resolution, Lanczos
    // upscaling when the internal resolution is below the window, CAS sharpening (0..1). All off =
    // the plain blit (box filter / bilinear + gamma ramp), unchanged.
    struct PostFx
    {
        bool fxaa = false;
        bool upscale = false;
        float sharpness = 0.0f;
    };
    void SetPostFx(const PostFx& fx) { m_postFx = fx; }
    // Internal resolution scale (1-4), changed between guest frames: resolve copies are rescaled, EDRAM
    // targets rebuilt.
    void SetScale(uint32_t scale) { m_pendingScale = scale; }
    uint32_t Scale() const { return m_scale; }
    void SetVsync(bool on) { m_vsync = on; }
    void SetAnisotropy(uint32_t anisotropy);
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    ID3D11Device* Device() const { return m_device; }
    ID3D11DeviceContext* Context() const { return m_context; }
    // Drawn over each presented frame, after the gamma ramp (back buffer bound): the F1 menu.
    void SetOverlay(std::function<void()> overlay) { m_overlay = std::move(overlay); }
    // Writes completed occlusion query results into guest memory (also called while no frame is
    // queued: a title waiting for a result swaps no frame until it lands).
    void PollQueries();

private:
    void CreateBackBufferView();
    void ReleaseAll();
    bool CreateDrawResources();
    void ExecuteDraw(const DrawCall& d, const Frame& frame);
    void ExecuteResolve(const Resolve& r);
    void ExecuteQuery(const OcclusionQuery& q);
    void ExecuteFence(const Fence& f);
    void WritePendingFences();
    void BindRegisterState(const RegisterFile& regs, uint32_t texSlots);
    void ApplyRegisterWrites(const Frame& frame, size_t end);
    void UploadVertexMemory(const DrawCall& d, const Frame& frame);
    void Blit(ID3D11ShaderResourceView* source);
    // PostFx path of Blit: FXAA (source size), scaling, sharpening + ramp, through intermediate targets.
    void BlitPost(ID3D11ShaderResourceView* source);
    void PostPass(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width, uint32_t height, int mode, bool ramp);
    void ApplyScale(uint32_t scale);
    // ramp: apply the display gamma ramp (presented frames, so dumps match the window).
    void Dump(const std::string& path, ID3D11Texture2D* source, bool ramp = false);
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_backBuffer = nullptr;
    std::unique_ptr<DrawResources> m_draw;
    bool m_vsync = true;
    uint32_t m_scale = 1, m_anisotropy = 1;
    bool m_cleanGoggles = false;
    uint64_t m_presented = 0;
    uint32_t m_lastDrawn = 0, m_lastSkipped = 0, m_lastResolved = 0;
    bool m_lastFrontbuffer = false;
    std::string m_dumpPath, m_backDumpPath;
    PostFx m_postFx;
    uint32_t m_pendingScale = 0;
    bool m_midGuestFrame = false; // the last Frame was partial: the guest frame continues
    std::function<void()> m_overlay;
};
