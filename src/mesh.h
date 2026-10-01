#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Vec3 { float x, y, z; };

inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline Vec3 normalize(Vec3 a) { float l = std::sqrt(dot(a, a)); return l > 0 ? a * (1.0f / l) : a; }

struct Vertex
{
    Vec3     pos;
    uint32_t normal;   // R10G10B10A2_UNORM, n * 0.5 + 0.5
    float    u, v;
};

// Unit normal -> DXGI_FORMAT_R10G10B10A2_UNORM.
inline uint32_t pack_normal(Vec3 n)
{
    auto q = [](float x) { return (uint32_t)((x * 0.5f + 0.5f) * 1023.0f + 0.5f) & 1023u; };
    return q(n.x) | (q(n.y) << 10) | (q(n.z) << 20);
}

// Heap array whose resize() leaves elements uninitialized: no pointless zeroing of 100+ MB.
template <class T> struct Array
{
    std::unique_ptr<T[]> ptr;
    size_t count = 0;

    void resize(size_t n) { ptr.reset(n ? new T[n] : nullptr); count = n; }
    T* data() const { return ptr.get(); }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    T& operator[](size_t i) const { return ptr[i]; }
};

// Where to find a texture image: embedded bytes, or candidate file paths (best first).
struct TextureSource
{
    std::vector<uint8_t>      embedded;
    std::vector<std::wstring> paths;
};

struct Material
{
    Vec3 color{ 0.8f, 0.8f, 0.8f };
    int  texture = -1;        // index into Mesh::textures
    bool alphaTest = false;
};

// Contiguous index range drawn with one material.
struct Draw
{
    uint32_t firstIndex, indexCount, material;
};

// Whole scene flattened into one world-space triangle list, grouped by material.
struct Mesh
{
    Array<Vertex>   vertices;
    Array<uint32_t> indices;
    std::vector<Draw>          draws;
    std::vector<Material>      materials;
    std::vector<TextureSource> textures;
    Vec3 bmin{ 0, 0, 0 }, bmax{ 0, 0, 0 };
    std::wstring error;   // non-empty if loading failed
    double parseMs = 0, buildMs = 0;   // diagnostics
    bool fromCache = false;
};

// loader.cpp. True for the file extensions the viewer opens (.fbx, .stl).
bool is_model_path(const wchar_t* path);

// loader.cpp. Safe to call from any thread. STL files are parsed by stl.cpp.
bool load_mesh(const wchar_t* path, Mesh& mesh);

// stl.cpp. Binary or ASCII STL -> the same flat triangle list (no materials or textures, facet
// normals, Z-up data rotated upright). `data` holds the whole file. Safe to call from any thread.
bool load_stl(const void* data, size_t size, Mesh& mesh);

// cache.cpp. cache_load() is used by load_mesh(); cache_store() is slow (writes the whole mesh to
// disk) and is meant for a background thread once the model is displayed. Both are thread-safe.
bool cache_load(const wchar_t* path, Mesh& mesh);
void cache_store(const wchar_t* path, const Mesh& mesh);
// Triangle count of a valid cache entry, without loading it (reads only the header).
bool cache_peek(const wchar_t* path, uint64_t& triangles);

// Diagnostics: appends a line to %TEMP%\NoTimeFbx.log when NOTIMEFBX_LOG is set. Thread-safe.
void debug_log(const wchar_t* fmt, ...);

// Diagnostics: milliseconds since an arbitrary start, for load/parse timings. Thread-safe.
double now_ms();
