#include <algorithm>
#include <bitset>
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
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }
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

// Device objects for executing draws; rebuilt with the device.
struct DrawResources
{
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> targetRtv;
    ComPtr<ID3D11ShaderResourceView> targetSrv;
    // Depth/stencil for the target. Float depth holds both guest formats (D24S8, D24FS8).
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> depthDsv;
    float depthClear = 0.0f; // last guest depth clear, reused by the per-frame clear
    ComPtr<ID3D11VertexShader> blitVs;
    ComPtr<ID3D11PixelShader> blitPs;
    ComPtr<ID3D11GeometryShader> rectListGs;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants[3]; // b0 VS, b1 PS, b2 shared
    ComPtr<ID3D11ShaderResourceView> white;
    // Raw view of guest physical memory from address 0 (t96); grown on demand.
    ComPtr<ID3D11Buffer> memory;
    ComPtr<ID3D11ShaderResourceView> memorySrv;
    uint32_t memoryBytes = 0;
    std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> vs;
    std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> ps;
    // Fixed-function state by the registers it is built from.
    std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blend;
    std::unordered_map<uint32_t, ComPtr<ID3D11RasterizerState>> raster;
    std::unordered_map<uint32_t, ComPtr<ID3D11DepthStencilState>> depthStates;
    std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> samplers;
    // Per frame: guest ranges already uploaded, textures by fetch constant.
    std::unordered_map<uint64_t, uint32_t> uploaded; // range key -> Frame::memory snapshot in the buffer
    std::unordered_map<uint64_t, ComPtr<ID3D11ShaderResourceView>> textures;
    const RegisterFile* lastRegs = nullptr;
    // Resolve copies of the target by guest physical destination; kept across frames (the
    // frontbuffer and render-to-texture results are read after the frame that made them).
    struct Resolved
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t width = 0, height = 0;
    };
    std::unordered_map<uint32_t, Resolved> resolved;
};

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

bool D3D11Renderer::Init(HWND hwnd, uint32_t width, uint32_t height, bool vsync)
{
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
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kTargetWidth;
    td.Height = kTargetHeight;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &r->target)) ||
        FAILED(m_device->CreateRenderTargetView(r->target.Get(), nullptr, &r->targetRtv)) ||
        FAILED(m_device->CreateShaderResourceView(r->target.Get(), nullptr, &r->targetSrv)))
        return false;
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(m_device->CreateTexture2D(&dd, nullptr, &r->depth)) ||
        FAILED(m_device->CreateDepthStencilView(r->depth.Get(), nullptr, &r->depthDsv)))
        return false;

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

static const uint8_t* PhysicalPtr(uint32_t physical) { return (const uint8_t*)GuestPtr(GuestPhysical(physical)); }

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

void D3D11Renderer::BindRegisterState(const RegisterFile& regs)
{
    DrawResources& r = *m_draw;
    uint32_t alu[1024];
    CopyAluConstants(regs, false, alu);
    m_context->UpdateSubresource(r.constants[0].Get(), 0, nullptr, alu, 0, 0);
    CopyAluConstants(regs, true, alu);
    m_context->UpdateSubresource(r.constants[1].Get(), 0, nullptr, alu, 0, 0);
    SharedConstants shared = BuildSharedConstants(regs, kTargetWidth, kTargetHeight);
    m_context->UpdateSubresource(r.constants[2].Get(), 0, nullptr, &shared, 0, 0);

    HostBlend b = DecodeBlend(regs);
    if ((regs[kRegRbModeControl] & 7) == 5) // depth-only pass (Z prepass, occlusion queries)
        b.writeMask = 0;
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
    auto& depth = r.depthStates[uint32_t(z.depthEnable) | (uint32_t(z.depthWrite) << 1) | (z.depthFunc << 2)];
    if (!depth)
    {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = z.depthEnable;
        dd.DepthWriteMask = z.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_FUNC(z.depthFunc);
        // shortcut: stencil off; RB_DEPTHCONTROL stencil func/ops and RB_STENCILREFMASK are not decoded.
        dd.StencilEnable = FALSE;
        m_device->CreateDepthStencilState(&dd, &depth);
    }
    m_context->OMSetDepthStencilState(depth.Get(), 0);

    // Textures: every texture fetch constant the stages might sample (the shader's slots are not
    // known here), converted on the CPU once per frame.
    ID3D11ShaderResourceView* srvs[32];
    for (uint32_t slot = 0; slot < 32; slot++)
    {
        srvs[slot] = r.white.Get();
        TextureFetch t = DecodeTextureFetch(regs, slot);
        if (!t.valid)
            continue;
        // A resolve target: guest memory there does not hold the image, the host copy does.
        // shortcut: matched by address and size only; entries are never invalidated.
        if (auto res = r.resolved.find(t.physical); res != r.resolved.end() && t.width <= res->second.width && t.height <= res->second.height)
        {
            srvs[slot] = res->second.srv.Get();
            continue;
        }
        if (!TextureElementBytes(t.format))
            continue;
        uint64_t key = 1469598103934665603ull;
        for (uint32_t i = 0; i < 6; i++)
            key = (key ^ regs[kRegFetchConstants + slot * 6 + i]) * 1099511628211ull;
        auto it = r.textures.find(key);
        if (it == r.textures.end())
        {
            ComPtr<ID3D11ShaderResourceView> srv;
            const uint8_t* guest = PhysicalPtr(t.physical);
            if (GuestRangeReadable(guest, TextureGuestBytes(t)))
            {
                std::vector<uint32_t> texels = ConvertTexture(t, guest);
                // SCDA_DUMP_TEXTURES=<dir>: each converted texture once, as <physical>_<format>_<w>x<h>.bmp.
                static const char* dumpDir = getenv("SCDA_DUMP_TEXTURES");
                static std::unordered_set<uint64_t> dumped;
                if (dumpDir && dumped.insert(key).second && dumped.size() < 2000)
                {
                    char name[96];
                    snprintf(name, sizeof(name), "\\%08X_%u_%ux%u.bmp", t.physical, t.format, t.width, t.height);
                    WriteBmp(dumpDir + std::string(name), texels, t.width, t.height);
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
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_IMMUTABLE;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA init{ texels.data(), t.width * 4, 0 };
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(m_device->CreateTexture2D(&td, &init, &tex)))
                    m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
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
        HostSampler hs = DecodeSampler(regs, slot);
        auto& state = r.samplers[(uint64_t(hs.filter) << 24) | (hs.addressU << 16) | (hs.addressV << 8) | hs.addressW];
        if (!state)
        {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER(hs.filter);
            sd.AddressU = D3D11_TEXTURE_ADDRESS_MODE(hs.addressU);
            sd.AddressV = D3D11_TEXTURE_ADDRESS_MODE(hs.addressV);
            sd.AddressW = D3D11_TEXTURE_ADDRESS_MODE(hs.addressW);
            sd.MaxLOD = D3D11_FLOAT32_MAX;
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
    uint32_t topology = HostTopology(d.primType);
    const bool rectList = d.primType == 8;
    if (!topology || !d.vs || !d.ps || !d.regs || d.source == IndexSource::Immediate || (rectList && !r.rectListGs))
    {
        m_lastSkipped++;
        return;
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
    if (d.regs.get() != r.lastRegs)
    {
        BindRegisterState(*d.regs);
        r.lastRegs = d.regs.get();
    }
    UploadVertexMemory(d, frame);
    m_context->VSSetShader(vs.Get(), nullptr, 0);
    m_context->PSSetShader(ps.Get(), nullptr, 0);
    m_context->GSSetShader(rectList ? r.rectListGs.Get() : nullptr, nullptr, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(topology));
    if (d.source == IndexSource::Dma)
    {
        if (d.indexSnapshot < 0 || size_t(d.indexSnapshot) >= frame.memory.size())
        {
            m_lastSkipped++;
            return;
        }
        // shortcut: one immutable index buffer per draw.
        std::vector<uint8_t> indices(d.indexBytes);
        SwapIndices(frame.memory[d.indexSnapshot].bytes.data(), d.indexBytes, d.indexEndian, indices.data());
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = d.indexBytes;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{ indices.data(), 0, 0 };
        ComPtr<ID3D11Buffer> ib;
        if (FAILED(m_device->CreateBuffer(&bd, &init, &ib)))
        {
            m_lastSkipped++;
            return;
        }
        m_context->IASetIndexBuffer(ib.Get(), d.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
        m_context->DrawIndexed(std::min(d.indexCount, d.indexBytes / (d.index32 ? 4 : 2)), 0, 0);
    }
    else
        m_context->Draw(d.indexCount, 0);
    m_lastDrawn++;
}

// EDRAM resolve: copy the target to the host texture for the destination, then clear it.
void D3D11Renderer::ExecuteResolve(const Resolve& res)
{
    DrawResources& r = *m_draw;
    if (ResolveCopies(res))
    {
        // shortcut: the whole destination from the target origin; the resolve draw's rect and the
        // window offset are not applied, and every format is kept as RGBA8.
        uint32_t w = std::min(ResolveWidth(res), kTargetWidth), h = std::min(ResolveHeight(res), kTargetHeight);
        auto& dst = r.resolved[res.destBase & 0x1FFFF000];
        if (w && h && (dst.width != w || dst.height != h))
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            dst = {};
            if (SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &dst.texture)) &&
                SUCCEEDED(m_device->CreateShaderResourceView(dst.texture.Get(), nullptr, &dst.srv)))
            {
                dst.width = w;
                dst.height = h;
            }
        }
        if (!dst.srv)
            r.resolved.erase(res.destBase & 0x1FFFF000);
        else
        {
            D3D11_BOX box{ 0, 0, 0, w, h, 1 };
            m_context->CopySubresourceRegion(dst.texture.Get(), 0, 0, 0, 0, r.target.Get(), 0, &box);
            m_lastResolved++;
        }
    }
    if (ResolveClearsColor(res))
    {
        const uint32_t c = res.colorClear;
        const float color[4] = { (c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        m_context->ClearRenderTargetView(r.targetRtv.Get(), color);
    }
    if (ResolveClearsDepth(res))
    {
        r.depthClear = ResolveDepthClearValue(res);
        m_context->ClearDepthStencilView(r.depthDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, r.depthClear, UINT8(res.depthClear & 0xFF));
    }
    r.lastRegs = nullptr; // texture bindings may now point at a new resolve
}

void D3D11Renderer::Blit(ID3D11ShaderResourceView* source)
{
    DrawResources& r = *m_draw;
    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(0, 1, &none);
    m_context->OMSetRenderTargets(1, &m_backBuffer, nullptr);
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
    // No draws: a dark colour that pulses with the frame number, so a running game is visibly
    // distinct from a hung one.
    float pulse = float(frame.stats.frame % 120) / 120.0f;
    const float idle[4] = { 0.02f, 0.05f + 0.10f * pulse, 0.02f, 1.0f }, black[4] = { 0, 0, 0, 1 };
    if (!m_draw)
    {
        m_context->OMSetRenderTargets(1, &m_backBuffer, nullptr);
        m_context->ClearRenderTargetView(m_backBuffer, idle);
    }
    else
    {
        DrawResources& r = *m_draw;
        // shortcut: cleared every frame; guest clears (resolve with clear) are not emulated yet.
        m_context->ClearRenderTargetView(r.targetRtv.Get(), frame.draws.empty() ? idle : black);
        m_context->ClearDepthStencilView(r.depthDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, r.depthClear, 0);
        ID3D11RenderTargetView* rtv = r.targetRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, r.depthDsv.Get());
        D3D11_VIEWPORT vp{ 0, 0, float(kTargetWidth), float(kTargetHeight), 0, 1 };
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
        r.lastRegs = nullptr;
        // Draws and resolves in stream order: resolve k runs before draw resolves[k].drawIndex.
        size_t next = 0;
        for (size_t i = 0; i <= frame.draws.size(); i++)
        {
            for (; next < frame.resolves.size() && frame.resolves[next].drawIndex <= i; next++)
                ExecuteResolve(frame.resolves[next]);
            if (i < frame.draws.size())
                ExecuteDraw(frame.draws[i], frame);
        }
        r.uploaded.clear();
        r.textures.clear();
        ID3D11ShaderResourceView* none[32] = {};
        m_context->VSSetShaderResources(0, 32, none);
        m_context->PSSetShaderResources(0, 32, none);
        auto front = frame.frontbuffer ? r.resolved.find(frame.frontbuffer & 0x1FFFF000) : r.resolved.end();
        m_lastFrontbuffer = front != r.resolved.end();
        if (!m_dumpPath.empty())
        {
            Dump(m_dumpPath, m_lastFrontbuffer ? front->second.texture.Get() : r.target.Get());
            m_dumpPath.clear();
        }
        Blit(m_lastFrontbuffer ? front->second.srv.Get() : r.targetSrv.Get());
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
    out.resize(size_t(td.Width) * td.Height);
    for (uint32_t y = 0; y < td.Height; y++)
        memcpy(&out[size_t(y) * td.Width], (uint8_t*)m.pData + size_t(y) * m.RowPitch, td.Width * 4);
    context->Unmap(staging.Get(), 0);
    return out;
}

uint32_t D3D11Renderer::ReadTargetTexel(uint32_t x, uint32_t y)
{
    if (!m_draw || x >= kTargetWidth || y >= kTargetHeight)
        return 0;
    std::vector<uint32_t> texels = ReadTarget(m_device, m_context, m_draw->target.Get());
    return texels.empty() ? 0 : texels[size_t(y) * kTargetWidth + x];
}

void D3D11Renderer::Dump(const std::string& path, ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC td;
    source->GetDesc(&td);
    if (WriteBmp(path, ReadTarget(m_device, m_context, source), td.Width, td.Height))
        printf("[d3d11] frame dumped to %s\n", path.c_str());
}
