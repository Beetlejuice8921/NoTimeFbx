// NoTimeFbx: minimal, fast-starting FBX/STL viewer. Rotate with LMB, zoom with wheel, Esc to quit.
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
#include <d3d11_1.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mesh.h"
#include "texture.h"

#include "shaders_vs.h"
#include "shaders_ps.h"
#include "shaders_ui_vs.h"
#include "shaders_ui_ps.h"

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

static void scan_folder(const wchar_t* path);         // builds the file list for a model's folder
static void open_async(const std::wstring& path);     // loads a file on a worker thread

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

// Triangle count guess before loading, to start the right device early. Exact for cached meshes
// and binary STL; otherwise from the file size. Bytes per triangle measured on real files: FBX
// binary 65-130, FBX ASCII 150-400, STL ASCII ~250. The low ends are used, so the guess errs on
// the heavy side.
static uint64_t estimate_triangles(const wchar_t* path)
{
    uint64_t tris;
    if (cache_peek(path, tris)) return tris;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size = {};
    unsigned char head[84] = {};
    DWORD got = 0;
    GetFileSizeEx(f, &size);
    ReadFile(f, head, sizeof(head), &got, nullptr);
    CloseHandle(f);
    if (got == sizeof(head)) {
        uint64_t n = head[80] | (uint64_t)head[81] << 8 | (uint64_t)head[82] << 16 | (uint64_t)head[83] << 24;
        if (84 + 50 * n == (uint64_t)size.QuadPart) return n;   // binary STL: exact
        if (!_strnicmp((const char*)head, "solid", 5)) return (uint64_t)size.QuadPart / 200;   // ASCII STL
    }
    bool binary = got >= 7 && !memcmp(head, "Kaydara", 7);   // "Kaydara FBX Binary"
    return (uint64_t)size.QuadPart / (binary ? 60 : 140);
}

// ---------------------------------------------------------------------------
// File list: the right panel lists the other models in the open file's folder

static const int kPanelWidth = 128;    // panel strip on the right
static const int kThumbSize = 112;     // square 3D preview of a list item
static const int kThumbLabel = 20;     // filename strip under the preview
static const int kItemPitch = kThumbSize + kThumbLabel + 6;

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
    // File list: pixel-space quads, plus a reusable render target for thumbnails.
    ID3D11VertexShader*    uiVs = nullptr;
    ID3D11PixelShader*     uiPs = nullptr;
    ID3D11InputLayout*     uiLayout = nullptr;
    ID3D11Buffer*          uiCB = nullptr;
    ID3D11Buffer*          uiVb = nullptr;         // one quad, rewritten per quad
    ID3D11DepthStencilState* uiDepth = nullptr;    // depth test off for UI drawing
    ID3D11ShaderResourceView* whiteSrv = nullptr;  // 1x1 white: tinted solid quads
    ID3D11Texture2D*       thumbRt = nullptr;      // 3D previews are rendered here...
    ID3D11RenderTargetView* thumbRtv = nullptr;
    ID3D11Texture2D*       thumbDepth = nullptr;
    ID3D11DepthStencilView* thumbDsv = nullptr;
    ID3D11Texture2D*       thumbGdi = nullptr;     // ...copied here, GDI adds the filename
    bool                   warp = false;
    double                 readyMs = 0;   // when creation finished, ms since launch (diagnostics)
};

static void release_gpu(Gpu& g)
{
    safe_release(g.thumbGdi);
    safe_release(g.thumbDsv);
    safe_release(g.thumbDepth);
    safe_release(g.thumbRtv);
    safe_release(g.thumbRt);
    safe_release(g.whiteSrv);
    safe_release(g.uiDepth);
    safe_release(g.uiVb);
    safe_release(g.uiCB);
    safe_release(g.uiLayout);
    safe_release(g.uiPs);
    safe_release(g.uiVs);
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
    // BGRA support is what lets GDI write into the GDI-compatible thumbnail textures.
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
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

    // File list UI: quads in pixel coordinates, a 1x1 white texture for tinted solids, and the
    // thumbnail render target with its GDI-compatible copy (preview on top, filename strip below).
    g.device->CreateVertexShader(g_ui_vs_main, sizeof(g_ui_vs_main), nullptr, &g.uiVs);
    g.device->CreatePixelShader(g_ui_ps_main, sizeof(g_ui_ps_main), nullptr, &g.uiPs);
    D3D11_INPUT_ELEMENT_DESC uiLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g.device->CreateInputLayout(uiLayout, 2, g_ui_vs_main, sizeof(g_ui_vs_main), &g.uiLayout);
    cd.ByteWidth = 32;   // Ui cbuffer: backbuffer size + tint
    g.device->CreateBuffer(&cd, nullptr, &g.uiCB);
    D3D11_BUFFER_DESC vd = {};
    vd.ByteWidth = 64;   // one quad: 4 vertices x (x, y, u, v)
    vd.Usage = D3D11_USAGE_DYNAMIC;
    vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.device->CreateBuffer(&vd, nullptr, &g.uiVb);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    g.device->CreateDepthStencilState(&dd, &g.uiDepth);
    uint32_t whitePixel = 0xffffffffu;
    D3D11_TEXTURE2D_DESC td = { 1, 1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA whiteInit = { &whitePixel, 4, 0 };
    {
        ID3D11Texture2D* white = nullptr;
        if (SUCCEEDED(g.device->CreateTexture2D(&td, &whiteInit, &white))) {
            g.device->CreateShaderResourceView(white, nullptr, &g.whiteSrv);
            white->Release();
        }
    }
    td.Usage = D3D11_USAGE_DEFAULT;
    td.Width = kThumbSize;
    td.Height = kThumbSize;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    g.device->CreateTexture2D(&td, nullptr, &g.thumbRt);
    if (g.thumbRt) g.device->CreateRenderTargetView(g.thumbRt, nullptr, &g.thumbRtv);
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    g.device->CreateTexture2D(&td, nullptr, &g.thumbDepth);
    if (g.thumbDepth) g.device->CreateDepthStencilView(g.thumbDepth, nullptr, &g.thumbDsv);
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.Height = kThumbSize + kThumbLabel;
    // GetDC on a surface requires the GDI_COMPATIBLE flag and a render-target bind flag.
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    g.device->CreateTexture2D(&td, nullptr, &g.thumbGdi);

    g.readyMs = ms_since_start();
    if (g.vs && g.ps && g.layout && g.frameCB && g.materialCB && g.sampler && g.raster && g.uiVs &&
        g.uiPs && g.uiLayout && g.uiCB && g.uiVb && g.uiDepth && g.whiteSrv && g.thumbRtv &&
        g.thumbDsv && g.thumbGdi)
        return true;
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

// Other models in the open file's folder, shown as previews in the right panel.
struct ListItem
{
    std::wstring path;   // full path to the model file
    std::wstring name;   // file name, drawn under the preview
    uint64_t     bytes = 0;
    bool         ready = false;
    ID3D11ShaderResourceView* thumb = nullptr;   // preview + filename, on the active device
};
static std::vector<ListItem> g_list;
static std::wstring g_listDir;       // folder the list was built from, with the trailing slash
static std::wstring g_activePath;    // the file the viewer shows (or is loading)
static float g_listScroll = 0;       // list pixels scrolled off the top
static uint32_t g_openGeneration;    // bumps on every open: stale load results are dropped
static uint32_t g_previewGeneration; // bumps when the list is rebuilt: stale previews are dropped

// Viewport the 3D model is drawn in: the whole window, minus the file list when it is shown.
static UINT viewport_width() { return g_list.empty() ? g_width : g_width - kPanelWidth; }

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
    // Thumbnails too: the list stays usable across the switch (each is tiny, copying is instant).
    for (ListItem& it : g_list)
        if (it.thumb) {
            ID3D11ShaderResourceView* t = copy_texture(it.thumb, gpu.device);
            safe_release(it.thumb);
            it.thumb = t;
        }
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
    float tanH = tanV * (g_height ? (float)viewport_width() / g_height : 1.6f);
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

// ---------------------------------------------------------------------------
// File list: thumbnails, panel drawing, selection

static void clamp_scroll()
{
    float content = 8.0f + (float)g_list.size() * kItemPitch + 8.0f;
    g_listScroll = std::fmax(0.0f, std::fmin(g_listScroll, std::fmax(0.0f, content - g_height)));
}

static void scroll_into_view(size_t i)
{
    float top = 8.0f + (float)i * kItemPitch;
    g_listScroll = std::fmin(g_listScroll, top - 8.0f);
    g_listScroll = std::fmax(g_listScroll, top + kThumbSize + kThumbLabel - (float)g_height + 8.0f);
    clamp_scroll();
}

// Renders one list item's preview: the model into the shared render target, the filename under it
// via GDI, and keeps the result as the item's texture. Runs on the UI thread, like all drawing.
static void render_thumbnail(ListItem& it, const Mesh& mesh)
{
    if (!mesh.vertices.size() || !mesh.indices.size()) return;

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(mesh.vertices.size() * sizeof(Vertex));
    D3D11_SUBRESOURCE_DATA init = { mesh.vertices.data() };
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    g_gpu.device->CreateBuffer(&bd, &init, &vb);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(mesh.indices.size() * sizeof(uint32_t));
    init.pSysMem = mesh.indices.data();
    g_gpu.device->CreateBuffer(&bd, &init, &ib);
    if (!vb || !ib) {
        safe_release(vb);
        safe_release(ib);
        return;
    }

    // Same framing and headlight as the main view, square aspect.
    Vec3 target = (mesh.bmin + mesh.bmax) * 0.5f;
    Vec3 d = view_dir();
    Vec3 right = normalize(cross({ 0, 1, 0 }, d));
    Vec3 up = cross(d, right);
    Vec3 diag = mesh.bmax - mesh.bmin;
    float radius = std::fmax(std::sqrt(dot(diag, diag)) * 0.5f, 1e-4f);
    float tanV = std::tan(kFovY * 0.5f);
    float dist = 0;
    for (int i = 0; i < 8; ++i) {
        Vec3 c = { (i & 1) ? mesh.bmax.x : mesh.bmin.x, (i & 2) ? mesh.bmax.y : mesh.bmin.y,
                   (i & 4) ? mesh.bmax.z : mesh.bmin.z };
        Vec3 rel = c - target;
        float z = dot(rel, d);
        dist = std::fmax(dist, z + std::fabs(dot(rel, right)) / tanV);
        dist = std::fmax(dist, z + std::fabs(dot(rel, up)) / tanV);
    }
    dist = std::fmax(dist * 1.08f, radius * 0.05f);

    ID3D11DeviceContext* ctx = g_gpu.ctx;
    float zn = std::fmax(dist - radius * 1.5f, dist * 0.01f);
    float zf = dist + radius * 1.5f;
    FrameConstants fc;
    fc.viewProj = mul(perspective_rh(kFovY, 1.0f, zn, zf), look_at_rh(target + d * dist, target, { 0, 1, 0 }));
    fc.lightDir = normalize(d + Vec3{ 0, 0.5f, 0 });
    fc.pad = 0;
    MaterialConstants mc = { { 0.8f, 0.8f, 0.8f }, 0, 0, { 0, 0, 0 } };
    D3D11_MAPPED_SUBRESOURCE ms;
    ctx->Map(g_gpu.frameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &fc, sizeof(fc));
    ctx->Unmap(g_gpu.frameCB, 0);
    ctx->Map(g_gpu.materialCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &mc, sizeof(mc));
    ctx->Unmap(g_gpu.materialCB, 0);

    const float clear[4] = { 0.16f, 0.17f, 0.19f, 1.0f };
    ctx->ClearRenderTargetView(g_gpu.thumbRtv, clear);
    ctx->ClearDepthStencilView(g_gpu.thumbDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetRenderTargets(1, &g_gpu.thumbRtv, g_gpu.thumbDsv);
    D3D11_VIEWPORT vp = { 0, 0, (float)kThumbSize, (float)kThumbSize, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_gpu.raster);
    UINT stride = sizeof(Vertex), offs = 0;
    ctx->IASetInputLayout(g_gpu.layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offs);
    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
    ctx->VSSetShader(g_gpu.vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_gpu.frameCB);
    ctx->PSSetShader(g_gpu.ps, nullptr, 0);
    ID3D11Buffer* psBuffers[2] = { g_gpu.frameCB, g_gpu.materialCB };
    ctx->PSSetConstantBuffers(0, 2, psBuffers);
    ctx->PSSetSamplers(0, 1, &g_gpu.sampler);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    // A thumbnail is tiny: draw at most 90k triangles, more cannot be seen anyway.
    ctx->DrawIndexed((UINT)std::min<size_t>(mesh.indices.size(), 270000) / 3 * 3, 0, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    ctx->CopySubresourceRegion(g_gpu.thumbGdi, 0, 0, 0, 0, g_gpu.thumbRt, 0, nullptr);
    IDXGISurface1* surf = nullptr;
    if (SUCCEEDED(g_gpu.thumbGdi->QueryInterface(IID_PPV_ARGS(&surf)))) {
        HDC dc = nullptr;
        if (SUCCEEDED(surf->GetDC(FALSE, &dc))) {
            static const HBRUSH brush = CreateSolidBrush(RGB(30, 31, 35));
            RECT rc = { 0, kThumbSize, kThumbSize, kThumbSize + kThumbLabel };
            FillRect(dc, &rc, brush);
            static const HFONT font = [] {
                NONCLIENTMETRICSW ncm = { sizeof(ncm) };
                SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
                return CreateFontIndirectW(&ncm.lfMessageFont);
            }();
            SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(219, 220, 222));
            RECT trc = { 2, kThumbSize, kThumbSize - 2, kThumbSize + kThumbLabel };
            DrawTextW(dc, it.name.c_str(), -1, &trc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            surf->ReleaseDC(nullptr);
        }
        surf->Release();
    }

    safe_release(it.thumb);
    // The shared render target only holds the most recent preview: each item gets its own copy.
    D3D11_TEXTURE2D_DESC copyDesc = { kThumbSize, kThumbSize + kThumbLabel, 1, 1,
                                      DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                      D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D* copy = nullptr;
    if (SUCCEEDED(g_gpu.device->CreateTexture2D(&copyDesc, nullptr, &copy))) {
        ctx->CopyResource(copy, g_gpu.thumbGdi);
        g_gpu.device->CreateShaderResourceView(copy, nullptr, &it.thumb);
        copy->Release();
    }
    it.ready = it.thumb != nullptr;
    safe_release(vb);
    safe_release(ib);
}

// One UI quad in pixel coordinates (y down), drawn as a triangle strip.
static void draw_quad(ID3D11DeviceContext* ctx, float x, float y, float w, float h, float u1, float v1,
                      float u2, float v2, const float tint[4])
{
    struct UiVertex { float x, y, u, v; };
    const UiVertex verts[4] = {
        { x,     y,     u1, v1 },
        { x + w, y,     u2, v1 },
        { x,     y + h, u1, v2 },
        { x + w, y + h, u2, v2 },
    };
    D3D11_MAPPED_SUBRESOURCE ms;
    ctx->Map(g_gpu.uiCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    float data[8] = { (float)g_width, (float)g_height, 0, 0, tint[0], tint[1], tint[2], tint[3] };
    memcpy(ms.pData, data, sizeof(data));
    ctx->Unmap(g_gpu.uiCB, 0);
    ctx->Map(g_gpu.uiVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, verts, sizeof(verts));
    ctx->Unmap(g_gpu.uiVb, 0);
    ctx->Draw(4, 0);
}

static void draw_ui(ID3D11DeviceContext* ctx)
{
    if (g_list.empty()) return;

    ctx->OMSetDepthStencilState(g_gpu.uiDepth, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    UINT stride = sizeof(float) * 4, offs = 0;
    ctx->IASetInputLayout(g_gpu.uiLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetVertexBuffers(0, 1, &g_gpu.uiVb, &stride, &offs);
    ctx->VSSetShader(g_gpu.uiVs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_gpu.uiCB);
    ctx->PSSetShader(g_gpu.uiPs, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_gpu.uiCB);
    ctx->PSSetSamplers(0, 1, &g_gpu.sampler);

    const float x0 = (float)(g_width - kPanelWidth);
    const float bgTint[4] = { 0.114f, 0.121f, 0.135f, 1 };
    draw_quad(ctx, x0, 0, (float)kPanelWidth, (float)g_height, 0, 0, 0, 0, bgTint);

    for (size_t i = 0; i < g_list.size(); ++i) {
        const ListItem& it = g_list[i];
        float y = 8.0f + (float)i * kItemPitch - g_listScroll;
        if (y + kThumbSize + kThumbLabel < 0 || y > (float)g_height) continue;
        bool active = _wcsicmp(it.path.c_str(), g_activePath.c_str()) == 0;
        const float borderTint[4] = { active ? 0.22f : 0.255f, active ? 0.50f : 0.265f,
                                      active ? 0.92f : 0.295f, 1 };
        draw_quad(ctx, x0 + 6, y, (float)(kThumbSize + 4), (float)(kThumbSize + kThumbLabel + 4),
                  0, 0, 0, 0, borderTint);
        if (it.ready) {
            const float white[4] = { 1, 1, 1, 1 };
            ctx->PSSetShaderResources(0, 1, &it.thumb);
            draw_quad(ctx, x0 + 8, y + 2, (float)kThumbSize, (float)(kThumbSize + kThumbLabel),
                      0, 0, 1, 1, white);
        } else {
            const float placeholder[4] = { 0.185f, 0.195f, 0.215f, 1 };
            ctx->PSSetShaderResources(0, 1, &g_gpu.whiteSrv);
            draw_quad(ctx, x0 + 8, y + 2, (float)kThumbSize, (float)(kThumbSize + kThumbLabel),
                      0, 0, 0, 0, placeholder);
        }
    }
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
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
    draw_scene(g_rtv, g_dsv, viewport_width(), g_height);
    draw_ui(g_gpu.ctx);
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
static const UINT WM_APP_PREVIEW     = WM_APP + 4;   // wParam: generation, lParam: PreviewResult*

struct TextureResult
{
    uint32_t index;
    Image    image;
};

// A parsed folder file, ready to become a thumbnail (happens on the UI thread).
struct PreviewResult
{
    std::wstring path;
    Mesh*        mesh;
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
    uint32_t generation;
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
    scan_folder(path);
    set_title(path, mesh.error);
    if (!mesh.error.empty()) return;
    upload_mesh(mesh);
    // The open file's own thumbnail reuses this mesh: no second parse of a possibly huge file.
    for (ListItem& it : g_list)
        if (!it.ready && _wcsicmp(it.path.c_str(), path) == 0) {
            render_thumbnail(it, mesh);
            break;
        }
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
// (started early when the file size already suggests it) and the window switches to it. Each open
// bumps a generation: results of opens that were superseded (arrow-key flipping) are dropped.
static void open_async(const std::wstring& path)
{
    if (path == g_activePath) return;   // already showing (or loading) exactly this file
    g_activePath = path;
    uint32_t generation = ++g_openGeneration;
    SetWindowTextW(g_hwnd, (path + L" — loading…").c_str());
    std::thread([path, generation] {
        bool mayUpgrade = g_onWarp && renderer_override() != Renderer::Warp;
        LoadResult* r = new LoadResult{ path, generation };
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

// ---------------------------------------------------------------------------
// Folder scan and preview loading

static std::mutex g_pqMutex;
static std::condition_variable g_pqCv;
static std::deque<std::pair<std::wstring, uint32_t>> g_pq;   // path + generation it was queued for
static std::atomic<bool> g_previewWorkerStarted;

// One worker loads folder files one at a time, so previews never steal the whole machine from the
// file the user actually opened. Each finished parse is posted to the UI thread, which renders the
// thumbnail. Preview meshes go through load_mesh() like everything else: the cache makes repeats
// (clicking the item later) free.
static void start_preview_worker()
{
    if (g_previewWorkerStarted.exchange(true)) return;
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::wstring path;
            uint32_t generation = 0;
            {
                std::unique_lock<std::mutex> lock(g_pqMutex);
                g_pqCv.wait(lock, [] { return !g_pq.empty(); });
                path = std::move(g_pq.front().first);
                generation = g_pq.front().second;
                g_pq.pop_front();
            }
            if (generation != g_previewGeneration) continue;   // the list was rebuilt meanwhile
            Mesh* mesh = new Mesh();
            load_mesh(path.c_str(), *mesh);
            PreviewResult* r = new PreviewResult{ std::move(path), mesh };
            if (!PostMessageW(g_hwnd, WM_APP_PREVIEW, generation, (LPARAM)r)) {
                delete mesh;
                delete r;
            }
        }
        CoUninitialize();
    }).detach();
}

// Collects the model files next to `path` into the right panel. Cheap (a directory listing), runs
// on the UI thread; the actual preview parsing is queued for the background worker.
static void scan_folder(const wchar_t* path)
{
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    if (dir == g_listDir) return;   // same folder: keep the thumbnails loaded so far

    g_previewGeneration++;
    {
        std::lock_guard<std::mutex> lock(g_pqMutex);
        g_pq.clear();
    }
    for (ListItem& it : g_list) safe_release(it.thumb);
    g_list.clear();
    g_listDir = dir;
    g_listScroll = 0;

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_model_path(fd.cFileName)) continue;
        ListItem it;
        it.path = dir + fd.cFileName;
        it.name = fd.cFileName;
        it.bytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        g_list.push_back(std::move(it));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(g_list.begin(), g_list.end(), [](const ListItem& a, const ListItem& b) {
        return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0;
    });

    // Queue previews smallest-first: small models fill the list in quickly, heavy files last.
    std::vector<const ListItem*> order;
    for (const ListItem& it : g_list) order.push_back(&it);
    std::sort(order.begin(), order.end(), [](const ListItem* a, const ListItem* b) { return a->bytes < b->bytes; });
    {
        std::lock_guard<std::mutex> lock(g_pqMutex);
        for (const ListItem* it : order) g_pq.emplace_back(it->path, g_previewGeneration);
    }
    g_pqCv.notify_one();
    start_preview_worker();
}

// Makes the list item at `i` the active model (no-op when it is already showing).
static void select_item(size_t i)
{
    if (i >= g_list.size()) return;
    scroll_into_view(i);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    open_async(g_list[i].path);
}

// Flips through the list by keyboard: WASD and the arrows, with wrap-around.
static void select_relative(int dir)
{
    if (g_list.empty()) return;
    size_t i = 0;
    for (size_t k = 0; k < g_list.size(); ++k)
        if (_wcsicmp(g_list[k].path.c_str(), g_activePath.c_str()) == 0) { i = k; break; }
    select_item((i + dir + g_list.size()) % g_list.size());
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
    {
        int x = (short)LOWORD(lp);
        if (x >= (int)viewport_width() && !g_list.empty()) {   // click in the file list
            float y = (short)HIWORD(lp) + g_listScroll - 8.0f;
            if (y >= 0) select_item((size_t)(y / kItemPitch));
            return 0;
        }
        g_dragging = true;
        g_lastMouse = { (short)LOWORD(lp), (short)HIWORD(lp) };
        SetCapture(hwnd);
        return 0;
    }
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
    {
        // Over the file list the wheel scrolls it; over the model it zooms, as before.
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &p);
        if (!g_list.empty() && p.x >= (int)viewport_width()) {
            g_listScroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * kItemPitch * 1.5f;
            clamp_scroll();
        } else {
            g_dist *= std::pow(0.88f, GET_WHEEL_DELTA_WPARAM(wp) / 120.0f);
            g_dist = std::fmax(g_radius * 0.05f, std::fmin(g_radius * 50.0f, g_dist));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        // WASD and the arrows flip through the folder list; W/Up and A/Left go back.
        else if (wp == 'W' || wp == 'A' || wp == VK_LEFT || wp == VK_UP) select_relative(-1);
        else if (wp == 'S' || wp == 'D' || wp == VK_RIGHT || wp == VK_DOWN) select_relative(1);
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
        if (r->generation != g_openGeneration) {   // superseded by a newer open (key flipping)
            release_gpu(r->gpu);
            delete r;
            return 0;
        }
        if (r->gpu.device) {
            if (g_gpu.warp) {   // heavy model: switch from WARP to the GPU, moving the list along
                for (ListItem& it : g_list)
                    if (it.thumb) {
                        ID3D11ShaderResourceView* t = copy_texture(it.thumb, r->gpu.device);
                        safe_release(it.thumb);
                        it.thumb = t;
                    }
                activate_gpu(r->gpu);
            } else release_gpu(r->gpu);          // an earlier file already switched
        }
        show_mesh(r->path.c_str(), r->mesh);
        after_first_frame(r->path, std::move(r->mesh));
        delete r;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_APP_PREVIEW: {
        PreviewResult* r = (PreviewResult*)lp;
        if (wp == g_previewGeneration) {
            for (ListItem& it : g_list) {
                if (!it.ready && _wcsicmp(it.path.c_str(), r->path.c_str()) == 0) {
                    render_thumbnail(it, *r->mesh);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
        }
        delete r->mesh;
        delete r;
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
    if (path) g_activePath = path;

    if ((!path && is_setup_exe()) || (path && !lstrcmpiW(path, L"--install"))) return run_install();
    if (path && !lstrcmpiW(path, L"--uninstall")) return run_uninstall();
    if (path && (!lstrcmpiW(path, L"--register") || !lstrcmpiW(path, L"--unregister"))) {
        bool reg = !lstrcmpiW(path, L"--register");
        bool ok = reg ? register_association(nullptr) : unregister_association();
        if (!ok) MessageBoxW(nullptr, L"Failed to write file association to the registry.", L"NoTime Fbx", MB_ICONERROR);
        else if (!reg) MessageBoxW(nullptr, L".fbx/.stl association removed.", L"NoTime Fbx", MB_ICONINFORMATION);
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
