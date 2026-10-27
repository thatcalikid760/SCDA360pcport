#include <gpu/d3d11_renderer.h>

template <class T> static void Release(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

D3D11Renderer::~D3D11Renderer()
{
    Release(m_backBuffer);
    Release(m_swapChain);
    Release(m_context);
    Release(m_device);
}

bool D3D11Renderer::Init(HWND hwnd, uint32_t width, uint32_t height, bool vsync)
{
    m_vsync = vsync;
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
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, &sd, &m_swapChain, &m_device, nullptr, &m_context);
    if (FAILED(hr))
        return false;
    CreateBackBufferView();
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

void D3D11Renderer::Resize(uint32_t width, uint32_t height)
{
    if (!m_swapChain || !width || !height)
        return;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    Release(m_backBuffer);
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateBackBufferView();
}

void D3D11Renderer::Present(const Frame& frame)
{
    if (!m_backBuffer)
        return;
    // Placeholder until 2b-3 draws: a dark colour that pulses with the frame number, so a
    // running game is visibly distinct from a hung one.
    float pulse = float(frame.stats.frame % 120) / 120.0f;
    const float color[4] = { 0.02f, 0.05f + 0.10f * pulse, 0.02f, 1.0f };
    m_context->OMSetRenderTargets(1, &m_backBuffer, nullptr);
    m_context->ClearRenderTargetView(m_backBuffer, color);
    m_swapChain->Present(m_vsync ? 1 : 0, 0);
    m_presented++;
}
