// NoTimeFbx: minimal, fast-starting FBX viewer. Rotate with LMB, zoom with wheel, Esc to quit.
//
// Environment knobs, for diagnostics: NOTIMEFBX_TIMING=1 shows startup timings in the title,
// NOTIMEFBX_LOG=1 writes %TEMP%\NoTimeFbx.log, NOTIMEFBX_NOCACHE=1 bypasses the mesh cache,
// NOTIMEFBX_RENDERER=gpu|warp forces the renderer, NOTIMEFBX_WARP_MAX_TRIS sets the largest model
// drawn with WARP.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <imm.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mesh.h"
#include "texture.h"

#include "shaders_vs.h"
#include "shaders_ps.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>

bool register_association(const wchar_t* exePath);   // assoc.cpp
bool unregister_association();
bool is_setup_exe();                                  // install.cpp
int  run_install();
int  run_uninstall();

// ---------------------------------------------------------------------------
// Math (column-vector convention, column-major storage: m[col * 4 + row])

struct Mat4 { float m[16]; };

static Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static Mat4 look_at_rh(Vec3 eye, Vec3 target, Vec3 up)
{
    Vec3 z = normalize(eye - target);
    Vec3 x = normalize(cross(up, z));
    Vec3 y = cross(z, x);
    Mat4 r = {};
    r.m[0] = x.x; r.m[4] = x.y; r.m[8]  = x.z; r.m[12] = -dot(x, eye);
    r.m[1] = y.x; r.m[5] = y.y; r.m[9]  = y.z; r.m[13] = -dot(y, eye);
    r.m[2] = z.x; r.m[6] = z.y; r.m[10] = z.z; r.m[14] = -dot(z, eye);
    r.m[15] = 1;
    return r;
}

static Mat4 perspective_rh(float fovy, float aspect, float zn, float zf)
{
    float f = 1.0f / std::tan(fovy * 0.5f);
    Mat4 r = {};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1;
    r.m[14] = zn * zf / (zn - zf);
    return r;
}

// ---------------------------------------------------------------------------
// Startup timing (shown in the title when NOTIMEFBX_TIMING is set)

static LARGE_INTEGER g_freq, g_t0;
static wchar_t g_marks[320];

static double ms_since_start()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (t.QuadPart - g_t0.QuadPart) * 1000.0 / g_freq.QuadPart;
}

static void append_mark(const wchar_t* text)   // UI thread only
{
    size_t len = wcslen(g_marks);
    _snwprintf_s(g_marks + len, _countof(g_marks) - len, _TRUNCATE, L"%s", text);
}

static void mark(const wchar_t* name, double ms)
{
    wchar_t buf[48];
    swprintf(buf, 48, L" %s=%.0f", name, ms);
    append_mark(buf);
}

static void mark(const wchar_t* name) { mark(name, ms_since_start()); }

// Time from process creation to wWinMain: image loading, DLL imports, CRT init.
static double premain_ms()
{
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    uint64_t a = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
    uint64_t b = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return (b - a) / 10000.0;
}

// ---------------------------------------------------------------------------
// Renderer selection
//
// WARP (Microsoft's software D3D11 rasterizer) is ready ~5 ms after launch, while loading the GPU
// driver takes ~140 ms. WARP draws up to about 150k triangles at 60 fps even in a maximized
// 2560x1440 window, so light models use it and never load the GPU driver; heavier ones use the GPU.
// If WARP still turns out too slow (big window, slow CPU), the model moves to the GPU on the fly.

static uint64_t warp_max_triangles()
{
    static const uint64_t value = [] {
        wchar_t v[32] = {};
        if (GetEnvironmentVariableW(L"NOTIMEFBX_WARP_MAX_TRIS", v, 32)) return (uint64_t)_wtoi64(v);
        return (uint64_t)150000;
    }();
    return value;
}

enum class Renderer { Auto, Gpu, Warp };

static Renderer renderer_override()
{
    wchar_t v[16] = {};
    GetEnvironmentVariableW(L"NOTIMEFBX_RENDERER", v, 16);
    if (!_wcsicmp(v, L"gpu")) return Renderer::Gpu;
    if (!_wcsicmp(v, L"warp")) return Renderer::Warp;
    return Renderer::Auto;
}

// Triangle count guess before loading, to start the right device early. Exact for cached meshes;
// otherwise from the file size. Bytes per triangle measured on real files: binary 65-130,
// ASCII 150-400. The low ends are used, so the guess errs on the heavy side.
static uint64_t estimate_triangles(const wchar_t* path)
{
    uint64_t tris;
    if (cache_peek(path, tris)) return tris;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size = {};
    char head[7] = {};
    DWORD got = 0;
    GetFileSizeEx(f, &size);
    ReadFile(f, head, sizeof(head), &got, nullptr);
    CloseHandle(f);
    bool binary = got == sizeof(head) && memcmp(head, "Kaydara", 7) == 0;   // "Kaydara FBX Binary"
    return (uint64_t)size.QuadPart / (binary ? 60 : 140);
}

// ---------------------------------------------------------------------------
// Renderer state

struct FrameConstants
{
    Mat4  viewProj;
    Vec3  lightDir;
    float pad;
};

struct MaterialConstants
{
    Vec3  color;
    float hasTexture;
    float alphaTest;
    float pad[3];
};

struct GpuMaterial
{
    Vec3 color;
    int  texture;     // index into g_textures, -1 if none
    bool alphaTest;
};

template <class T> static void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Everything tied to one D3D device. create_gpu() is thread-safe; afterwards the UI thread owns it.
struct Gpu
{
    ID3D11Device*          device = nullptr;
    ID3D11DeviceContext*   ctx = nullptr;
    ID3D11VertexShader*    vs = nullptr;
    ID3D11PixelShader*     ps = nullptr;
    ID3D11InputLayout*     layout = nullptr;
    ID3D11Buffer*          frameCB = nullptr;
    ID3D11Buffer*          materialCB = nullptr;
    ID3D11SamplerState*    sampler = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11Query*           frameQuery = nullptr;   // WARP only: measures how long a frame takes
    bool                   warp = false;
    double                 readyMs = 0;   // when creation finished, ms since launch (diagnostics)
};

static void release_gpu(Gpu& g)
{
    safe_release(g.frameQuery);
    safe_release(g.raster);
    safe_release(g.sampler);
    safe_release(g.materialCB);
    safe_release(g.frameCB);
    safe_release(g.layout);
    safe_release(g.ps);
    safe_release(g.vs);
    safe_release(g.ctx);
    safe_release(g.device);
}

static bool create_gpu(bool warp, Gpu& g)
{
    g = Gpu();
    g.warp = warp;
    D3D_DRIVER_TYPE type = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 1, D3D11_SDK_VERSION, &g.device,
                                   nullptr, &g.ctx);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))   // debug layer not installed
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, levels, 1,
                               D3D11_SDK_VERSION, &g.device, nullptr, &g.ctx);
    if (FAILED(hr)) return false;

    g.device->CreateVertexShader(g_vs_main, sizeof(g_vs_main), nullptr, &g.vs);
    g.device->CreatePixelShader(g_ps_main, sizeof(g_ps_main), nullptr, &g.ps);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g.device->CreateInputLayout(layout, 3, g_vs_main, sizeof(g_vs_main), &g.layout);

    D3D11_BUFFER_DESC cd = {};
    cd.ByteWidth = sizeof(FrameConstants);
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.device->CreateBuffer(&cd, nullptr, &g.frameCB);
    cd.ByteWidth = sizeof(MaterialConstants);
    g.device->CreateBuffer(&cd, nullptr, &g.materialCB);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.device->CreateSamplerState(&sd, &g.sampler);

    // FBX winding is not reliable, so draw both sides; the pixel shader flips back-face normals.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = TRUE;   // FBX uses CCW winding for front faces
    rd.DepthClipEnable = TRUE;
    g.device->CreateRasterizerState(&rd, &g.raster);

    if (warp) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        g.device->CreateQuery(&qd, &g.frameQuery);
    }

    g.readyMs = ms_since_start();
    if (g.vs && g.ps && g.layout && g.frameCB && g.materialCB && g.sampler && g.raster) return true;
    release_gpu(g);
    return false;
}

// Creates a Gpu on a background thread. take() waits for it; abandon() (also done by the
// destructor) hands it to whichever side finishes last for release, so nobody waits for a
// device that is no longer needed.
class AsyncGpu
{
public:
    AsyncGpu() = default;
    AsyncGpu(const AsyncGpu&) = delete;
    AsyncGpu& operator=(const AsyncGpu&) = delete;
    ~AsyncGpu() { abandon(); }

    // With `after`, creation begins only once that device is finished: D3D11 creates devices one at
    // a time, and the GPU driver's ~140 ms would otherwise hold up WARP's ~5 ms.
    void start(bool warp, const AsyncGpu* after = nullptr)
    {
        abandon();
        auto s = std::make_shared<State>();
        std::shared_ptr<State> prev = after ? after->m_state : nullptr;
        m_state = s;
        std::thread([s, prev, warp] {
            if (prev) {
                std::unique_lock<std::mutex> lock(prev->mutex);
                prev->cv.wait(lock, [&] { return prev->finished; });
            }
            Gpu g;
            bool ok = create_gpu(warp, g);
            std::lock_guard<std::mutex> lock(s->mutex);
            s->finished = true;
            if (s->abandoned) {
                release_gpu(g);
            } else {
                s->gpu = g;
                s->ok = ok;
            }
            s->cv.notify_all();
        }).detach();
    }

    bool started() const { return m_state != nullptr; }

    bool take(Gpu& out)
    {
        std::shared_ptr<State> s = std::move(m_state);
        if (!s) return false;
        std::unique_lock<std::mutex> lock(s->mutex);
        s->cv.wait(lock, [&] { return s->finished; });
        out = s->gpu;
        s->gpu = Gpu();
        return s->ok;
    }

    void abandon()
    {
        std::shared_ptr<State> s = std::move(m_state);
        if (!s) return;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->finished) release_gpu(s->gpu);
        else s->abandoned = true;
    }

private:
    struct State
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false, abandoned = false, ok = false;
        Gpu gpu;
    };
    std::shared_ptr<State> m_state;
};

static HWND                     g_hwnd;
static Gpu                      g_gpu;       // active renderer (UI thread only)
static std::atomic<bool>        g_onWarp;    // g_gpu.warp, readable from worker threads
static IDXGISwapChain1*         g_swapchain;
static HANDLE                   g_frameWaitable;   // signaled when the swap chain can take a new frame
static ID3D11RenderTargetView*  g_rtv;
static ID3D11DepthStencilView*  g_dsv;
static ID3D11Buffer*            g_vb;
static ID3D11Buffer*            g_ib;
static UINT                     g_indexCount;
static DXGI_FORMAT              g_indexFormat;
static UINT                     g_width, g_height;

static std::vector<Draw>                      g_draws;
static std::vector<GpuMaterial>               g_materials;
static std::vector<ID3D11ShaderResourceView*> g_textures;
static std::vector<bool>                      g_textureCutout;
static uint32_t                               g_textureGeneration;   // bumps on every new model

// Orbit camera
static const float kFovY = 0.8f;
static Vec3  g_target{ 0, 0, 0 };
static Vec3  g_bmin{ 0, 0, 0 }, g_bmax{ 0, 0, 0 };
static float g_radius = 1.0f;
static float g_yaw = 0.6f, g_pitch = 0.4f, g_dist = 3.0f;
static bool  g_dragging;
static POINT g_lastMouse;

static void fatal(const wchar_t* msg)
{
    MessageBoxW(g_hwnd, msg, L"NoTime Fbx", MB_ICONERROR);
    ExitProcess(1);
}

static void create_targets()
{
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_gpu.device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = g_width;
    dd.Height = g_height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    g_gpu.device->CreateTexture2D(&dd, nullptr, &depth);
    g_gpu.device->CreateDepthStencilView(depth, nullptr, &g_dsv);
    depth->Release();
}

static void create_swapchain()
{
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    g_gpu.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Three buffers: with two, the one to draw into is still on screen until the next vsync.
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(factory->CreateSwapChainForHwnd(g_gpu.device, g_hwnd, &sd, nullptr, nullptr, &g_swapchain)))
        fatal(L"Failed to create swap chain.");
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    adapter->Release();
    dxgiDevice->Release();

    // Wait for vsync before a frame rather than inside it: lower input latency, and the WARP frame
    // time measurement then covers only the actual rendering.
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(g_swapchain->QueryInterface(IID_PPV_ARGS(&sc2)))) {
        sc2->SetMaximumFrameLatency(1);
        g_frameWaitable = sc2->GetFrameLatencyWaitableObject();
        sc2->Release();
    }

    DXGI_SWAP_CHAIN_DESC1 actual;
    g_swapchain->GetDesc1(&actual);
    g_width = actual.Width;
    g_height = actual.Height;
    create_targets();
}

// Makes `gpu` the active renderer (taking ownership) and creates its swap chain. Everything that
// lived on the previous device is released; the caller uploads the model afterwards.
static void activate_gpu(Gpu& gpu)
{
    if (g_gpu.ctx) g_gpu.ctx->ClearState();
    safe_release(g_rtv);
    safe_release(g_dsv);
    if (g_frameWaitable) {
        CloseHandle(g_frameWaitable);
        g_frameWaitable = nullptr;
    }
    safe_release(g_swapchain);
    safe_release(g_vb);
    safe_release(g_ib);
    g_indexCount = 0;
    for (ID3D11ShaderResourceView*& srv : g_textures) safe_release(srv);
    // D3D11 destroys objects lazily; flush so the window's old flip-model swap chain is really gone
    // before a new one is created for it.
    if (g_gpu.ctx) g_gpu.ctx->Flush();
    release_gpu(g_gpu);

    g_gpu = gpu;
    gpu = Gpu();
    g_onWarp = g_gpu.warp;
    create_swapchain();
}

// ---------------------------------------------------------------------------
// Switching from WARP to the GPU while a model is displayed

static const UINT WM_APP_GPU_READY = WM_APP + 3;   // lParam: Gpu* (owned by the receiver)

static bool g_upgradeStarted;     // GPU requested once (or failed): never retried
static const UINT_PTR kProbeTimer = 1;

// Copies an immutable buffer from the active device to `dst` through a staging copy.
static ID3D11Buffer* copy_buffer(ID3D11Buffer* src, ID3D11Device* dst)
{
    if (!src) return nullptr;
    D3D11_BUFFER_DESC desc;
    src->GetDesc(&desc);
    D3D11_BUFFER_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Buffer* staging = nullptr;
    ID3D11Buffer* out = nullptr;
    if (FAILED(g_gpu.device->CreateBuffer(&sd, nullptr, &staging))) return nullptr;
    g_gpu.ctx->CopyResource(staging, src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_gpu.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        D3D11_SUBRESOURCE_DATA init = { m.pData };
        dst->CreateBuffer(&desc, &init, &out);
        g_gpu.ctx->Unmap(staging, 0);
    }
    staging->Release();
    return out;
}

// Same for a texture with its whole mip chain.
static ID3D11ShaderResourceView* copy_texture(ID3D11ShaderResourceView* src, ID3D11Device* dst)
{
    if (!src) return nullptr;
    ID3D11Resource* res = nullptr;
    ID3D11Texture2D* tex = nullptr;
    src->GetResource(&res);
    res->QueryInterface(IID_PPV_ARGS(&tex));
    res->Release();
    if (!tex) return nullptr;

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    ID3D11ShaderResourceView* out = nullptr;
    if (SUCCEEDED(g_gpu.device->CreateTexture2D(&sd, nullptr, &staging))) {
        g_gpu.ctx->CopyResource(staging, tex);
        std::vector<D3D11_SUBRESOURCE_DATA> init(desc.MipLevels);
        UINT mapped = 0;
        for (; mapped < desc.MipLevels; ++mapped) {   // one array slice: subresource index = mip level
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_gpu.ctx->Map(staging, mapped, D3D11_MAP_READ, 0, &m))) break;
            init[mapped] = { m.pData, m.RowPitch, 0 };
        }
        ID3D11Texture2D* copy = nullptr;
        if (mapped == desc.MipLevels && SUCCEEDED(dst->CreateTexture2D(&desc, init.data(), &copy))) {
            dst->CreateShaderResourceView(copy, nullptr, &out);
            copy->Release();
        }
        for (UINT i = 0; i < mapped; ++i) g_gpu.ctx->Unmap(staging, i);
        staging->Release();
    }
    tex->Release();
    return out;
}

// Moves the displayed model (buffers and the textures loaded so far) to `gpu` and makes it active.
// Textures still being decoded are created on the new device when they arrive.
static void switch_to(Gpu& gpu)
{
    ID3D11Buffer* vb = copy_buffer(g_vb, gpu.device);
    ID3D11Buffer* ib = copy_buffer(g_ib, gpu.device);
    std::vector<ID3D11ShaderResourceView*> textures(g_textures.size(), nullptr);
    for (size_t i = 0; i < textures.size(); ++i) textures[i] = copy_texture(g_textures[i], gpu.device);
    if ((g_vb && !vb) || (g_ib && !ib)) {   // could not move the model: stay where we are
        safe_release(vb);
        safe_release(ib);
        for (ID3D11ShaderResourceView*& t : textures) safe_release(t);
        release_gpu(gpu);
        return;
    }
    UINT indexCount = g_indexCount;
    activate_gpu(gpu);
    g_vb = vb;
    g_ib = ib;
    g_indexCount = indexCount;
    g_textures = std::move(textures);
}

static void start_gpu_upgrade(double frameMs)
{
    if (g_upgradeStarted || renderer_override() == Renderer::Warp) return;
    g_upgradeStarted = true;
    debug_log(L"WARP too slow (%.1f ms per frame): switching to the GPU", frameMs);
    std::thread([] {
        Gpu* g = new Gpu();
        if (create_gpu(false, *g) && PostMessageW(g_hwnd, WM_APP_GPU_READY, 0, (LPARAM)g)) return;
        release_gpu(*g);
        delete g;
    }).detach();
}

// Blocks until the device has executed everything submitted so far.
static void wait_for(ID3D11DeviceContext* ctx, ID3D11Query* eventQuery)
{
    ctx->End(eventQuery);
    while (ctx->GetData(eventQuery, nullptr, 0, 0) == S_FALSE) SwitchToThread();
}

static void upload_mesh(const Mesh& mesh)
{
    safe_release(g_vb);
    safe_release(g_ib);
    g_indexCount = (UINT)mesh.indices.size();
    if (!g_indexCount) return;

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = (UINT)(mesh.vertices.size() * sizeof(Vertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { mesh.vertices.data() };
    g_gpu.device->CreateBuffer(&bd, &init, &g_vb);

    // Use 16-bit indices when possible: half the memory and bandwidth.
    std::vector<uint16_t> idx16;
    if (mesh.vertices.size() <= 0xFFFF) {
        idx16.resize(mesh.indices.size());
        for (size_t i = 0; i < idx16.size(); ++i) idx16[i] = (uint16_t)mesh.indices[i];
        g_indexFormat = DXGI_FORMAT_R16_UINT;
        bd.ByteWidth = (UINT)(idx16.size() * sizeof(uint16_t));
        init.pSysMem = idx16.data();
    } else {
        g_indexFormat = DXGI_FORMAT_R32_UINT;
        bd.ByteWidth = (UINT)(mesh.indices.size() * sizeof(uint32_t));
        init.pSysMem = mesh.indices.data();
    }
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    g_gpu.device->CreateBuffer(&bd, &init, &g_ib);

    g_draws = mesh.draws;
    g_materials.clear();
    for (const Material& m : mesh.materials)
        g_materials.push_back({ m.color, m.texture, m.alphaTest });
    for (ID3D11ShaderResourceView*& srv : g_textures) safe_release(srv);
    g_textures.assign(mesh.textures.size(), nullptr);
    g_textureCutout.assign(mesh.textures.size(), false);

    // Frame the model.
    g_target = (mesh.bmin + mesh.bmax) * 0.5f;
    g_radius = std::fmax(std::sqrt(dot(mesh.bmax - mesh.bmin, mesh.bmax - mesh.bmin)) * 0.5f, 1e-4f);
    g_bmin = mesh.bmin;
    g_bmax = mesh.bmax;
}

static Vec3 view_dir()   // from target towards the camera
{
    return { std::cos(g_pitch) * std::sin(g_yaw), std::sin(g_pitch), std::cos(g_pitch) * std::cos(g_yaw) };
}

// Pick the camera distance so that all 8 bounding-box corners are inside the frame.
static void fit_camera()
{
    Vec3 d = view_dir();
    Vec3 right = normalize(cross({ 0, 1, 0 }, d));
    Vec3 up = cross(d, right);
    float tanV = std::tan(kFovY * 0.5f);
    float tanH = tanV * (g_height ? (float)g_width / g_height : 1.6f);
    float dist = 0;
    for (int i = 0; i < 8; ++i) {
        Vec3 c = { (i & 1) ? g_bmax.x : g_bmin.x, (i & 2) ? g_bmax.y : g_bmin.y, (i & 4) ? g_bmax.z : g_bmin.z };
        Vec3 rel = c - g_target;
        float z = dot(rel, d);
        dist = std::fmax(dist, z + std::fabs(dot(rel, right)) / tanH);
        dist = std::fmax(dist, z + std::fabs(dot(rel, up)) / tanV);
    }
    g_dist = std::fmax(dist * 1.08f, g_radius * 0.05f);
}

// Draws the model with the current camera into the given targets.
static void draw_scene(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, UINT width, UINT height)
{
    ID3D11DeviceContext* ctx = g_gpu.ctx;

    Vec3 offset = view_dir();
    Vec3 eye = g_target + offset * g_dist;
    float zn = std::fmax(g_dist - g_radius * 1.5f, g_dist * 0.01f);
    float zf = g_dist + g_radius * 1.5f;

    FrameConstants fc;
    fc.viewProj = mul(perspective_rh(kFovY, (float)width / height, zn, zf), look_at_rh(eye, g_target, { 0, 1, 0 }));
    fc.lightDir = normalize(offset + Vec3{ 0, 0.5f, 0 });   // headlight, slightly from above
    fc.pad = 0;

    D3D11_MAPPED_SUBRESOURCE ms;
    ctx->Map(g_gpu.frameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &fc, sizeof(fc));
    ctx->Unmap(g_gpu.frameCB, 0);

    const float clear[4] = { 0.16f, 0.17f, 0.19f, 1.0f };
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetRenderTargets(1, &rtv, dsv);

    D3D11_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_gpu.raster);

    if (g_indexCount) {
        UINT stride = sizeof(Vertex), offs = 0;
        ctx->IASetInputLayout(g_gpu.layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offs);
        ctx->IASetIndexBuffer(g_ib, g_indexFormat, 0);
        ctx->VSSetShader(g_gpu.vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &g_gpu.frameCB);
        ctx->PSSetShader(g_gpu.ps, nullptr, 0);
        ID3D11Buffer* psBuffers[2] = { g_gpu.frameCB, g_gpu.materialCB };
        ctx->PSSetConstantBuffers(0, 2, psBuffers);
        ctx->PSSetSamplers(0, 1, &g_gpu.sampler);

        for (const Draw& d : g_draws) {
            const GpuMaterial& m = g_materials[d.material];
            ID3D11ShaderResourceView* srv = m.texture >= 0 ? g_textures[m.texture] : nullptr;

            MaterialConstants mc = {};
            mc.color = m.color;
            mc.hasTexture = srv ? 1.0f : 0.0f;
            mc.alphaTest = srv && (m.alphaTest || g_textureCutout[m.texture]) ? 1.0f : 0.0f;
            ctx->Map(g_gpu.materialCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
            memcpy(ms.pData, &mc, sizeof(mc));
            ctx->Unmap(g_gpu.materialCB, 0);

            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->DrawIndexed(d.indexCount, d.firstIndex, 0);
        }
    }
}

// WARP renders on the CPU, so its frame time grows with triangle count and window size. Shortly
// after the model, its textures or the window size change, time one frame drawn off-screen
// (frames on screen are paced by the compositor's vsync, so they can't be timed). If WARP is too
// slow for smooth rotation, move the model to the GPU: the driver loads in the background while
// WARP keeps drawing, usually before the user even starts rotating.
static void probe_warp_speed()
{
    const double kBudgetMs = 12.0;   // leaves headroom within a 60 Hz frame
    if (g_upgradeStarted || !g_indexCount || renderer_override() == Renderer::Warp) return;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_width;
    td.Height = g_height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* color = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    g_gpu.device->CreateTexture2D(&td, nullptr, &color);
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    g_gpu.device->CreateTexture2D(&td, nullptr, &depth);
    if (color) g_gpu.device->CreateRenderTargetView(color, nullptr, &rtv);
    if (depth) g_gpu.device->CreateDepthStencilView(depth, nullptr, &dsv);

    // After idling, WARP's worker threads wake up on the system timer tick (15.6 ms by default),
    // which would dwarf the frame being timed; a 1 ms tick for the duration of the probe avoids
    // that. (Continuous rendering keeps the threads awake, so normal frames are not affected.)
    // Other hiccups only ever inflate a measurement: take the fastest of a few tries and stop as
    // soon as one fits the budget.
    double best = -1;
    timeBeginPeriod(1);
    for (int i = 0; rtv && dsv && i < 4; ++i) {
        wait_for(g_gpu.ctx, g_gpu.frameQuery);
        double start = ms_since_start();
        draw_scene(rtv, dsv, g_width, g_height);
        wait_for(g_gpu.ctx, g_gpu.frameQuery);
        double ms = ms_since_start() - start;
        best = best < 0 ? ms : std::fmin(best, ms);
        if (best <= kBudgetMs) break;
    }
    timeEndPeriod(1);
    safe_release(rtv);
    safe_release(dsv);
    safe_release(color);
    safe_release(depth);

    debug_log(L"WARP frame: %.1f ms at %ux%u", best, g_width, g_height);
    if (best > kBudgetMs) start_gpu_upgrade(best);   // best < 0: could not measure, stay
}

// Measures WARP a moment after the last change, when nothing is being presented: presenting
// makes WARP's timings unreliable. Repeated changes (e.g. resizing) keep pushing the timer back.
static void schedule_probe()
{
    if (g_gpu.warp && !g_upgradeStarted) SetTimer(g_hwnd, kProbeTimer, 300, nullptr);
}

static void render()
{
    if (!g_width || !g_height) return;
    if (g_frameWaitable) WaitForSingleObjectEx(g_frameWaitable, 50, FALSE);
    draw_scene(g_rtv, g_dsv, g_width, g_height);
    g_swapchain->Present(1, 0);
}

static void resize(UINT w, UINT h)
{
    if (!g_swapchain || !w || !h || (w == g_width && h == g_height)) return;
    g_gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    safe_release(g_rtv);
    safe_release(g_dsv);
    g_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    g_width = w;
    g_height = h;
    schedule_probe();
    create_targets();
}

// ---------------------------------------------------------------------------
// Window

static const UINT WM_APP_MESH_LOADED = WM_APP + 1;   // lParam: LoadResult* (owned by the receiver)
static const UINT WM_APP_TEXTURE     = WM_APP + 2;   // wParam: generation, lParam: TextureResult*

struct TextureResult
{
    uint32_t index;
    Image    image;
};

// Decode textures in the background after the model is on screen; each finished image is sent
// to the UI thread, which creates the GPU texture and redraws.
static void start_texture_loading(const std::vector<TextureSource>& sources)
{
    uint32_t generation = ++g_textureGeneration;

    // Only textures that some drawn material actually uses.
    std::vector<uint32_t> wanted;
    std::vector<bool> seen(sources.size());
    for (const Draw& d : g_draws) {
        int t = g_materials[d.material].texture;
        if (t >= 0 && t < (int)sources.size() && !seen[t]) {
            seen[t] = true;
            wanted.push_back((uint32_t)t);
        }
    }
    if (wanted.empty()) return;

    std::thread([generation, sources, wanted = std::move(wanted)] {
        std::atomic<size_t> next = 0;
        auto worker = [&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (size_t i; (i = next.fetch_add(1)) < wanted.size();) {
                if (generation != g_textureGeneration) break;   // a newer model was opened
                TextureResult* r = new TextureResult{ wanted[i] };
                bool ok = load_image(sources[wanted[i]], r->image);
                debug_log(L"texture %u: %s (%ux%u%s)", wanted[i], ok ? L"loaded" : L"FAILED", r->image.width,
                          r->image.height, r->image.cutout ? L", cutout" : L"");
                if (!ok || !PostMessageW(g_hwnd, WM_APP_TEXTURE, generation, (LPARAM)r))
                    delete r;
            }
            CoUninitialize();
        };
        unsigned n = std::thread::hardware_concurrency();
        if (n > wanted.size()) n = (unsigned)wanted.size();
        std::vector<std::thread> threads;
        for (unsigned i = 1; i < n; ++i) threads.emplace_back(worker);
        worker();
        for (std::thread& t : threads) t.join();
    }).detach();
}

static void create_texture(const TextureResult& r)
{
    const Image& img = r.image;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = img.width;
    td.Height = img.height;
    td.MipLevels = (UINT)img.offsets.size();
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<D3D11_SUBRESOURCE_DATA> init(td.MipLevels);
    for (UINT i = 0; i < td.MipLevels; ++i) {
        UINT w = img.width >> i ? img.width >> i : 1;
        init[i].pSysMem = img.pixels.data() + img.offsets[i];
        init[i].SysMemPitch = w * 4;
    }
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_gpu.device->CreateTexture2D(&td, init.data(), &tex))) return;
    safe_release(g_textures[r.index]);
    g_gpu.device->CreateShaderResourceView(tex, nullptr, &g_textures[r.index]);
    g_textureCutout[r.index] = img.cutout;
    schedule_probe();   // textured drawing costs WARP more
    tex->Release();
}

struct LoadResult
{
    std::wstring path;
    Mesh mesh;
    Gpu  gpu;   // set when the model is too heavy for WARP and the window is on WARP
};

static std::wstring g_title;

static void set_title(const wchar_t* path, const std::wstring& error)
{
    const wchar_t* name = path ? path : L"NoTime Fbx";
    for (const wchar_t* p = name; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    g_title = name;
    if (!error.empty()) g_title += L" — error: " + error;
    SetWindowTextW(g_hwnd, g_title.c_str());
}

// Apply a loaded mesh (or its error) to the view. Texture loading and caching start separately,
// after the first frame, via after_first_frame().
static void show_mesh(const wchar_t* path, const Mesh& mesh)
{
    set_title(path, mesh.error);
    if (!mesh.error.empty()) return;
    upload_mesh(mesh);
    schedule_probe();
    g_yaw = 0.6f;
    g_pitch = 0.4f;
    fit_camera();
}

// Background work once the model is on screen: stream textures in, and save the built mesh to the
// cache (this takes ownership of the mesh and frees it when done).
static void after_first_frame(const std::wstring& path, Mesh&& mesh)
{
    if (!mesh.error.empty()) return;
    start_texture_loading(mesh.textures);
    if (!mesh.fromCache)
        std::thread([path, m = std::move(mesh)] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            cache_store(path.c_str(), m);
        }).detach();
}

// Loads a file on a worker thread and hands the result to the UI thread via WM_APP_MESH_LOADED.
// If the window is on WARP and the model is too heavy for it, a GPU device is created alongside
// (started early when the file size already suggests it) and the window switches to it.
static void open_async(const std::wstring& path)
{
    SetWindowTextW(g_hwnd, (path + L" — loading…").c_str());
    std::thread([path] {
        bool mayUpgrade = g_onWarp && renderer_override() != Renderer::Warp;
        LoadResult* r = new LoadResult{ path };
        AsyncGpu gpu;
        if (mayUpgrade && estimate_triangles(path.c_str()) > warp_max_triangles() / 2) gpu.start(false);
        load_mesh(path.c_str(), r->mesh);
        if (mayUpgrade && r->mesh.indices.size() / 3 > warp_max_triangles()) {
            if (!gpu.started()) gpu.start(false);
            if (!gpu.take(r->gpu)) r->gpu = Gpu();   // no usable GPU: stay on WARP
        }
        if (!PostMessageW(g_hwnd, WM_APP_MESH_LOADED, 0, (LPARAM)r)) {
            release_gpu(r->gpu);
            delete r;
        }
    }).detach();
}

static void open_dropped(HDROP drop)
{
    wchar_t file[MAX_PATH];
    UINT ok = DragQueryFileW(drop, 0, file, MAX_PATH);
    DragFinish(drop);
    if (ok) open_async(file);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN:
        g_dragging = true;
        g_lastMouse = { (short)LOWORD(lp), (short)HIWORD(lp) };
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g_dragging = false;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        g_dragging = false;
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
            g_yaw   -= (p.x - g_lastMouse.x) * 0.008f;
            g_pitch += (p.y - g_lastMouse.y) * 0.008f;
            g_pitch = std::fmax(-1.55f, std::fmin(1.55f, g_pitch));
            g_lastMouse = p;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        g_dist *= std::pow(0.88f, GET_WHEEL_DELTA_WPARAM(wp) / 120.0f);
        g_dist = std::fmax(g_radius * 0.05f, std::fmin(g_radius * 50.0f, g_dist));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_SIZE:
        resize(LOWORD(lp), HIWORD(lp));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        if (!g_swapchain) break;
        render();
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_ERASEBKGND:
        if (!g_swapchain) break;   // before D3D is up, let the class brush paint the background
        return 1;
    case WM_DROPFILES:
        open_dropped((HDROP)wp);
        return 0;
    case WM_APP_MESH_LOADED: {
        LoadResult* r = (LoadResult*)lp;
        if (r->gpu.device) {
            if (g_gpu.warp) activate_gpu(r->gpu);   // heavy model: switch from WARP to the GPU
            else release_gpu(r->gpu);               // an earlier file already switched
        }
        show_mesh(r->path.c_str(), r->mesh);
        after_first_frame(r->path, std::move(r->mesh));
        delete r;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wp != kProbeTimer) break;
        KillTimer(hwnd, kProbeTimer);
        if (g_gpu.warp) probe_warp_speed();
        return 0;
    case WM_APP_GPU_READY: {
        Gpu* g = (Gpu*)lp;
        if (g_gpu.warp) switch_to(*g);   // WARP was too slow for this model and window size
        else release_gpu(*g);            // already moved to the GPU by opening a heavy file
        delete g;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_APP_TEXTURE: {
        TextureResult* r = (TextureResult*)lp;
        if (wp == g_textureGeneration && r->index < g_textures.size()) {
            create_texture(*r);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete r;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    mark(L"pre", premain_ms());

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t* path = argc > 1 ? argv[1] : nullptr;

    if ((!path && is_setup_exe()) || (path && !lstrcmpiW(path, L"--install"))) return run_install();
    if (path && !lstrcmpiW(path, L"--uninstall")) return run_uninstall();
    if (path && (!lstrcmpiW(path, L"--register") || !lstrcmpiW(path, L"--unregister"))) {
        bool reg = !lstrcmpiW(path, L"--register");
        bool ok = reg ? register_association(nullptr) : unregister_association();
        if (!ok) MessageBoxW(nullptr, L"Failed to write file association to the registry.", L"NoTime Fbx", MB_ICONERROR);
        else if (!reg) MessageBoxW(nullptr, L".fbx association removed.", L"NoTime Fbx", MB_ICONINFORMATION);
        return ok ? 0 : 1;
    }

    // Start the renderer(s) and the file load right away, in parallel with window creation.
    // WARP unless the model is clearly heavy; the GPU driver too if the model may be too heavy for
    // WARP (queued behind WARP, since D3D11 creates devices one at a time).
    Renderer forced = renderer_override();
    uint64_t guess = forced == Renderer::Auto && path ? estimate_triangles(path) : 0;
    bool mayUseWarp = forced == Renderer::Warp || (forced == Renderer::Auto && guess <= 2 * warp_max_triangles());
    bool mayUseGpu = forced == Renderer::Gpu || (forced == Renderer::Auto && guess > warp_max_triangles() / 2);
    AsyncGpu warp, gpu;
    if (mayUseWarp) warp.start(true);
    if (mayUseGpu) gpu.start(false, &warp);
    Mesh mesh;
    std::thread loader([&] { if (path) load_mesh(path, mesh); });

    // No text input anywhere: skipping IME / text services setup shows the window ~5 ms sooner.
    ImmDisableIME((DWORD)-1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));   // res/NoTimeFbx.rc
    wc.hbrBackground = CreateSolidBrush(RGB(41, 43, 48));   // matches the clear color: no white flash
    wc.lpszClassName = L"NoTimeFbx";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, inst, nullptr);
    set_title(path, L"");
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    mark(L"wnd");

    loader.join();
    mark(L"load");
    {
        wchar_t buf[96];
        swprintf(buf, 96, L" (%s=%.0f build=%.0f %zuk tris)", mesh.fromCache ? L"cache" : L"parse", mesh.parseMs,
                 mesh.buildMs, mesh.indices.size() / 3000);
        append_mark(buf);
    }

    // Final choice, now that the exact triangle count is known.
    bool useWarp = forced == Renderer::Warp ||
                   (forced == Renderer::Auto && mesh.indices.size() / 3 <= warp_max_triangles());
    AsyncGpu& chosen = useWarp ? warp : gpu;
    (useWarp ? gpu : warp).abandon();
    if (!chosen.started()) chosen.start(useWarp);   // the guess was too low: start the GPU now
    Gpu dev;
    if (!chosen.take(dev) && !create_gpu(!useWarp, dev))   // e.g. no usable GPU: use the other renderer
        fatal(L"Failed to create a Direct3D 11 device.");
    mark(dev.warp ? L"warp" : L"gpu", dev.readyMs);
    activate_gpu(dev);
    mark(L"swap");

    if (path) show_mesh(path, mesh);
    render();
    mark(L"draw");
    if (path) after_first_frame(path, std::move(mesh));

    double ms = ms_since_start();
    wchar_t dbg[400];
    swprintf(dbg, 400, L"NoTimeFbx: first frame in %.1f ms (%s)\n", ms, g_marks);
    OutputDebugStringW(dbg);
    if (GetEnvironmentVariableW(L"NOTIMEFBX_TIMING", nullptr, 0)) {
        swprintf(dbg, 400, L"  [%.1f ms:%s]", ms, g_marks);
        SetWindowTextW(g_hwnd, (g_title + dbg).c_str());
    }
    LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
