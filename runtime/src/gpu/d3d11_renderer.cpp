#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>
#include <gpu/d3d11_renderer.h>
#include <gpu/draw_state.h>
#include <gpu/shader_compiler.h>
#include <gpu/xenos/translate.h>
#include <gpu_state.h>
#include <runtime.h>

using Microsoft::WRL::ComPtr;

template <class T> static void Release(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

static const char kBlitVs[] = R"HLSL(
void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)HLSL";

static const char kBlitPs[] = R"HLSL(
Texture2D t : register(t0);
SamplerState s : register(s0);
// A source larger than the window (internal resolution scale) is box-filtered: n x n taps at the
// centres of the source texels under the window pixel; otherwise one bilinear tap.
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float w, h;
    t.GetDimensions(w, h);
    float2 texels = float2(ddx(uv.x) * w, ddy(uv.y) * h);
    int2 n = clamp(int2(ceil(texels - 0.01)), 1, 4);
    // Scaled frames: the last host column/row can be stale (half-integer-edged passes miss it with the
    // host-pixel half-pixel offset), so taps stop at the one before.
    float2 last = n > 1 ? (float2(w, h) - 1.5) / float2(w, h) : 1.0;
    float4 sum = 0;
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++)
            sum += t.SampleLevel(s, min(uv + ((float2(x, y) + 0.5) / float2(n) - 0.5) * texels / float2(w, h), last), 0);
    return sum / float(n.x * n.y);
}
)HLSL";

// Resolve copy into a fixed-point destination format: the EDRAM -> memory conversion saturates,
// but float host targets hold values past [0, 1] (FP16 7e3 blending overflows the 2-bit alpha).
static const char kSaturateCs[] = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Rect : register(b0) { int4 rect; int dy; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int2 p = int2(rect.xy) + int2(id.xy);
    if (p.x < rect.z && p.y < rect.w)
        dst[p + int2(0, dy)] = saturate(src[p]);
}
)HLSL";

// Rect lists: 3 corners per rectangle. The corner opposite the longest edge is the right angle;
// the 4th corner (and its interpolators) is the other two minus it. Layout = translated VS output.
static const char kRectListGs[] = R"HLSL(
struct V
{
    float4 pos : SV_Position;
    float4 t[16] : TEXCOORD0;
    float4 c[2] : COLOR0;
};
[maxvertexcount(4)]
void main(triangle V v[3], inout TriangleStream<V> s)
{
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
    uint c = d12 >= d01 && d12 >= d20 ? 0 : (d20 >= d01 ? 1 : 2);
    V vc = v[c], va = v[(c + 1) % 3], vb = v[(c + 2) % 3], vd;
    vd.pos = va.pos + vb.pos - vc.pos;
    [unroll] for (int i = 0; i < 16; i++)
        vd.t[i] = va.t[i] + vb.t[i] - vc.t[i];
    [unroll] for (int j = 0; j < 2; j++)
        vd.c[j] = va.c[j] + vb.c[j] - vc.c[j];
    s.Append(vc);
    s.Append(va);
    s.Append(vb);
    s.Append(vd);
}
)HLSL";

// Host render target for one EDRAM colour surface (EdramColorKey).
struct ColorTarget
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

// Device objects for executing draws; rebuilt with the device.
struct DrawResources
{
    // EDRAM surfaces, kTargetWidth x kTargetHeight (x scale) each, kept across frames like EDRAM.
    // Depth is float with stencil: it holds both guest formats (D24S8, D24FS8).
    uint32_t scale = 1; // host texels per guest pixel, each axis (targets and resolve copies)
    uint32_t anisotropy = 1; // settings minimum for linear-filtered textures
    std::unordered_map<uint32_t, ColorTarget> colorTargets;
    std::unordered_map<uint32_t, ComPtr<ID3D11DepthStencilView>> depthTargets;
    uint32_t boundKeys[5] = {}; // RT0-3 + depth keys bound now; ~0u = none
    bool bound = false;         // boundKeys valid (false after the blit or a new frame)
    ColorTarget* last = nullptr; // RT0 of the last draw or resolve: presented when there is no frontbuffer
    ComPtr<ID3D11VertexShader> blitVs;
    ComPtr<ID3D11PixelShader> blitPs;
    ComPtr<ID3D11GeometryShader> rectListGs;
    ComPtr<ID3D11ComputeShader> saturateCs;
    ComPtr<ID3D11Buffer> saturateRect;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants[3]; // b0 VS, b1 PS, b2 shared
    ComPtr<ID3D11ShaderResourceView> white;
    // Raw view of guest physical memory from address 0 (t96); grown on demand.
    ComPtr<ID3D11Buffer> memory;
    ComPtr<ID3D11ShaderResourceView> memorySrv;
    uint32_t memoryBytes = 0;
    // Dynamic ring for per-draw index data (NO_OVERWRITE appends, DISCARD on wrap).
    ComPtr<ID3D11Buffer> indexRing;
    uint32_t indexRingBytes = 0, indexRingPos = 0;
    std::vector<uint8_t> indexScratch; // reused per draw
    std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> vs;
    std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> ps;
    // Fixed-function state by the registers it is built from.
    std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blend;
    std::unordered_map<uint32_t, ComPtr<ID3D11RasterizerState>> raster;
    std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> depthStates;
    std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> samplers;
    // Per frame: guest ranges already uploaded, textures by fetch constant.
    std::unordered_map<uint64_t, uint32_t> uploaded; // range key -> Frame::memory snapshot in the buffer
    std::unordered_map<uint64_t, ComPtr<ID3D11ShaderResourceView>> textures;
    // Converted textures across frames by fetch constant, reused while the guest bytes hash the same.
    struct CachedTexture
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        uint64_t hash = 0, lastUsed = 0;
        uint64_t probe = 0, fullAt = 0; // ProbeBytes of level 0 and present number at the last full hash
        uint32_t stable = 0; // consecutive full hashes equal to hash
        // Guest bytes of level 0, 1, ...: fixed by the fetch constant (the key), so computed once.
        std::vector<uint32_t> levelBytes;
    };
    std::unordered_map<uint64_t, CachedTexture> textureCache;
    // Register state replayed from Frame::regWrites; applied = writes of the current frame done.
    RegisterFile regs;
    size_t applied = 0;
    bool stateDirty = true;            // registers changed, or bindings invalidated, since the last bind
    bool aluDirty[2] = { true, true }; // b0 / b1 constants changed since their last upload
    uint32_t lastTexSlots = 0;         // texture slots the last bind decoded
    // Resolve copies of the target by guest physical destination; kept across frames (the
    // frontbuffer and render-to-texture results are read after the frame that made them).
    struct Resolved
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav; // float formats: saturating copies (kSaturateCs)
        uint32_t width = 0, height = 0; // guest pixels; the texture is scale times larger
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // the source target's
        uint32_t rowBytes = 0; // guest bytes per row (RB_COPY_DEST_INFO format x width)
        uint32_t destFormat = 0; // RB_COPY_DEST_INFO bits 7-12
        uint32_t version = 0;    // bumped by each copy into texture
        uint64_t guestHash = 0;  // SCDA_TRACE_STALE: guest bytes under the texture after the last copy
        uint64_t checkedAt = ~0ull; // SCDA_TRACE_STALE: presented frame of the last hash check
        uint64_t staleSince = 0;    // SCDA_TRACE_STALE: presented frame + 1 the change was seen, 0 = fresh
        // Fetches smaller than the resolve (pitch-aligned dest, e.g. a 1x1 fetch of a 32x1 resolve
        // that wrote 8x8): the top-left w x h, so normalized coordinates hit the written texels.
        struct Crop
        {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            uint32_t version = ~0u;
        };
        std::unordered_map<uint32_t, Crop> crops; // by w << 16 | h
    };
    std::unordered_map<uint32_t, Resolved> resolved;
};

static std::atomic<bool> g_resetRequested;

void D3D11Renderer::RequestReset() { g_resetRequested = true; }

D3D11Renderer::D3D11Renderer() = default;

void D3D11Renderer::ReleaseAll()
{
    m_draw.reset();
    Release(m_backBuffer);
    Release(m_swapChain);
    Release(m_context);
    Release(m_device);
}

D3D11Renderer::~D3D11Renderer() { ReleaseAll(); }

bool D3D11Renderer::Init(HWND hwnd, uint32_t width, uint32_t height, bool vsync, uint32_t scale, uint32_t anisotropy)
{
    m_scale = std::clamp<uint32_t>(scale, 1, 4);
    m_anisotropy = std::clamp<uint32_t>(anisotropy, 1, 16);
    RuntimeLog("[d3d11] internal resolution %ux%u (scale %u)\n", kTargetWidth * m_scale, 720 * m_scale, m_scale);
    m_vsync = vsync;
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    // SCDA_D3D_DEBUG=1: debug layer; its messages go to the runtime log after each frame.
    UINT flags = getenv("SCDA_D3D_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1,
        D3D11_SDK_VERSION, &sd, &m_swapChain, &m_device, nullptr, &m_context);
    if (FAILED(hr) && flags)
    {
        printf("[d3d11] debug layer unavailable (install Graphics Tools)\n");
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, &sd, &m_swapChain, &m_device, nullptr, &m_context);
    }
    if (FAILED(hr))
        return false;
    CreateBackBufferView();
    if (m_backBuffer && !CreateDrawResources())
        printf("[d3d11] draw resources unavailable: presenting placeholder frames\n");
    return m_backBuffer != nullptr;
}

void D3D11Renderer::CreateBackBufferView()
{
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&tex)))
    {
        m_device->CreateRenderTargetView(tex, nullptr, &m_backBuffer);
        tex->Release();
    }
}

bool D3D11Renderer::CreateDrawResources()
{
    auto r = std::make_unique<DrawResources>();
    r->scale = m_scale;
    r->anisotropy = m_anisotropy;
    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;

    std::string error;
    auto vs = CompileHlsl(kBlitVs, false, &error), ps = CompileHlsl(kBlitPs, true, &error);
    if (vs.empty() || ps.empty() || FAILED(m_device->CreateVertexShader(vs.data(), vs.size(), nullptr, &r->blitVs)) ||
        FAILED(m_device->CreatePixelShader(ps.data(), ps.size(), nullptr, &r->blitPs)))
    {
        printf("[d3d11] blit shader: %s\n", error.c_str());
        return false;
    }
    auto gs = CompileHlsl(kRectListGs, "gs_5_0", &error);
    if (gs.empty() || FAILED(m_device->CreateGeometryShader(gs.data(), gs.size(), nullptr, &r->rectListGs)))
        printf("[d3d11] rect list shader: %s\n", error.c_str()); // rect lists are then skipped
    auto cs = CompileHlsl(kSaturateCs, "cs_5_0", &error);
    D3D11_BUFFER_DESC rb{};
    rb.ByteWidth = 32;
    rb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (cs.empty() || FAILED(m_device->CreateComputeShader(cs.data(), cs.size(), nullptr, &r->saturateCs)) ||
        FAILED(m_device->CreateBuffer(&rb, nullptr, &r->saturateRect)))
        printf("[d3d11] saturate shader: %s\n", error.c_str()); // resolves then copy unclamped

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // blit; draws use state from PA_SU_SC_MODE_CNTL
    rd.DepthClipEnable = TRUE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; // blit; draws use samplers from fetch constants
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateRasterizerState(&rd, &r->rasterizer)) || FAILED(m_device->CreateSamplerState(&sd, &r->sampler)))
        return false;

    const UINT cbSizes[3] = { 4096, 4096, sizeof(SharedConstants) };
    for (int i = 0; i < 3; i++)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = cbSizes[i];
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &r->constants[i])))
            return false;
    }

    // Unsupported or unreadable textures sample opaque white.
    const uint32_t white = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC wd = td;
    wd.Width = wd.Height = 1;
    wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    wd.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA init{ &white, 4, 4 };
    ComPtr<ID3D11Texture2D> wt;
    if (FAILED(m_device->CreateTexture2D(&wd, &init, &wt)) || FAILED(m_device->CreateShaderResourceView(wt.Get(), nullptr, &r->white)))
        return false;
    m_draw = std::move(r);
    return true;
}

void D3D11Renderer::Resize(uint32_t width, uint32_t height)
{
    if (!m_swapChain || !width || !height)
        return;
    m_width = width;
    m_height = height;
    // Views must be gone (ClearState + Flush release deferred references) before ResizeBuffers.
    m_context->ClearState();
    Release(m_backBuffer);
    m_context->Flush();
    if (FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
        printf("[d3d11] ResizeBuffers failed\n");
    CreateBackBufferView();
}

// Writes RGBA8 texels (row by row) as a top-down 32-bit BMP.
static bool WriteBmp(const std::string& path, std::vector<uint32_t> texels, uint32_t width, uint32_t height)
{
    FILE* f = texels.empty() ? nullptr : fopen(path.c_str(), "wb");
    if (!f)
        return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = width;
    ih.biHeight = -int32_t(height); // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + uint32_t(texels.size() * 4);
    for (auto& t : texels) // RGBA -> BGRA
        t = (t & 0xFF00FF00) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(texels.data(), 4, texels.size(), f);
    fclose(f);
    return true;
}

// SCDA_TRACE_RESOLVE=1: each suspicious resolve/fetch pairing once (sub-page dest offsets, dest or
// fetch bases inside a resolve texture that do not match it, exact hits with another shape).
static bool TraceResolveOnce(uint64_t key)
{
    static const bool on = getenv("SCDA_TRACE_RESOLVE") != nullptr;
    static std::unordered_set<uint64_t> seen;
    return on && seen.size() < 2000 && seen.insert(key).second;
}

// SCDA_TRACE_RESOLVE=<n>[/<period>]: also log every resolve and texture fetch of presented frame n
// (and every period-th one after it) in order.
static bool TraceResolveFrame(uint64_t presented)
{
    static const char* v = getenv("SCDA_TRACE_RESOLVE");
    static const long long frame = v && strcmp(v, "1") ? atoll(v) : -1;
    static const long long period = v && strchr(v, '/') ? std::max(1ll, atoll(strchr(v, '/') + 1)) : 0;
    const long long n = (long long)presented;
    return frame >= 0 && (n == frame || (period && n > frame && (n - frame) % period == 0));
}

static const uint8_t* PhysicalPtr(uint32_t physical) { return (const uint8_t*)GuestPtr(GuestPhysical(physical)); }

static uint32_t ToUnorm8(float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// Garbage registers must not allocate targets without bound (each is several MB).
constexpr size_t kMaxEdramTargets = 64;

// The host target for an EDRAM colour surface, created (cleared to 0) on first use; null on failure.
// 64-bit content hash (4 independent multiply-xor lanes); never 0.
static uint64_t HashBytes(const uint8_t* p, size_t n)
{
    uint64_t h[4] = { 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; l++)
        {
            uint64_t w;
            memcpy(&w, p + i + l * 8, 8);
            h[l] = (h[l] ^ w) * 0x100000001B3ull;
            h[l] ^= h[l] >> 29;
        }
    uint64_t r = n;
    for (; i < n; i++)
        r = (r ^ p[i]) * 0x100000001B3ull;
    for (int l = 0; l < 4; l++)
        r = (r ^ h[l]) * 0x9E3779B97F4A7C15ull;
    return r | 1;
}

// Per-present texture check: first and last 4 KB plus 16 strided 256-byte runs (all of small ones); never 0.
static uint64_t ProbeBytes(const uint8_t* p, size_t n)
{
    if (n <= 16384)
        return HashBytes(p, n);
    uint64_t h = HashBytes(p, 4096) ^ HashBytes(p + n - 4096, 4096) * 3;
    for (size_t i = 1; i <= 16; i++)
        h = (h ^ HashBytes(p + (n * i / 17 & ~size_t(255)), 256)) * 1099511628211ull;
    return h | 1;
}

constexpr uint32_t kTexStableFrames = 4, kTexFullEvery = 16;
static struct { uint32_t probes = 0, skips = 0; uint64_t bytes = 0; } g_texHash; // SCDA_TRACE_TEXHASH counters (render thread)

// The resolve's view for a w x h fetch: the whole texture, or a top-left crop copied after each resolve.
static ID3D11ShaderResourceView* ResolvedView(ID3D11Device* device, ID3D11DeviceContext* context, DrawResources::Resolved& res, uint32_t w, uint32_t h)
{
    if (w >= res.width && h >= res.height)
        return res.srv.Get();
    auto& crop = res.crops[(w << 16) | h];
    D3D11_TEXTURE2D_DESC sd;
    res.texture->GetDesc(&sd);
    const uint32_t scale = sd.Width / res.width;
    w *= scale;
    h *= scale;
    if (!crop.srv)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = res.format;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &crop.texture)) ||
            FAILED(device->CreateShaderResourceView(crop.texture.Get(), nullptr, &crop.srv)))
            return res.srv.Get();
    }
    if (crop.version != res.version)
    {
        D3D11_BOX box{ 0, 0, 0, w, h, 1 };
        context->CopySubresourceRegion(crop.texture.Get(), 0, 0, 0, 0, res.texture.Get(), 0, &box);
        crop.version = res.version;
    }
    return crop.srv.Get();
}

static ColorTarget* ColorTargetFor(ID3D11Device* device, ID3D11DeviceContext* context, DrawResources& r, uint32_t key)
{
    if (auto it = r.colorTargets.find(key); it != r.colorTargets.end())
        return &it->second;
    if (r.colorTargets.size() >= kMaxEdramTargets)
        return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = D3D11Renderer::kTargetWidth * r.scale;
    td.Height = D3D11Renderer::kTargetHeight * r.scale;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT(HostColorFormat((key >> 12) & 0xF));
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ColorTarget t;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &t.texture)) ||
        FAILED(device->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv)) ||
        FAILED(device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv)))
        return nullptr;
    const float zero[4] = {};
    context->ClearRenderTargetView(t.rtv.Get(), zero);
    return &r.colorTargets.emplace(key, std::move(t)).first->second;
}

static ID3D11DepthStencilView* DepthTargetFor(ID3D11Device* device, ID3D11DeviceContext* context, DrawResources& r, uint32_t key)
{
    if (auto it = r.depthTargets.find(key); it != r.depthTargets.end())
        return it->second.Get();
    if (r.depthTargets.size() >= kMaxEdramTargets)
        return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = D3D11Renderer::kTargetWidth * r.scale;
    td.Height = D3D11Renderer::kTargetHeight * r.scale;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &tex)) || FAILED(device->CreateDepthStencilView(tex.Get(), nullptr, &dsv)))
        return nullptr;
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    return (r.depthTargets[key] = dsv).Get();
}

// Guest vertex memory: uploads the snapshots the draw reads unless that version of the range is
// already in the buffer (a range the CPU rewrote between draws is uploaded again).
void D3D11Renderer::UploadVertexMemory(const DrawCall& d, const Frame& frame)
{
    DrawResources& r = *m_draw;
    constexpr uint32_t kMaxMemory = 512u << 20;
    for (uint32_t index : d.memory)
    {
        if (index >= frame.memory.size())
            continue;
        const Frame::Snapshot& snap = frame.memory[index];
        MemoryRange range{ uint32_t(snap.key >> 32), uint32_t(snap.key) };
        if (range.physical + uint64_t(range.bytes) > kMaxMemory)
            continue;
        // shortcut: overlapping ranges with different keys do not invalidate each other.
        auto up = r.uploaded.find(snap.key);
        if (up != r.uploaded.end() && up->second == index)
            continue;
        uint32_t end = range.physical + range.bytes;
        if (end > r.memoryBytes)
        {
            uint32_t size = std::min<uint32_t>((end + (16u << 20) - 1) & ~((16u << 20) - 1), kMaxMemory);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = size;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT_R32_TYPELESS;
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            vd.BufferEx.NumElements = size / 4;
            vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            r.memory.Reset();
            r.memorySrv.Reset();
            r.memoryBytes = 0;
            r.uploaded.clear(); // new buffer: earlier ranges must be uploaded again
            if (FAILED(m_device->CreateBuffer(&bd, nullptr, &r.memory)) ||
                FAILED(m_device->CreateShaderResourceView(r.memory.Get(), &vd, &r.memorySrv)))
            {
                printf("[d3d11] guest memory buffer (%u MB) failed\n", size >> 20);
                r.memory.Reset();
                r.memorySrv.Reset();
                return;
            }
            r.memoryBytes = size;
        }
        D3D11_BOX box{ range.physical, 0, 0, end, 1, 1 };
        m_context->UpdateSubresource(r.memory.Get(), 0, &box, snap.bytes.data(), 0, 0);
        r.uploaded[snap.key] = index;
    }
    ID3D11ShaderResourceView* mem = r.memorySrv.Get();
    m_context->VSSetShaderResources(kHlslMemorySlot, 1, &mem);
}

// texSlots: texture fetch slots (bit n) the draw's shaders sample; only those are decoded and bound.
const RegisterFile& D3D11Renderer::Registers() const
{
    static const RegisterFile none;
    return m_draw ? m_draw->regs : none;
}

void D3D11Renderer::ApplyRegisterWrites(const Frame& frame, size_t end)
{
    DrawResources& r = *m_draw;
    end = std::min(end, frame.regWrites.size());
    for (; r.applied < end; r.applied++)
    {
        const RegisterWrite& w = frame.regWrites[r.applied];
        if (w.index == kRegClearAll)
        {
            r.regs.Clear();
            r.aluDirty[0] = r.aluDirty[1] = true;
        }
        else
        {
            r.regs.Set(w.index, w.value);
            if (w.index - kRegAluConstants < 2048)
                r.aluDirty[(w.index - kRegAluConstants) >> 10] = true;
        }
        r.stateDirty = true;
    }
}

void D3D11Renderer::BindRegisterState(const RegisterFile& regs, uint32_t texSlots)
{
    DrawResources& r = *m_draw;
    for (uint32_t pixel = 0; pixel < 2; pixel++)
    {
        if (!r.aluDirty[pixel])
            continue; // b0/b1 already hold these constants
        uint32_t alu[1024];
        CopyAluConstants(regs, pixel != 0, alu);
        m_context->UpdateSubresource(r.constants[pixel].Get(), 0, nullptr, alu, 0, 0);
        r.aluDirty[pixel] = false;
    }
    SharedConstants shared = BuildSharedConstants(regs, kTargetWidth, kTargetHeight);
    shared.positionScale = 1.0f / float(r.scale);
    m_context->UpdateSubresource(r.constants[2].Get(), 0, nullptr, &shared, 0, 0);

    // Render targets: RT0, RT1-3 with a colour mask (MRT), and the depth surface.
    static constexpr uint32_t kInfo[4] = { kRegRbColorInfo, kRegRbColor1Info, kRegRbColor2Info, kRegRbColor3Info };
    const uint32_t surface = regs[kRegRbSurfaceInfo];
    uint32_t keys[5] = { ~0u, ~0u, ~0u, ~0u, EdramDepthKey(regs[kRegRbDepthInfo], surface) };
    ID3D11RenderTargetView* rtvs[4] = {};
    UINT count = 0;
    for (uint32_t n = 0; n < 4; n++)
    {
        const uint32_t key = EdramColorKey(regs[kInfo[n]], surface);
        if (n && (!((regs[kRegRbColorMask] >> (4 * n)) & 0xF) || std::count(keys, keys + n, key)))
            continue; // RT n unused, or the same surface as an earlier RT
        if (ColorTarget* t = ColorTargetFor(m_device, m_context, r, key))
        {
            keys[n] = key;
            rtvs[n] = t->rtv.Get();
            count = n + 1;
            if (!n)
                r.last = t;
        }
    }
    if (!r.bound || !std::equal(keys, keys + 5, r.boundKeys))
    {
        m_context->OMSetRenderTargets(count, rtvs, DepthTargetFor(m_device, m_context, r, keys[4]));
        std::copy(keys, keys + 5, r.boundKeys);
        r.bound = true;
    }

    HostBlend b = DecodeBlend(regs);
    if ((regs[kRegRbModeControl] & 7) == 5) // depth-only pass (Z prepass, occlusion queries)
        b.writeMask = 0;
    // SCDA_TRACE_BLEND=1: each RB_BLENDCONTROL0 per RT0 format and write mask once.
    static const bool traceBlend = getenv("SCDA_TRACE_BLEND") != nullptr;
    static std::unordered_set<uint64_t> blendSeen;
    if (traceBlend && blendSeen.size() < 4000 &&
        blendSeen.insert((uint64_t(regs[0x2201]) << 12) | ((regs[kRegRbColorInfo] >> 16 & 0xF) << 4) | b.writeMask).second)
        RuntimeLog("[blend] frame %llu rt fmt %u blend %08X mask %X\n", m_presented, regs[kRegRbColorInfo] >> 16 & 0xF, regs[0x2201], b.writeMask);
    auto& blend = r.blend[(uint64_t(regs[0x2201]) << 4) | b.writeMask];
    if (!blend)
    {
        D3D11_BLEND_DESC bd{};
        auto& t = bd.RenderTarget[0];
        t.BlendEnable = b.enable;
        t.SrcBlend = D3D11_BLEND(b.src);
        t.DestBlend = D3D11_BLEND(b.dst);
        t.BlendOp = D3D11_BLEND_OP(b.op);
        t.SrcBlendAlpha = D3D11_BLEND(b.srcAlpha);
        t.DestBlendAlpha = D3D11_BLEND(b.dstAlpha);
        t.BlendOpAlpha = D3D11_BLEND_OP(b.opAlpha);
        t.RenderTargetWriteMask = UINT8(b.writeMask);
        m_device->CreateBlendState(&bd, &blend);
    }
    m_context->OMSetBlendState(blend.Get(), b.factor, 0xFFFFFFFF);

    uint32_t cull = HostCullMode(regs), ccw = HostFrontCounterClockwise(regs);
    auto& raster = r.raster[cull | (ccw << 4)];
    if (!raster)
    {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_MODE(cull);
        rd.FrontCounterClockwise = ccw;
        rd.DepthClipEnable = TRUE;
        m_device->CreateRasterizerState(&rd, &raster);
    }
    m_context->RSSetState(raster.Get());

    HostDepthStencil z = DecodeDepthStencil(regs);
    auto& depth = r.depthStates[(uint64_t(regs[0x2200]) << 16) | (z.stencilEnable ? (regs[0x210D] >> 8) & 0xFFFF : 0)];
    if (!depth)
    {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = z.depthEnable;
        dd.DepthWriteMask = z.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_FUNC(z.depthFunc);
        dd.StencilEnable = z.stencilEnable;
        dd.StencilReadMask = z.readMask;
        dd.StencilWriteMask = z.writeMask;
        auto face = [](const HostStencilFace& f) {
            return D3D11_DEPTH_STENCILOP_DESC{D3D11_STENCIL_OP(f.fail), D3D11_STENCIL_OP(f.depthFail), D3D11_STENCIL_OP(f.pass), D3D11_COMPARISON_FUNC(f.func)};
        };
        dd.FrontFace = face(z.front);
        dd.BackFace = face(z.back);
        m_device->CreateDepthStencilState(&dd, &depth);
    }
    m_context->OMSetDepthStencilState(depth.Get(), z.ref);

    // Textures: every texture fetch constant the stages might sample (the shader's slots are not
    // known here), converted on the CPU once per frame.
    ID3D11ShaderResourceView* srvs[32];
    for (uint32_t slot = 0; slot < 32; slot++)
    {
        srvs[slot] = r.white.Get();
        if (!(texSlots >> slot & 1))
            continue;
        TextureFetch t = DecodeTextureFetch(regs, slot);
        if (!t.valid)
            continue;
        if (TraceResolveFrame(m_presented))
        {
            auto hit = r.resolved.find(t.physical);
            RuntimeLog("[rtrace] draw %u slot %u fetch %08X fmt %u %ux%u tiled %d resolve %ux%u fmt %u\n", m_lastDrawn + m_lastSkipped, slot, t.physical,
                t.format, t.width, t.height, t.tiled, hit != r.resolved.end() ? hit->second.width : 0, hit != r.resolved.end() ? hit->second.height : 0,
                hit != r.resolved.end() ? hit->second.destFormat : 0);
        }
        // A resolve target: guest memory there does not hold the image, the host copy does.
        // shortcut: matched by address and size only; entries are never invalidated.
        // SCDA_TRACE_STALE 19-map census (2026-10-29): no copy fetched after a CPU write in a later frame.
        if (auto res = r.resolved.find(t.physical); res != r.resolved.end() && t.width <= res->second.width && t.height <= res->second.height)
        {
            if (res->second.destFormat != t.format && TraceResolveOnce((1ull << 60) | (uint64_t(t.physical) << 8) | t.format))
                RuntimeLog("[resolve] fetch %08X fmt %u %ux%u hits resolve fmt %u %ux%u\n", t.physical, t.format, t.width, t.height,
                    res->second.destFormat, res->second.width, res->second.height);
            // SCDA_TRACE_STALE=1: the CPU wrote the destination since the resolve (once per frame and base):
            // a texture streamed into a former resolve target would still bind the stale copy.
            static const bool traceStale = getenv("SCDA_TRACE_STALE") != nullptr;
            if (traceStale && res->second.checkedAt != m_presented)
            {
                res->second.checkedAt = m_presented;
                const uint8_t* guest = PhysicalPtr(t.physical);
                const size_t n = size_t(res->second.rowBytes) * res->second.height;
                const uint64_t hash = GuestRangeReadable(guest, n) ? HashBytes(guest, n) : 0;
                static std::unordered_set<uint32_t> stillLogged;
                if (res->second.staleSince && res->second.staleSince - 1 < m_presented && stillLogged.insert(t.physical).second)
                    RuntimeLog("[stale] frame %llu fetch %08X still the copy made before frame %llu\n", m_presented, t.physical, res->second.staleSince - 1);
                if (hash != res->second.guestHash && !res->second.staleSince)
                    res->second.staleSince = m_presented + 1;
                if (hash != res->second.guestHash)
                    RuntimeLog("[stale] frame %llu fetch %08X fmt %u %ux%u tiled %d: guest bytes changed since resolve v%u fmt %u %ux%u\n", m_presented,
                        t.physical, t.format, t.width, t.height, t.tiled, res->second.version, res->second.destFormat, res->second.width, res->second.height);
                static uint32_t checks = 0;
                if (++checks % 5000 == 1)
                    RuntimeLog("[stale] check #%u frame %llu fetch %08X hash %016llx\n", checks, m_presented, t.physical, (unsigned long long)hash);
                res->second.guestHash = hash; // log each change once
            }
            srvs[slot] = ResolvedView(m_device, m_context, res->second, t.width, t.height);
            continue;
        }
        static const bool traceResolve = getenv("SCDA_TRACE_RESOLVE") != nullptr;
        if (traceResolve)
            for (auto& e : r.resolved)
                if (t.physical >= e.first && t.physical < e.first + e.second.rowBytes * e.second.height &&
                    TraceResolveOnce((2ull << 60) | ((m_presented & 0xFFFF) << 40) | (uint64_t(t.physical) << 8) | t.format))
                    RuntimeLog("[resolve] frame %llu fetch %08X fmt %u %ux%u tiled %d inside resolve %08X fmt %u %ux%u (+%u B), not used\n", m_presented, t.physical, t.format,
                        t.width, t.height, t.tiled, e.first, e.second.destFormat, e.second.width, e.second.height, t.physical - e.first);
        if (!TextureElementBytes(t.format))
        {
            // SCDA_DUMP_TEXTURES: each unsupported texture once, with the nearest resolve at or below it.
            static const bool log = getenv("SCDA_DUMP_TEXTURES") != nullptr;
            static std::unordered_set<uint64_t> logged;
            if (log && logged.insert((uint64_t(t.physical) << 8) | t.format).second && logged.size() < 500)
            {
                auto res = std::find_if(r.resolved.begin(), r.resolved.end(), [&](auto& e) { return e.first <= t.physical && t.physical < e.first + 0x01000000; });
                const bool hit = res != r.resolved.end();
                RuntimeLog("[tex] unsupported fmt %u phys %08X %ux%u tiled %d; resolve %08X %ux%u\n", t.format, t.physical, t.width, t.height, t.tiled,
                    hit ? res->first : 0, hit ? res->second.width : 0, hit ? res->second.height : 0);
            }
            continue;
        }
        uint64_t key = 1469598103934665603ull;
        for (uint32_t i = 0; i < 6; i++)
            key = (key ^ regs[kRegFetchConstants + slot * 6 + i]) * 1099511628211ull;
        auto it = r.textures.find(key);
        if (it == r.textures.end())
        {
            ComPtr<ID3D11ShaderResourceView> srv;
            const uint8_t* guest = PhysicalPtr(t.physical);
            auto& cached = r.textureCache[key];
            if (cached.levelBytes.empty())
            {
                cached.levelBytes.push_back(TextureGuestBytes(t));
                for (uint32_t level = 1; level <= t.mipMaxLevel && HasGuestMips(t); level++)
                    cached.levelBytes.push_back(TextureGuestBytes(TextureMipLevel(t, level)));
            }
            const uint32_t guestBytes = cached.levelBytes[0];
            static const bool noCache = getenv("SCDA_NO_TEXCACHE") != nullptr; // debug: convert every frame
            // A texture whose full hash held for kTexStableFrames presents is only probed (sparse
            // sample of level 0) per present; its full hash still runs at its key phase of every kTexFullEvery
            // presents (staggered), and at least once per kTexFullEvery presents. newyork_02 hashed ~115 MB per present before.
            // shortcut: a CPU write outside the probed bytes shows up to kTexFullEvery presents late.
            // SCDA_TEXHASH_COMPARE=1: always full-hash, log each change the probe alone would have missed.
            static const bool compare = getenv("SCDA_TEXHASH_COMPARE") != nullptr;
            const bool readable = GuestRangeReadable(guest, guestBytes);
            const uint64_t probe = readable ? ProbeBytes(guest, guestBytes) : 0;
            const bool probeOnly = cached.srv && !noCache && probe && probe == cached.probe && cached.stable >= kTexStableFrames &&
                m_presented < cached.fullAt + kTexFullEvery && (m_presented + (key * 0x9E3779B97F4A7C15ull >> 32)) % kTexFullEvery != 0;
            uint64_t hash = cached.hash;
            bool guestMips = false;
            g_texHash.probes++;
            g_texHash.skips += probeOnly;
            if (!probeOnly || compare)
            {
                hash = readable ? HashBytes(guest, guestBytes) : 0;
                g_texHash.bytes += guestBytes;
                // Authored mip levels (2D only): part of the hash; unreadable ones fall back to generated mips.
                guestMips = hash && t.mipMaxLevel && HasGuestMips(t);
                for (uint32_t level = 1; guestMips && level <= t.mipMaxLevel; level++)
                {
                    const uint8_t* g = PhysicalPtr(TextureMipLevel(t, level).physical);
                    const uint32_t n = cached.levelBytes[level];
                    guestMips = GuestRangeReadable(g, n);
                    if (guestMips)
                        hash = (hash ^ HashBytes(g, n)) * 1099511628211ull;
                    g_texHash.bytes += n;
                }
                if (probeOnly && hash != cached.hash)
                    RuntimeLog("[texhash] MISS present %llu fetch %08X fmt %u %ux%u tiled %d: guest bytes changed, probe unchanged (stable %u)\n",
                        m_presented, t.physical, t.format, t.width, t.height, t.tiled, cached.stable);
                if (!probeOnly || hash != cached.hash) // compare mode: keep the probe-only schedule
                {
                    cached.stable = hash && hash == cached.hash ? cached.stable + 1 : 0;
                    cached.probe = probe;
                    cached.fullAt = m_presented;
                }
            }
            cached.lastUsed = m_presented;
            if (hash && cached.srv && cached.hash == hash && !noCache)
                srv = cached.srv;
            else if (hash)
            {
                const bool isFloat = IsFloatTextureFormat(t.format);
                std::vector<float> floats = isFloat ? ConvertTextureFloat(t, guest) : std::vector<float>();
                std::vector<uint32_t> texels = isFloat ? std::vector<uint32_t>() : ConvertTexture(t, guest);
                if (isFloat && getenv("SCDA_DUMP_TEXTURES")) // RGBA8 copy for the dump only
                    for (size_t i = 0; i < floats.size(); i += 4)
                        texels.push_back(ToUnorm8(floats[i]) | (ToUnorm8(floats[i + 1]) << 8) | (ToUnorm8(floats[i + 2]) << 16) | (ToUnorm8(floats[i + 3]) << 24));
                // SCDA_DUMP_TEXTURES=<dir>: each converted texture once, as <physical>_<format>_<w>x<h>.bmp.
                static const char* dumpDir = getenv("SCDA_DUMP_TEXTURES");
                static std::unordered_set<uint64_t> dumped;
                if (dumpDir && dumped.insert(key).second && dumped.size() < 2000)
                {
                    char name[96];
                    snprintf(name, sizeof(name), "\\%08X_%u_%ux%u.bmp", t.physical, t.format, t.width, t.height);
                    WriteBmp(dumpDir + std::string(name), texels, t.width, t.height);
                    if ((t.offsetX | t.offsetY) && !isFloat) // packed-tail base: also as read from (0, 0)
                    {
                        TextureFetch origin = t;
                        origin.offsetX = origin.offsetY = 0;
                        WriteBmp(dumpDir + std::string(name).replace(strlen(name) - 4, 4, "_at0.bmp"), ConvertTexture(origin, guest), t.width, t.height);
                    }
                    uint32_t bytes = TextureGuestBytes(t), nonFF = 0;
                    for (uint32_t i = 0; i < bytes; i++)
                        nonFF += guest[i] != 0xFF;
                    RuntimeLog("[tex] %s fetch %08X %08X %08X %08X: %u of %u guest bytes not FF, first %02X%02X%02X%02X\n", name + 1,
                        regs[kRegFetchConstants + slot * 6], regs[kRegFetchConstants + slot * 6 + 1], regs[kRegFetchConstants + slot * 6 + 2],
                        regs[kRegFetchConstants + slot * 6 + 3], nonFF, bytes, guest[0], guest[1], guest[2], guest[3]);
                }
                D3D11_TEXTURE2D_DESC td{};
                td.Width = t.width;
                td.Height = t.height;
                td.MipLevels = td.ArraySize = 1;
                td.Format = isFloat ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_IMMUTABLE;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const void* data = isFloat ? (const void*)floats.data() : texels.data();
                const UINT rowPitch = t.width * (isFloat ? 16 : 4);
                D3D11_SUBRESOURCE_DATA init{ data, rowPitch, 0 };
                ComPtr<ID3D11Texture2D> tex;
                // Guest mip chain (dword 4 mip_max_level, levels at dword 5 mip_address): the authored
                // levels when readable as 2D, else generated on the GPU from level 0.
                const uint32_t levels = t.mipMaxLevel + 1;
                static const bool mipCheckBase = getenv("SCDA_MIP_CHECK") != nullptr;
                if (mipCheckBase && (t.offsetX | t.offsetY))
                    RuntimeLog("[mip] base %08X fmt %u %ux%u tiled %d at +(%u,%u) levels %u dword5 %08X\n", t.physical, t.format, t.width, t.height,
                        t.tiled, t.offsetX, t.offsetY, levels, regs[kRegFetchConstants + slot * 6 + 5]);
                if (guestMips)
                {
                    std::vector<std::vector<float>> mipFloats(levels);
                    std::vector<std::vector<uint32_t>> mipTexels(levels);
                    std::vector<D3D11_SUBRESOURCE_DATA> mipInit{ init };
                    static const bool mipCheck = getenv("SCDA_MIP_CHECK") != nullptr;
                    for (uint32_t level = 1; level < levels; level++)
                    {
                        const TextureFetch m = TextureMipLevel(t, level);
                        const uint8_t* g = PhysicalPtr(m.physical);
                        if (isFloat)
                            mipFloats[level] = ConvertTextureFloat(m, g);
                        else
                            mipTexels[level] = ConvertTexture(m, g);
                        mipInit.push_back({ isFloat ? (const void*)mipFloats[level].data() : mipTexels[level].data(), m.width * (isFloat ? 16 : 4), 0 });
                        // SCDA_MIP_CHECK: mean |authored - 2x2 box of the level above| per channel (0-255);
                        // a wrong layout reads unrelated bytes and scores far above a real downsample.
                        const std::vector<uint32_t>& up = level == 1 ? texels : mipTexels[level - 1];
                        const uint32_t uw = std::max(t.width >> (level - 1), 1u), uh = std::max(t.height >> (level - 1), 1u);
                        if (mipCheck && !isFloat && up.size() == size_t(uw) * uh)
                        {
                            uint64_t sum = 0, alphaSum = 0;
                            for (uint32_t y = 0; y < m.height; y++)
                                for (uint32_t x = 0; x < m.width; x++)
                                    for (uint32_t c = 0; c < 32; c += 8)
                                    {
                                        uint32_t box = 0;
                                        for (uint32_t k = 0; k < 4; k++)
                                            box += (up[size_t(std::min(y * 2 + k / 2, uh - 1)) * uw + std::min(x * 2 + k % 2, uw - 1)] >> c) & 0xFF;
                                        const uint32_t d = uint32_t(std::abs(int((mipTexels[level][size_t(y) * m.width + x] >> c) & 0xFF) - int(box / 4)));
                                        sum += d;
                                        alphaSum += c == 24 ? d : 0;
                                    }
                            RuntimeLog("[mip] %08X fmt %u %ux%u tiled %d packed %d level %u %ux%u at %08X +(%u,%u) diff %.1f alpha %.1f\n", t.physical,
                                t.format, t.width, t.height, t.tiled, t.packedMips, level, m.width, m.height, m.physical, m.offsetX, m.offsetY,
                                double(sum) / (m.width * m.height * 4.0), double(alphaSum) / (m.width * m.height));
                        }
                    }
                    td.MipLevels = levels;
                    if (SUCCEEDED(m_device->CreateTexture2D(&td, mipInit.data(), &tex)))
                        m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
                }
                else if (levels > 1)
                {
                    td.MipLevels = levels;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                    if (SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
                    {
                        m_context->UpdateSubresource(tex.Get(), 0, nullptr, data, rowPitch, 0);
                        m_context->GenerateMips(srv.Get());
                    }
                    else
                    {
                        tex.Reset();
                        srv.Reset();
                    }
                }
                if (!srv)
                {
                    td.MipLevels = 1;
                    td.Usage = D3D11_USAGE_IMMUTABLE;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    td.MiscFlags = 0;
                    if (SUCCEEDED(m_device->CreateTexture2D(&td, &init, &tex)))
                        m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
                }
                cached.srv = srv;
                cached.hash = hash;
            }
            it = r.textures.emplace(key, srv).first;
        }
        if (it->second)
            srvs[slot] = it->second.Get();
    }
    for (auto set : { &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::PSSetShaderResources })
        (m_context->*set)(kHlslTex2DSlot, 32, srvs);

    // Samplers s0-s15 from fetch constants 0-15 (the translator indexes samplers by fetch slot & 15).
    ID3D11SamplerState* samplers[16];
    for (uint32_t slot = 0; slot < 16; slot++)
    {
        samplers[slot] = r.sampler.Get();
        if (!((texSlots | texSlots >> 16) >> slot & 1)) // sampler = fetch slot & 15
            continue;
        HostSampler hs = DecodeSampler(regs, slot);
        // Anisotropy: the guest's, raised to the settings minimum for fully linear filtering.
        const uint32_t aniso = std::max(hs.anisotropy, hs.filter == 0x15 ? r.anisotropy : 1u);
        auto& state = r.samplers[(uint64_t(hs.filter) << 24) | (hs.addressU << 16) | (hs.addressV << 8) | hs.addressW |
            (uint64_t(hs.minLod) << 32) | (uint64_t(hs.maxLod) << 36) | (uint64_t(aniso) << 40)];
        if (!state)
        {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = aniso > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER(hs.filter);
            sd.MaxAnisotropy = aniso;
            sd.AddressU = D3D11_TEXTURE_ADDRESS_MODE(hs.addressU);
            sd.AddressV = D3D11_TEXTURE_ADDRESS_MODE(hs.addressV);
            sd.AddressW = D3D11_TEXTURE_ADDRESS_MODE(hs.addressW);
            sd.MinLOD = float(hs.minLod);
            sd.MaxLOD = float(hs.maxLod);
            m_device->CreateSamplerState(&sd, &state);
        }
        samplers[slot] = state ? state.Get() : r.sampler.Get();
    }
    m_context->VSSetSamplers(0, 16, samplers);
    m_context->PSSetSamplers(0, 16, samplers);
}

void D3D11Renderer::ExecuteDraw(const DrawCall& d, const Frame& frame)
{
    DrawResources& r = *m_draw;
    ApplyRegisterWrites(frame, d.regWritesEnd);
    uint32_t topology = HostTopology(d.primType);
    const bool rectList = d.primType == 8;
    // SCDA_SKIP_SHADER=<hex hash>[,..]: skip draws using that VS or PS (finding which draw makes a glitch
    // when draw order varies between runs).
    static const std::vector<uint64_t> skipShaders = []
    {
        std::vector<uint64_t> v;
        for (const char* p = getenv("SCDA_SKIP_SHADER"); p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] ? 1 : 0))
            v.push_back(strtoull(p, nullptr, 16));
        return v;
    }();
    const bool skipped = d.vs && d.ps &&
        std::find_if(skipShaders.begin(), skipShaders.end(), [&](uint64_t h) { return h == d.vs->hash || h == d.ps->hash; }) != skipShaders.end();
    if (skipped || !topology || !d.vs || !d.ps || d.source == IndexSource::Immediate || (rectList && !r.rectListGs))
    {
        m_lastSkipped++;
        return;
    }
    if (TraceResolveFrame(m_presented))
    {
        // Texture slots the PS samples: s<n>_Texture*DescriptorIndex uses outside the #define header.
        std::string used;
        const std::string hlsl = TranslateShader(*d.ps).hlsl;
        for (uint32_t n = 0; n < 32; n++)
        {
            const std::string name = "s" + std::to_string(n) + "_Texture";
            size_t count = 0;
            for (size_t at = hlsl.find(name); at != std::string::npos; at = hlsl.find(name, at + 1))
                count++;
            if (count > 3)
                used += " " + std::to_string(n);
        }
        RuntimeLog("[rtrace] draw %u vs %016llx ps %016llx rt %08X/%08X mask %08X zctl %08X zinfo %08X vte %08X zs %08X zo %08X wo %08X sc %08X cc %08X aref %08X ib %08X n %u samples%s\n",
            m_lastDrawn + m_lastSkipped, (unsigned long long)d.vs->hash, (unsigned long long)d.ps->hash, r.regs[kRegRbSurfaceInfo],
            r.regs[kRegRbColorInfo], r.regs[0x2104], r.regs[0x2200], r.regs[kRegRbDepthInfo], r.regs[0x2206], r.regs[0x2113],
            r.regs[0x2114], r.regs[0x2080], r.regs[0x2205], r.regs[0x2202], r.regs[0x210E], d.indexBase, d.indexCount, used.c_str());
        // SCDA_TRACE_PS=<ps hash hex>: draws with that PS also log PS c0-c7 and blend control.
        static const uint64_t tracePs = getenv("SCDA_TRACE_PS") ? strtoull(getenv("SCDA_TRACE_PS"), nullptr, 16) : 0;
        if (tracePs && d.ps->hash == tracePs)
            for (uint32_t c = 0; c < 8; c++)
            {
                auto f = [&](uint32_t i) { return std::bit_cast<float>(r.regs[kRegAluConstants + 1024 + c * 4 + i]); };
                RuntimeLog("[rtrace]   ps c%u %g %g %g %g blend %08X\n", c, f(0), f(1), f(2), f(3), r.regs[0x2201]);
            }
        // SCDA_TRACE_CONSTS=<index base hex>: draws with that index base also log their vertex/index
        // snapshot hashes, draw state, PS c0, bool and vertex fetch constants and VS c0-c255.
        static const uint32_t constsIb = getenv("SCDA_TRACE_CONSTS") ? strtoul(getenv("SCDA_TRACE_CONSTS"), nullptr, 16) : 0;
        if (constsIb && d.indexBase == constsIb)
        {
            auto hash = [&](int32_t m)
            {
                uint64_t h = 1469598103934665603ull;
                if (m >= 0 && size_t(m) < frame.memory.size())
                    for (uint8_t b : frame.memory[m].bytes)
                        h = (h ^ b) * 1099511628211ull;
                return (unsigned long long)h;
            };
            for (uint32_t m : d.memory)
                RuntimeLog("[rtrace]   snap %u key %016llx size %zu hash %016llx\n", m, (unsigned long long)frame.memory[m].key,
                    frame.memory[m].bytes.size(), hash(int32_t(m)));
            RuntimeLog("[rtrace]   idx %d hash %016llx prim %u src %d i32 %d endian %u vgt %08X %08X %08X %08X colorctl %08X aref %08X mode %08X\n",
                d.indexSnapshot, hash(d.indexSnapshot), d.primType, int(d.source), int(d.index32), d.indexEndian, r.regs[0x2100],
                r.regs[0x2101], r.regs[0x2102], r.regs[0x2103], r.regs[0x2202], r.regs[0x210E], r.regs[kRegRbModeControl]);
            RuntimeLog("[rtrace]   psc0 %08X %08X %08X %08X psc255 %08X bool %08X %08X\n", r.regs[kRegAluConstants + 1024],
                r.regs[kRegAluConstants + 1025], r.regs[kRegAluConstants + 1026], r.regs[kRegAluConstants + 1027],
                r.regs[kRegAluConstants + 1024 + 255 * 4], r.regs[kRegBoolConstants], r.regs[kRegBoolConstants + 4]);
            for (uint32_t c = 0; c < 192; c += 6)
                if ((r.regs[kRegFetchConstants + c] & 3) == 3 || (r.regs[kRegFetchConstants + c + 2] & 3) == 3 || (r.regs[kRegFetchConstants + c + 4] & 3) == 3)
                    RuntimeLog("[rtrace]   fetch%u %08X %08X %08X %08X %08X %08X\n", c / 6, r.regs[kRegFetchConstants + c],
                        r.regs[kRegFetchConstants + c + 1], r.regs[kRegFetchConstants + c + 2], r.regs[kRegFetchConstants + c + 3],
                        r.regs[kRegFetchConstants + c + 4], r.regs[kRegFetchConstants + c + 5]);
            for (uint32_t c = 0; c < 256; c++)
                RuntimeLog("[rtrace]   c%u %08X %08X %08X %08X\n", c, r.regs[kRegAluConstants + c * 4], r.regs[kRegAluConstants + c * 4 + 1],
                    r.regs[kRegAluConstants + c * 4 + 2], r.regs[kRegAluConstants + c * 4 + 3]);
        }
    }
    static const std::filesystem::path cacheDir = DefaultShaderCacheDir();
    auto& vs = r.vs[d.vs->hash];
    if (!vs)
    {
        const CompiledShader& c = GetCompiledShader(*d.vs, cacheDir);
        if (c.ok)
            m_device->CreateVertexShader(c.bytecode.data(), c.bytecode.size(), nullptr, &vs);
    }
    auto& ps = r.ps[d.ps->hash];
    if (!ps)
    {
        const CompiledShader& c = GetCompiledShader(*d.ps, cacheDir);
        if (c.ok)
            m_device->CreatePixelShader(c.bytecode.data(), c.bytecode.size(), nullptr, &ps);
    }
    if (!vs || !ps)
    {
        m_lastSkipped++;
        return;
    }
    const uint32_t texSlots = ShaderTextureFetchSlots(*d.vs) | ShaderTextureFetchSlots(*d.ps);
    if (r.stateDirty || (texSlots & ~r.lastTexSlots))
    {
        r.lastTexSlots = r.stateDirty ? texSlots : r.lastTexSlots | texSlots;
        BindRegisterState(r.regs, r.lastTexSlots);
        r.stateDirty = false;
    }
    UploadVertexMemory(d, frame);
    m_context->VSSetShader(vs.Get(), nullptr, 0);
    // RB_MODECONTROL 5 (depth only): the pixel shader does not run, so its kills (a masked prepass
    // PS left bound with another draw's texture) must not drop depth (Xenia xenos::ModeControl).
    // SCDA_DEPTH_ONLY_PS=1 runs it anyway (A/B).
    static const bool depthOnlyPs = getenv("SCDA_DEPTH_ONLY_PS") != nullptr;
    const bool depthOnly = (r.regs[kRegRbModeControl] & 7) == 5 && !depthOnlyPs;
    m_context->PSSetShader(depthOnly ? nullptr : ps.Get(), nullptr, 0);
    m_context->GSSetShader(rectList ? r.rectListGs.Get() : nullptr, nullptr, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(topology));
    const bool expand = NeedsTriangleListExpansion(d.primType);
    if (d.source == IndexSource::Dma || expand)
    {
        std::vector<uint8_t>& indices = r.indexScratch;
        indices.clear();
        uint32_t count = d.indexCount;
        bool wide = d.index32;
        if (d.source == IndexSource::Dma)
        {
            if (d.indexSnapshot < 0 || size_t(d.indexSnapshot) >= frame.memory.size())
            {
                m_lastSkipped++;
                return;
            }
            indices.resize(d.indexBytes);
            SwapIndices(frame.memory[d.indexSnapshot].bytes.data(), d.indexBytes, d.indexEndian, indices.data());
            count = std::min(d.indexCount, d.indexBytes / (d.index32 ? 4 : 2));
        }
        if (expand)
        {
            std::vector<uint32_t> list(count);
            for (uint32_t i = 0; i < count; i++)
                if (indices.empty())
                    list[i] = i;
                else if (d.index32)
                    memcpy(&list[i], &indices[i * 4], 4);
                else
                    list[i] = uint16_t(indices[i * 2] | (indices[i * 2 + 1] << 8));
            list = ExpandToTriangleList(d.primType, list);
            count = uint32_t(list.size());
            indices.resize(list.size() * 4);
            memcpy(indices.data(), list.data(), indices.size());
            wide = true;
        }
        if (!count)
        {
            m_lastSkipped++;
            return;
        }
        const uint32_t bytes = (uint32_t(indices.size()) + 3) & ~3u;
        if (!r.indexRing)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16u << 20;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (SUCCEEDED(m_device->CreateBuffer(&bd, nullptr, &r.indexRing)))
                r.indexRingBytes = bd.ByteWidth;
        }
        if (bytes <= r.indexRingBytes)
        {
            const bool wrap = r.indexRingPos + bytes > r.indexRingBytes;
            D3D11_MAPPED_SUBRESOURCE map;
            if (FAILED(m_context->Map(r.indexRing.Get(), 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &map)))
            {
                m_lastSkipped++;
                return;
            }
            const uint32_t offset = wrap ? 0 : r.indexRingPos;
            memcpy(static_cast<uint8_t*>(map.pData) + offset, indices.data(), indices.size());
            m_context->Unmap(r.indexRing.Get(), 0);
            r.indexRingPos = offset + bytes;
            m_context->IASetIndexBuffer(r.indexRing.Get(), wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, offset);
        }
        else
        {
            // Larger than the ring: one immutable buffer for this draw.
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = uint32_t(indices.size());
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init{ indices.data(), 0, 0 };
            ComPtr<ID3D11Buffer> ib;
            if (FAILED(m_device->CreateBuffer(&bd, &init, &ib)))
            {
                m_lastSkipped++;
                return;
            }
            m_context->IASetIndexBuffer(ib.Get(), wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
        }
        m_context->DrawIndexed(count, 0, 0);
    }
    else
        m_context->Draw(d.indexCount, 0);
    m_lastDrawn++;
}

// EDRAM resolve: copy the source surface to the host texture for the destination, then clear it.
void D3D11Renderer::ExecuteResolve(const Resolve& res)
{
    DrawResources& r = *m_draw;
    ColorTarget* source = ColorTargetFor(m_device, m_context, r, EdramColorKey(res.colorInfo, res.surfaceInfo));
    if (TraceResolveFrame(m_presented))
        RuntimeLog("[rtrace] resolve @draw %u ctrl %08X src %08X surf %08X -> dest %08X fmt %u %ux%u rect %d %d %d %d\n", res.drawIndex, res.copyControl,
            res.colorInfo, res.surfaceInfo, res.destBase, (res.destInfo >> 7) & 0x3F, ResolveWidth(res), ResolveHeight(res), res.rect[0], res.rect[1],
            res.rect[2], res.rect[3]);
    // shortcut: depth-source resolves copy nothing (the destination keeps its last copy, if any).
    if (ResolveCopies(res) && source && !ResolveFromDepth(res))
    {
        // The resolve rect goes to the same position in the destination. A destination inside an
        // earlier resolve's texture (predicated tiling: one 32-row-aligned base per screen tile)
        // is written into that texture at the row the base advanced to.
        // shortcut: the destination keeps the surface's host format; rows only, no x offset in the base.
        // shortcut: resolved entries are never invalidated, so a stale texture can capture a later base.
        D3D11_TEXTURE2D_DESC sd;
        source->texture->GetDesc(&sd);
        const uint32_t base = res.destBase & 0x1FFFF000;
        if ((res.destBase & 0xFFF) && TraceResolveOnce((3ull << 60) | res.destBase))
            RuntimeLog("[resolve] dest %08X has sub-page offset %03X (dropped)\n", res.destBase, res.destBase & 0xFFF);
        if (TraceResolveOnce((4ull << 60) | res.colorInfo | (uint64_t(res.surfaceInfo & 0x3FFF) << 32)))
            RuntimeLog("[resolve] source info %08X surf %08X from_depth %d -> dest %08X info %08X fmt %u %ux%u\n", res.colorInfo, res.surfaceInfo,
                ResolveFromDepth(res), res.destBase, res.destInfo, (res.destInfo >> 7) & 0x3F, ResolveWidth(res), ResolveHeight(res));
        uint32_t dy = 0;
        auto dstIt = r.resolved.end();
        for (auto it = r.resolved.begin(); it != r.resolved.end(); ++it)
            if (it->first < base && it->second.rowBytes && it->second.format == sd.Format && it->second.width == ResolveWidth(res) &&
                (base - it->first) % it->second.rowBytes == 0 && (base - it->first) / it->second.rowBytes < it->second.height)
            {
                dstIt = it;
                dy = (base - it->first) / it->second.rowBytes;
                break;
            }
            else if (it->first < base && base < it->first + it->second.rowBytes * it->second.height &&
                TraceResolveOnce((5ull << 60) | (uint64_t(base) << 20) | ((it->first >> 12) & 0xFFFFF)))
                RuntimeLog("[resolve] dest %08X fmt %u w %u inside resolve %08X fmt %u w %u rowBytes %u (+%u B) rejected: new texture\n", base,
                    (res.destInfo >> 7) & 0x3F, ResolveWidth(res), it->first, it->second.destFormat, it->second.width, it->second.rowBytes, base - it->first);
        if (dstIt == r.resolved.end())
        {
            uint32_t w = std::min(ResolveWidth(res), kTargetWidth), h = std::min(ResolveHeight(res), kTargetHeight);
            auto& dst = r.resolved[base];
            if (w && h && (dst.width != w || dst.height != h || dst.format != sd.Format))
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w * r.scale;
                td.Height = h * r.scale;
                td.MipLevels = td.ArraySize = 1;
                td.Format = sd.Format;
                td.SampleDesc.Count = 1;
                const bool floatTarget = sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && sd.Format != DXGI_FORMAT_R10G10B10A2_UNORM;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (floatTarget ? D3D11_BIND_UNORDERED_ACCESS : 0);
                dst = {};
                if (SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &dst.texture)) &&
                    SUCCEEDED(m_device->CreateShaderResourceView(dst.texture.Get(), nullptr, &dst.srv)) &&
                    (!floatTarget || SUCCEEDED(m_device->CreateUnorderedAccessView(dst.texture.Get(), nullptr, &dst.uav))))
                {
                    dst.width = w;
                    dst.height = h;
                    dst.format = sd.Format;
                }
            }
            dst.rowBytes = w * TextureElementBytes((res.destInfo >> 7) & 0x3F);
            dst.destFormat = (res.destInfo >> 7) & 0x3F;
            dstIt = r.resolved.find(base);
        }
        auto& dst = dstIt->second;
        if (!dst.srv)
            r.resolved.erase(dstIt);
        else
        {
            // Source rect: the resolve draw's, else the whole destination from the surface origin.
            int32_t x0 = 0, y0 = 0, x1 = int32_t(dst.width), y1 = int32_t(dst.height - dy);
            if (res.hasRect)
            {
                x0 = std::max(res.rect[0], 0);
                y0 = std::max(res.rect[1], 0);
                x1 = std::min(res.rect[2], x1);
                y1 = std::min(res.rect[3], y1);
            }
            x1 = std::min(x1, int32_t(kTargetWidth));
            y1 = std::min(y1, int32_t(kTargetHeight));
            if (x1 > x0 && y1 > y0)
            {
                // Host texels from here on.
                const int32_t s = int32_t(r.scale);
                x0 *= s, y0 *= s, x1 *= s, y1 *= s;
                const uint32_t hostDy = dy * r.scale;
                const uint32_t destFormat = (res.destInfo >> 7) & 0x3F;
                if (dst.uav && r.saturateCs && !IsFloatTextureFormat(destFormat))
                {
                    // The source is bound as a render target: unbind it before reading it.
                    m_context->OMSetRenderTargets(0, nullptr, nullptr);
                    r.bound = false;
                    const int32_t rect[8] = { x0, y0, x1, y1, int32_t(hostDy) };
                    m_context->UpdateSubresource(r.saturateRect.Get(), 0, nullptr, rect, 0, 0);
                    ID3D11ShaderResourceView* srv = source->srv.Get();
                    ID3D11UnorderedAccessView* uav = dst.uav.Get();
                    m_context->CSSetShader(r.saturateCs.Get(), nullptr, 0);
                    m_context->CSSetConstantBuffers(0, 1, r.saturateRect.GetAddressOf());
                    m_context->CSSetShaderResources(0, 1, &srv);
                    m_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
                    m_context->Dispatch(UINT(x1 - x0 + 7) / 8, UINT(y1 - y0 + 7) / 8, 1);
                    srv = nullptr;
                    uav = nullptr;
                    m_context->CSSetShaderResources(0, 1, &srv);
                    m_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
                }
                else
                {
                    D3D11_BOX box{ UINT(x0), UINT(y0), 0, UINT(x1), UINT(y1), 1 };
                    m_context->CopySubresourceRegion(dst.texture.Get(), 0, UINT(x0), UINT(y0) + hostDy, 0, source->texture.Get(), 0, &box);
                }
                m_lastResolved++;
                dst.version++;
                static const bool traceStale = getenv("SCDA_TRACE_STALE") != nullptr;
                if (traceStale)
                {
                    const uint8_t* guest = PhysicalPtr(dstIt->first);
                    const size_t n = size_t(dst.rowBytes) * dst.height;
                    dst.guestHash = GuestRangeReadable(guest, n) ? HashBytes(guest, n) : 0;
                    if (dst.staleSince)
                        RuntimeLog("[stale] frame %llu resolve %08X refreshes copy stale since frame %llu\n", m_presented, dstIt->first, dst.staleSince - 1);
                    dst.staleSince = 0;
                }
                // SCDA_DUMP_RESOLVES=<dir>: on a SCDA_DUMP_FRAME frame, each resolve's destination after the copy.
                static const char* dumpResolves = getenv("SCDA_DUMP_RESOLVES");
                if (dumpResolves && !m_dumpPath.empty())
                {
                    char name[96];
                    snprintf(name, sizeof(name), "/res_%02u_%08X_y%u.bmp", m_lastResolved, base, dy);
                    RuntimeLog("[resolve] dump #%u src fmt %u dest fmt %u\n", m_lastResolved, (res.colorInfo >> 16) & 0xF, (res.destInfo >> 7) & 0x3F);
                    Dump(std::string(dumpResolves) + name, dst.texture.Get());
                }
            }
        }
    }
    if (source)
        r.last = source;
    if (ResolveClearsColor(res) && source)
    {
        const uint32_t c = res.colorClear;
        if (TraceResolveOnce((5ull << 60) | res.colorInfo | (uint64_t(c) << 20)))
            RuntimeLog("[resolve] clear src %08X surf %08X value %08X\n", res.colorInfo, res.surfaceInfo, c);
        float color[4];
        ColorClearValue((res.colorInfo >> 16) & 0xF, c, res.colorClearLo, color);
        m_context->ClearRenderTargetView(source->rtv.Get(), color);
    }
    if (ResolveClearsDepth(res))
        if (ID3D11DepthStencilView* dsv = DepthTargetFor(m_device, m_context, r, EdramDepthKey(res.depthInfo, res.surfaceInfo)))
            m_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, ResolveDepthClearValue(res), UINT8(res.depthClear & 0xFF));
    r.stateDirty = true; // texture bindings may now point at a new resolve
}

void D3D11Renderer::Blit(ID3D11ShaderResourceView* source)
{
    DrawResources& r = *m_draw;
    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(0, 1, &none);
    m_context->OMSetRenderTargets(1, &m_backBuffer, nullptr);
    r.bound = false;
    D3D11_VIEWPORT vp{ 0, 0, float(m_width), float(m_height), 0, 1 };
    m_context->RSSetViewports(1, &vp);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->RSSetState(r.rasterizer.Get());
    m_context->VSSetShader(r.blitVs.Get(), nullptr, 0);
    m_context->GSSetShader(nullptr, nullptr, 0);
    m_context->PSSetShader(r.blitPs.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* s = r.sampler.Get();
    m_context->PSSetSamplers(0, 1, &s);
    m_context->Draw(3, 0);
    m_context->PSSetShaderResources(0, 1, &none);
}

void D3D11Renderer::Present(const Frame& frame)
{
    // Lost back buffer (failed resize) or device: rebuild everything for the current size.
    if (!m_backBuffer)
    {
        ReleaseAll();
        if (!m_hwnd || !Init(m_hwnd, m_width, m_height, m_vsync))
            return;
    }
    m_lastDrawn = m_lastSkipped = m_lastResolved = 0;
    m_lastFrontbuffer = false;
    if (TraceResolveFrame(m_presented))
        RuntimeLog("[rtrace] present #%llu guest frame %llu\n", (unsigned long long)m_presented, (unsigned long long)frame.stats.frame);
    // No draws: a dark colour that pulses with the frame number, so a running game is visibly
    // distinct from a hung one.
    float pulse = float(frame.stats.frame % 120) / 120.0f;
    const float idle[4] = { 0.02f, 0.05f + 0.10f * pulse, 0.02f, 1.0f };
    if (!m_draw)
    {
        m_context->OMSetRenderTargets(1, &m_backBuffer, nullptr);
        m_context->ClearRenderTargetView(m_backBuffer, idle);
    }
    else
    {
        DrawResources& r = *m_draw;
        if (g_resetRequested.exchange(false))
        {
            m_context->ClearState();
            r.colorTargets.clear();
            r.depthTargets.clear();
            r.resolved.clear();
            r.textureCache.clear();
            r.last = nullptr;
            r.stateDirty = true;
            r.aluDirty[0] = r.aluDirty[1] = true;
        }
        // EDRAM surfaces persist across frames; only guest clears (resolves) clear them.
        r.bound = false;
        D3D11_VIEWPORT vp{ 0, 0, float(kTargetWidth * r.scale), float(kTargetHeight * r.scale), 0, 1 };
        m_context->RSSetViewports(1, &vp);
        m_context->RSSetState(r.rasterizer.Get());
        m_context->IASetInputLayout(nullptr);
        ID3D11SamplerState* samplers[16];
        for (auto& s : samplers)
            s = r.sampler.Get();
        m_context->VSSetSamplers(0, 16, samplers);
        m_context->PSSetSamplers(0, 16, samplers);
        ID3D11Buffer* cbs[3] = { r.constants[0].Get(), r.constants[1].Get(), r.constants[2].Get() };
        m_context->VSSetConstantBuffers(0, 3, cbs);
        m_context->PSSetConstantBuffers(0, 3, cbs);
        r.stateDirty = true;
        r.applied = 0;
        // Draws and resolves in stream order: resolve k runs before draw resolves[k].drawIndex.
        // SCDA_DRAW_LIMIT=<n>: run only draws [0, n) (and the resolves before them), then show the
        // last RT0 instead of the frontbuffer (bisecting which pass draws what).
        static const size_t drawLimit = getenv("SCDA_DRAW_LIMIT") ? size_t(atoll(getenv("SCDA_DRAW_LIMIT"))) : SIZE_MAX;
        size_t next = 0;
        for (size_t i = 0; i <= std::min(frame.draws.size(), drawLimit); i++)
        {
            for (; next < frame.resolves.size() && frame.resolves[next].drawIndex <= i; next++)
                ExecuteResolve(frame.resolves[next]);
            if (i < std::min(frame.draws.size(), drawLimit))
                ExecuteDraw(frame.draws[i], frame);
            // SCDA_DUMP_DRAWS=<dir>,<step>[,<first>,<last>]: on a SCDA_DUMP_FRAME frame, RT0 after every
            // step-th draw in [first, last] (bisecting which draw writes what in one run).
            static const char* dumpDraws = getenv("SCDA_DUMP_DRAWS");
            static size_t step = 1, first = 0, last = SIZE_MAX;
            static const bool parsed = [&]
            {
                if (const char* c = dumpDraws ? strchr(dumpDraws, ',') : nullptr)
                    sscanf(c + 1, "%zu,%zu,%zu", &step, &first, &last);
                step = std::max<size_t>(step, 1);
                return true;
            }();
            (void)parsed;
            if (dumpDraws && !m_dumpPath.empty() && r.last && i % step == 0 && i >= first && i <= last)
            {
                char name[64];
                snprintf(name, sizeof(name), "/draw_%04zu.bmp", i);
                Dump(std::string(dumpDraws, strcspn(dumpDraws, ",")) + name, r.last->texture.Get());
                // SCDA_DUMP_DEPTH=1: also the bound depth target (z_<n>.bmp, see TexelToRgba8).
                static const bool dumpDepth = getenv("SCDA_DUMP_DEPTH") != nullptr;
                auto z = r.bound ? r.depthTargets.find(r.boundKeys[4]) : r.depthTargets.end();
                if (dumpDepth && z != r.depthTargets.end())
                {
                    ComPtr<ID3D11Resource> res;
                    ComPtr<ID3D11Texture2D> tex;
                    z->second->GetResource(&res);
                    if (SUCCEEDED(res.As(&tex)))
                    {
                        snprintf(name, sizeof(name), "/z_%04zu.bmp", i);
                        Dump(std::string(dumpDraws, strcspn(dumpDraws, ",")) + name, tex.Get());
                    }
                }
            }
        }
        ApplyRegisterWrites(frame, frame.regWrites.size()); // draws past SCDA_DRAW_LIMIT still wrote registers
        r.uploaded.clear();
        // SCDA_TRACE_TEXHASH=1: texture checks and fully hashed guest bytes, every 100 presents.
        static const bool traceTexHash = getenv("SCDA_TRACE_TEXHASH") != nullptr;
        if (traceTexHash && m_presented % 100 == 0)
            RuntimeLog("[texhash] present %llu: %u checks, %u probe-only, %.1f MB hashed\n", m_presented, g_texHash.probes, g_texHash.skips, g_texHash.bytes / 1048576.0);
        g_texHash = {};
        r.textures.clear();
        if (m_presented % 60 == 0) // drop textures unused for ~10 s
            std::erase_if(r.textureCache, [&](auto& e) { return e.second.lastUsed + 600 < m_presented; });
        ID3D11ShaderResourceView* none[32] = {};
        m_context->VSSetShaderResources(0, 32, none);
        m_context->PSSetShaderResources(0, 32, none);
        auto front = frame.frontbuffer ? r.resolved.find(frame.frontbuffer & 0x1FFFF000) : r.resolved.end();
        m_lastFrontbuffer = front != r.resolved.end() && drawLimit == SIZE_MAX;
        // No frontbuffer resolve: the last RT0 (or the idle colour when nothing was drawn yet).
        ID3D11Texture2D* shown = m_lastFrontbuffer ? front->second.texture.Get() : r.last ? r.last->texture.Get() : nullptr;
        ID3D11ShaderResourceView* shownSrv = m_lastFrontbuffer ? front->second.srv.Get() : r.last ? r.last->srv.Get() : nullptr;
        if (!m_dumpPath.empty() && shown)
        {
            Dump(m_dumpPath, shown);
            m_dumpPath.clear();
        }
        if (shownSrv)
            Blit(shownSrv);
        else
        {
            m_context->OMSetRenderTargets(1, &m_backBuffer, nullptr);
            m_context->ClearRenderTargetView(m_backBuffer, idle);
        }
    }
    HRESULT hr = m_swapChain->Present(m_vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        printf("[d3d11] device lost (0x%08lX, reason 0x%08lX): recreating\n", (unsigned long)hr, (unsigned long)m_device->GetDeviceRemovedReason());
        Release(m_backBuffer); // next Present rebuilds the device
        return;
    }
    m_presented++;
    ComPtr<ID3D11InfoQueue> queue;
    if (getenv("SCDA_D3D_DEBUG") && SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&queue))))
    {
        for (UINT64 i = 0, n = queue->GetNumStoredMessages(); i < n && i < 16; i++)
        {
            SIZE_T size = 0;
            queue->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> buf(size);
            auto* msg = (D3D11_MESSAGE*)buf.data();
            if (SUCCEEDED(queue->GetMessage(i, msg, &size)))
                RuntimeLog("[d3d11] debug: %.*s\n", int(msg->DescriptionByteLength), msg->pDescription);
        }
        queue->ClearStoredMessages();
    }
}

// One host texel as RGBA8 (float formats clamped to [0, 1]; R32 / R32G32 as grey / red-green).
static uint32_t TexelToRgba8(DXGI_FORMAT format, const uint8_t* p)
{
    float c[4] = { 0, 0, 0, 1 };
    if (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) // depth dumps: unorm24 depth in RGB (R high), stencil in A
    {
        float z;
        memcpy(&z, p, 4);
        const uint32_t d = uint32_t(std::clamp(z, 0.0f, 1.0f) * 16777215.0f + 0.5f);
        return (d >> 16) | (d & 0xFF00) | ((d & 0xFF) << 16) | (uint32_t(p[4]) << 24);
    }
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (int i = 0; i < 4; i++)
        {
            uint16_t h;
            memcpy(&h, p + 2 * i, 2);
            c[i] = HalfToFloat(h);
        }
        break;
    case DXGI_FORMAT_R32_FLOAT:
        memcpy(c, p, 4);
        c[1] = c[2] = c[0];
        break;
    case DXGI_FORMAT_R32G32_FLOAT:
        memcpy(c, p, 8);
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    {
        uint32_t t;
        memcpy(&t, p, 4);
        for (int i = 0; i < 3; i++)
            c[i] = ((t >> (10 * i)) & 0x3FF) / 1023.0f;
        c[3] = (t >> 30) / 3.0f;
        break;
    }
    default: // RGBA8
    {
        uint32_t t;
        memcpy(&t, p, 4);
        return t;
    }
    }
    return ToUnorm8(c[0]) | (ToUnorm8(c[1]) << 8) | (ToUnorm8(c[2]) << 16) | (ToUnorm8(c[3]) << 24);
}

// Copies the render target to a CPU-readable texture; texels row by row (RGBA8).
static std::vector<uint32_t> ReadTarget(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* target)
{
    D3D11_TEXTURE2D_DESC td;
    target->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint32_t> out;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging)))
        return out;
    context->CopyResource(staging.Get(), target);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return out;
    const uint32_t bpp = td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || td.Format == DXGI_FORMAT_R32G32_FLOAT ||
        td.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8 : 4;
    out.resize(size_t(td.Width) * td.Height);
    // SCDA_RT_STATS=1: log the FP16 maxima (rgb, alpha) of each target read back (dumps squash to 8-bit).
    static const bool stats = getenv("SCDA_RT_STATS") != nullptr;
    if (stats && td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        float maxRgb = 0, maxA = 0;
        for (uint32_t y = 0; y < td.Height; y++)
            for (uint32_t x = 0; x < td.Width * 4; x++)
            {
                uint16_t h;
                memcpy(&h, (uint8_t*)m.pData + size_t(y) * m.RowPitch + size_t(x) * 2, 2);
                float& mx = (x & 3) == 3 ? maxA : maxRgb;
                mx = std::max(mx, HalfToFloat(h));
            }
        RuntimeLog("[rtstats] %ux%u fp16 max rgb %.3f alpha %.3f\n", td.Width, td.Height, maxRgb, maxA);
    }
    for (uint32_t y = 0; y < td.Height; y++)
        for (uint32_t x = 0; x < td.Width; x++)
            out[size_t(y) * td.Width + x] = TexelToRgba8(td.Format, (uint8_t*)m.pData + size_t(y) * m.RowPitch + size_t(x) * bpp);
    context->Unmap(staging.Get(), 0);
    return out;
}

uint32_t D3D11Renderer::ReadTargetTexel(uint32_t x, uint32_t y)
{
    if (!m_draw || !m_draw->last || x >= kTargetWidth || y >= kTargetHeight)
        return 0;
    std::vector<uint32_t> texels = ReadTarget(m_device, m_context, m_draw->last->texture.Get());
    const uint32_t s = m_draw->scale;
    return texels.empty() ? 0 : texels[size_t(y * s) * kTargetWidth * s + x * s];
}

void D3D11Renderer::Dump(const std::string& path, ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC td;
    source->GetDesc(&td);
    if (WriteBmp(path, ReadTarget(m_device, m_context, source), td.Width, td.Height))
        printf("[d3d11] frame dumped to %s\n", path.c_str());
}
