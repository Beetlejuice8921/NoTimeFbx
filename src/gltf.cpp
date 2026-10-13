// glTF 2.0 (.glb and .gltf) -> flat world-space triangle list, via cgltf.
//
// glTF is already right-handed Y-up with V pointing down, like the viewer, so positions and UVs are
// used as they are. Unlike the FBX path, vertices stay shared: glTF data is already indexed, so
// every primitive instance copies its vertices once and its indices are rebased. Instances are
// grouped by material (one draw each), and both vertex and index work is split into chunks so that
// one huge primitive still uses every core. A .glb is parsed straight from the memory mapping; the
// binary chunk is never copied. Animation, skinning and morph targets are ignored (bind pose), and
// Draco/meshopt-compressed geometry is reported as unsupported.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "mesh.h"

#pragma warning(push, 0)
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#pragma warning(pop)

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <execution>
#include <unordered_map>
#include <vector>

namespace {

std::wstring widen(const char* s, int len = -1)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, len, w.data(), n);
    if (len < 0 && !w.empty()) w.pop_back();   // the terminator counted by -1
    return w;
}

std::string narrow(const wchar_t* s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    return r;
}

// cgltf opens external .bin files with fopen(), which takes the ANSI code page on Windows and
// fails on non-Latin paths. Its paths are UTF-8, so read them through the wide API instead.
cgltf_result read_file(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size,
                       void** data)
{
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return cgltf_result_file_not_found;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(f, &sz);
    void* buf = sz.QuadPart > 0 && sz.QuadPart < (1ll << 32) ? malloc((size_t)sz.QuadPart) : nullptr;
    DWORD got = 0;
    bool ok = buf && ReadFile(f, buf, (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart;
    CloseHandle(f);
    if (!ok) {
        free(buf);
        return cgltf_result_io_error;
    }
    *size = (cgltf_size)sz.QuadPart;
    *data = buf;
    return cgltf_result_success;
}

void release_file(const cgltf_memory_options*, const cgltf_file_options*, void* data) { free(data); }

cgltf_options make_options()
{
    cgltf_options o = {};
    o.file.read = read_file;
    o.file.release = release_file;
    return o;
}

const wchar_t* result_text(cgltf_result r)
{
    switch (r) {
    case cgltf_result_data_too_short:   return L"truncated glTF file";
    case cgltf_result_unknown_format:   return L"not a glTF 2.0 file";
    case cgltf_result_invalid_json:     return L"corrupt glTF JSON";
    case cgltf_result_invalid_gltf:     return L"invalid glTF file";
    case cgltf_result_file_not_found:   return L"glTF buffer file not found";
    case cgltf_result_io_error:         return L"cannot read glTF buffer file";
    case cgltf_result_out_of_memory:    return L"out of memory";
    case cgltf_result_legacy_gltf:      return L"glTF 1.0 files are not supported";
    default:                            return L"cannot load glTF file";
    }
}

// Column-major 4x4, as glTF stores them.
struct Mat4 { float m[16]; };

Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r.m[c * 4 + row] = a.m[row] * b.m[c * 4] + a.m[4 + row] * b.m[c * 4 + 1] +
                               a.m[8 + row] * b.m[c * 4 + 2] + a.m[12 + row] * b.m[c * 4 + 3];
    return r;
}

Vec3 transform_point(const Mat4& t, const float* p)
{
    const float* m = t.m;
    return { m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
             m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
             m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] };
}

// Normals transform by the inverse transpose of the upper 3x3. Its cofactor matrix is that times
// the determinant, which is fine since normals are renormalized; the sign of the determinant is
// put back so that mirroring transforms keep normals pointing out.
struct NormalMat { Vec3 c0, c1, c2; };

NormalMat normal_matrix(const Mat4& t, float& det)
{
    Vec3 x = { t.m[0], t.m[1], t.m[2] }, y = { t.m[4], t.m[5], t.m[6] }, z = { t.m[8], t.m[9], t.m[10] };
    Vec3 cx = cross(y, z), cy = cross(z, x), cz = cross(x, y);   // rows of the cofactor matrix
    det = dot(x, cx);
    float s = det < 0 ? -1.0f : 1.0f;
    // n' = s * (n.x * cx + n.y * cy + n.z * cz)
    return { cx * s, cy * s, cz * s };
}

Vec3 transform_normal(const NormalMat& nm, const float* n)
{
    return nm.c0 * n[0] + nm.c1 * n[1] + nm.c2 * n[2];
}

// Linear base color factor -> the display-referred colors the viewer's shader works in.
float linear_to_srgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Float attribute reader: points straight into the buffer when the accessor holds plain floats,
// otherwise at a converted copy (normalized integers from KHR_mesh_quantization, sparse data).
struct Stream
{
    const uint8_t* data = nullptr;
    size_t stride = 0;
    const float* at(size_t i) const { return (const float*)(data + i * stride); }
};

struct Streams
{
    std::unordered_map<const cgltf_accessor*, std::vector<float>> converted;

    bool get(const cgltf_accessor* a, size_t comps, Stream& s)
    {
        if (!a || cgltf_num_components(a->type) != comps) return false;
        if (a->component_type == cgltf_component_type_r_32f && !a->normalized && !a->is_sparse && a->buffer_view) {
            const uint8_t* view = cgltf_buffer_view_data(a->buffer_view);
            if (!view) return false;
            s.data = view + a->offset;
            s.stride = a->stride;
            return true;
        }
        auto it = converted.find(a);
        if (it == converted.end()) {
            std::vector<float> v(a->count * comps);
            if (cgltf_accessor_unpack_floats(a, v.data(), v.size()) != v.size()) return false;
            it = converted.emplace(a, std::move(v)).first;
        }
        s.data = (const uint8_t*)it->second.data();
        s.stride = comps * sizeof(float);
        return true;
    }
};

// One primitive placed in the world by one node.
struct Instance
{
    const cgltf_primitive* prim;
    Mat4 world;
    NormalMat normalMat;
    bool mirrored;
    uint32_t material;
    Stream pos, normal, uv;
    bool hasNormal, hasUV;
    const uint8_t* indices;   // null: non-indexed
    size_t indexSize;         // bytes per index
    size_t vertCount, triCount;
    size_t outVert, outTri;
};

size_t triangle_count(const cgltf_primitive& p)
{
    size_t n = p.indices ? p.indices->count : (p.attributes_count ? p.attributes[0].data->count : 0);
    switch (p.type) {
    case cgltf_primitive_type_triangles:      return n / 3;
    case cgltf_primitive_type_triangle_strip:
    case cgltf_primitive_type_triangle_fan:   return n >= 3 ? n - 2 : 0;
    default:                                  return 0;   // points and lines are not drawn
    }
}

const cgltf_accessor* find_attribute(const cgltf_primitive& p, cgltf_attribute_type type, int index)
{
    for (size_t i = 0; i < p.attributes_count; ++i)
        if (p.attributes[i].type == type && p.attributes[i].index == index) return p.attributes[i].data;
    return nullptr;
}

// Calls fn(node, world) for every node of the default scene (all root nodes if there is none).
template <class Fn> void walk_scene(const cgltf_data* g, Fn&& fn)
{
    std::vector<std::pair<const cgltf_node*, Mat4>> stack;
    auto push_roots = [&](cgltf_node* const* nodes, size_t count) {
        Mat4 identity = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
        for (size_t i = count; i-- > 0;) stack.push_back({ nodes[i], identity });
    };
    const cgltf_scene* scene = g->scene ? g->scene : g->scenes_count ? &g->scenes[0] : nullptr;
    if (scene) {
        push_roots(scene->nodes, scene->nodes_count);
    } else {
        std::vector<cgltf_node*> roots;
        for (size_t i = 0; i < g->nodes_count; ++i)
            if (!g->nodes[i].parent) roots.push_back(&g->nodes[i]);
        push_roots(roots.data(), roots.size());
    }
    size_t visited = 0;
    while (!stack.empty() && visited++ < g->nodes_count * 4 + 16) {   // the bound guards against cycles
        auto [node, parent] = stack.back();
        stack.pop_back();
        Mat4 local;
        cgltf_node_transform_local(node, local.m);
        Mat4 world = mul(parent, local);
        fn(node, world);
        for (size_t i = node->children_count; i-- > 0;) stack.push_back({ node->children[i], world });
    }
}

const char* required_unsupported(const cgltf_data* g)
{
    for (size_t i = 0; i < g->extensions_required_count; ++i) {
        const char* e = g->extensions_required[i];
        if (!strcmp(e, "KHR_draco_mesh_compression")) return "Draco-compressed glTF is not supported";
        if (!strcmp(e, "EXT_meshopt_compression") || !strcmp(e, "KHR_meshopt_compression"))
            return "meshopt-compressed glTF is not supported";
    }
    return nullptr;
}

// Texture source for a glTF image: bytes from the binary chunk, a data: URI, or a file next to
// the model.
void describe_image(const cgltf_options& opts, const cgltf_image& img, const std::wstring& dir,
                    TextureSource& ts)
{
    if (img.buffer_view) {
        const uint8_t* p = cgltf_buffer_view_data(img.buffer_view);
        if (p) ts.embedded.assign(p, p + img.buffer_view->size);
        return;
    }
    if (!img.uri) return;
    if (!strncmp(img.uri, "data:", 5)) {
        const char* comma = strchr(img.uri, ',');
        if (!comma || !strstr(img.uri, ";base64,")) return;
        const char* b64 = comma + 1;
        size_t len = strlen(b64);
        while (len && b64[len - 1] == '=') --len;
        size_t bytes = len * 3 / 4;
        void* out = nullptr;
        if (bytes && cgltf_load_buffer_base64(&opts, bytes, b64, &out) == cgltf_result_success) {
            ts.embedded.assign((const uint8_t*)out, (const uint8_t*)out + bytes);
            free(out);
        }
        return;
    }
    std::string uri = img.uri;
    uri.resize(cgltf_decode_uri(uri.data()));
    std::wstring rel = widen(uri.c_str());
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    ts.paths.push_back(dir + rel);
    // Moved files: also try the bare name next to the model and in a textures subfolder.
    std::wstring name = rel.substr(rel.find_last_of(L'\\') + 1);
    if (name != rel) ts.paths.push_back(dir + name);
    ts.paths.push_back(dir + L"textures\\" + name);
}

} // namespace

bool load_gltf(const wchar_t* path, const void* data, size_t size, Mesh& mesh)
{
    double t0 = now_ms();
    cgltf_options opts = make_options();
    cgltf_data* g = nullptr;
    cgltf_result r = cgltf_parse(&opts, data, size, &g);
    std::string pathUtf8 = narrow(path);
    if (r == cgltf_result_success) r = cgltf_load_buffers(&opts, g, pathUtf8.c_str());
    if (r == cgltf_result_success) r = cgltf_validate(g);
    if (r != cgltf_result_success) {
        mesh.error = result_text(r);
        cgltf_free(g);
        return false;
    }
    if (const char* unsupported = required_unsupported(g)) {
        mesh.error = widen(unsupported);
        cgltf_free(g);
        return false;
    }

    double t1 = now_ms();
    mesh.parseMs = t1 - t0;

    // Material slots: the file's materials plus a default one for primitives without a material.
    const size_t numMats = g->materials_count + 1;
    const uint32_t defaultMat = (uint32_t)g->materials_count;
    auto base_texture = [](const cgltf_material* m) -> const cgltf_texture_view* {
        if (!m) return nullptr;
        if (m->has_pbr_metallic_roughness && m->pbr_metallic_roughness.base_color_texture.texture)
            return &m->pbr_metallic_roughness.base_color_texture;
        if (m->has_pbr_specular_glossiness && m->pbr_specular_glossiness.diffuse_texture.texture)
            return &m->pbr_specular_glossiness.diffuse_texture;
        return nullptr;
    };

    // Collect primitive instances.
    Streams streams;
    std::vector<Instance> insts;
    bool compressed = false;
    walk_scene(g, [&](const cgltf_node* node, const Mat4& world) {
        if (!node->mesh) return;
        for (size_t p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node->mesh->primitives[p];
            if (prim.has_draco_mesh_compression) {
                compressed = true;
                continue;
            }
            Instance in = {};
            in.prim = &prim;
            in.triCount = triangle_count(prim);
            if (!in.triCount || !streams.get(find_attribute(prim, cgltf_attribute_type_position, 0), 3, in.pos))
                continue;
            in.vertCount = find_attribute(prim, cgltf_attribute_type_position, 0)->count;
            in.hasNormal = streams.get(find_attribute(prim, cgltf_attribute_type_normal, 0), 3, in.normal);
            const cgltf_texture_view* tv = base_texture(prim.material);
            in.hasUV = streams.get(find_attribute(prim, cgltf_attribute_type_texcoord, tv ? tv->texcoord : 0), 2, in.uv);
            if (prim.indices) {
                const cgltf_accessor* ia = prim.indices;
                if (ia->is_sparse || !ia->buffer_view || !cgltf_buffer_view_data(ia->buffer_view)) continue;
                in.indices = cgltf_buffer_view_data(ia->buffer_view) + ia->offset;
                in.indexSize = cgltf_component_size(ia->component_type);
            }
            in.world = world;
            float det;
            in.normalMat = normal_matrix(world, det);
            in.mirrored = det < 0;   // negative scale flips winding
            in.material = prim.material ? (uint32_t)cgltf_material_index(g, prim.material) : defaultMat;
            insts.push_back(in);
        }
    });
    if (insts.empty()) {
        mesh.error = compressed ? L"Draco-compressed glTF is not supported" : L"no geometry in file";
        cgltf_free(g);
        return false;
    }

    // Output layout: vertices in instance order; triangles grouped by material (one draw each),
    // instances in order within a material, so every instance owns one contiguous range.
    size_t totalVerts = 0, totalTris = 0;
    for (Instance& in : insts) {
        in.outVert = totalVerts;
        totalVerts += in.vertCount;
    }
    for (uint32_t mat = 0; mat < numMats; ++mat) {
        size_t first = totalTris;
        for (Instance& in : insts)
            if (in.material == mat) {
                in.outTri = totalTris;
                totalTris += in.triCount;
            }
        if (totalTris > first)
            mesh.draws.push_back({ (uint32_t)(first * 3), (uint32_t)((totalTris - first) * 3), mat });
    }
    if (totalVerts >= UINT32_MAX || totalTris * 3 >= UINT32_MAX) {
        mesh.error = L"model too large";
        cgltf_free(g);
        return false;
    }
    mesh.vertices.resize(totalVerts);
    mesh.indices.resize(totalTris * 3);

    // Chunks of vertices and of triangles, so that one huge primitive still uses every core.
    struct Job
    {
        Instance* in;
        bool tris;            // triangle chunk, else vertex chunk
        size_t begin, end;
        Vec3 bmin, bmax;
    };
    const size_t kChunk = 64 * 1024;
    std::vector<Job> jobs;
    for (Instance& in : insts) {
        for (size_t v = 0; v < in.vertCount; v += kChunk)
            jobs.push_back({ &in, false, v, std::min(v + kChunk, in.vertCount), {}, {} });
        for (size_t t = 0; t < in.triCount; t += kChunk)
            jobs.push_back({ &in, true, t, std::min(t + kChunk, in.triCount), {}, {} });
    }

    Vertex* outV = mesh.vertices.data();
    uint32_t* outI = mesh.indices.data();
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [&](Job& j) {
        const Instance& in = *j.in;
        if (!j.tris) {
            Vec3 bmin = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            Vertex* v = outV + in.outVert + j.begin;
            for (size_t i = j.begin; i < j.end; ++i, ++v) {
                v->pos = transform_point(in.world, in.pos.at(i));
                v->normal = pack_normal(in.hasNormal ? normalize(transform_normal(in.normalMat, in.normal.at(i)))
                                                     : Vec3{ 0, 0, 0 });
                if (in.hasUV) {
                    v->u = in.uv.at(i)[0];
                    v->v = in.uv.at(i)[1];
                } else {
                    v->u = v->v = 0;
                }
                bmin = { std::fmin(bmin.x, v->pos.x), std::fmin(bmin.y, v->pos.y), std::fmin(bmin.z, v->pos.z) };
                bmax = { std::fmax(bmax.x, v->pos.x), std::fmax(bmax.y, v->pos.y), std::fmax(bmax.z, v->pos.z) };
            }
            j.bmin = bmin;
            j.bmax = bmax;
            return;
        }
        j.bmin = { FLT_MAX, FLT_MAX, FLT_MAX };
        j.bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        auto index = [&](size_t k) -> uint32_t {
            if (!in.indices) return (uint32_t)k;
            switch (in.indexSize) {
            case 1:  return in.indices[k];
            case 2:  { uint16_t x; memcpy(&x, in.indices + k * 2, 2); return x; }
            default: { uint32_t x; memcpy(&x, in.indices + k * 4, 4); return x; }
            }
        };
        const uint32_t base = (uint32_t)in.outVert, n = (uint32_t)in.vertCount;
        uint32_t* out = outI + (in.outTri + j.begin) * 3;
        for (size_t t = j.begin; t < j.end; ++t, out += 3) {
            uint32_t a, b, c;
            switch (in.prim->type) {
            case cgltf_primitive_type_triangle_strip:   // odd triangles run the other way
                a = index(t + (t & 1));
                b = index(t + 1 - (t & 1));
                c = index(t + 2);
                break;
            case cgltf_primitive_type_triangle_fan:
                a = index(0);
                b = index(t + 1);
                c = index(t + 2);
                break;
            default:
                a = index(t * 3);
                b = index(t * 3 + 1);
                c = index(t * 3 + 2);
            }
            if (a >= n || b >= n || c >= n) a = b = c = 0;   // out of range: degenerate, not a crash
            if (in.mirrored) std::swap(b, c);
            out[0] = base + a;
            out[1] = base + b;
            out[2] = base + c;
        }
    });

    // Primitives without normals get smooth ones from area-weighted face normals (glTF says flat,
    // but vertices are shared here, and smooth reads better than black).
    std::for_each(std::execution::par, insts.begin(), insts.end(), [&](const Instance& in) {
        if (in.hasNormal) return;
        std::vector<Vec3> acc(in.vertCount, Vec3{ 0, 0, 0 });
        const uint32_t* tri = outI + in.outTri * 3;
        for (size_t t = 0; t < in.triCount; ++t, tri += 3) {
            Vec3 fn = cross(outV[tri[1]].pos - outV[tri[0]].pos, outV[tri[2]].pos - outV[tri[0]].pos);
            for (int k = 0; k < 3; ++k) acc[tri[k] - in.outVert] = acc[tri[k] - in.outVert] + fn;
        }
        for (size_t i = 0; i < in.vertCount; ++i) {
            Vec3 nrm = dot(acc[i], acc[i]) > 0 ? normalize(acc[i]) : Vec3{ 0, 1, 0 };
            outV[in.outVert + i].normal = pack_normal(nrm);
        }
    });

    Vec3 bmin = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const Job& j : jobs) {
        if (j.tris) continue;
        bmin = { std::fmin(bmin.x, j.bmin.x), std::fmin(bmin.y, j.bmin.y), std::fmin(bmin.z, j.bmin.z) };
        bmax = { std::fmax(bmax.x, j.bmax.x), std::fmax(bmax.y, j.bmax.y), std::fmax(bmax.z, j.bmax.z) };
    }

    // Materials, and the images they use as base color. Only those images are described (normal,
    // roughness and other maps are never drawn); decoding happens after the first frame.
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    mesh.textures.resize(g->images_count);
    mesh.materials.resize(numMats);
    for (size_t i = 0; i < g->materials_count; ++i) {
        const cgltf_material& m = g->materials[i];
        Material& out = mesh.materials[i];
        const float* c = m.has_pbr_metallic_roughness ? m.pbr_metallic_roughness.base_color_factor
                       : m.has_pbr_specular_glossiness ? m.pbr_specular_glossiness.diffuse_factor
                       : nullptr;
        if (c) out.color = { linear_to_srgb(c[0]), linear_to_srgb(c[1]), linear_to_srgb(c[2]) };
        if (const cgltf_texture_view* tv = base_texture(&m)) {
            const cgltf_image* img = tv->texture->image ? tv->texture->image : tv->texture->webp_image;
            if (img) {
                size_t idx = cgltf_image_index(g, img);
                TextureSource& ts = mesh.textures[idx];
                if (ts.embedded.empty() && ts.paths.empty()) describe_image(opts, *img, dir, ts);
                out.texture = (int)idx;
            }
        }
        out.alphaTest = m.alpha_mode == cgltf_alpha_mode_mask;   // BLEND: cut-outs are auto-detected
    }

    debug_log(L"load %s: glTF, %zu instances, %zu materials, %zu images, %zu draws", path, insts.size(),
              g->materials_count, g->images_count, mesh.draws.size());
    cgltf_free(g);
    mesh.buildMs = now_ms() - t1;
    mesh.bmin = bmin;
    mesh.bmax = bmax;
    return true;
}

bool gltf_count_triangles(const wchar_t* path, uint64_t& triangles)
{
    // Only the JSON is needed: the whole file for .gltf, the first chunk for .glb.
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    uint8_t head[20] = {};
    DWORD got = 0;
    ReadFile(f, head, sizeof(head), &got, nullptr);
    std::vector<uint8_t> buf;
    if (got == sizeof(head) && !memcmp(head, "glTF", 4)) {
        uint32_t jsonLen;
        memcpy(&jsonLen, head + 12, 4);
        if (jsonLen < (64u << 20) && 20ull + jsonLen <= (uint64_t)size.QuadPart) {
            buf.resize(20 + jsonLen);
            memcpy(buf.data(), head, 20);
            uint32_t total = (uint32_t)buf.size();   // pretend the file ends after the JSON chunk
            memcpy(buf.data() + 8, &total, 4);
            if (!ReadFile(f, buf.data() + 20, jsonLen, &got, nullptr) || got != jsonLen) buf.clear();
        }
    } else if (size.QuadPart < (64ll << 20)) {
        buf.resize((size_t)size.QuadPart);
        memcpy(buf.data(), head, got);
        DWORD rest = (DWORD)(buf.size() - got);
        if (rest && (!ReadFile(f, buf.data() + got, rest, &got, nullptr) || got != rest)) buf.clear();
    }
    CloseHandle(f);
    if (buf.empty()) return false;

    cgltf_options opts = make_options();
    cgltf_data* g = nullptr;
    if (cgltf_parse(&opts, buf.data(), buf.size(), &g) != cgltf_result_success) return false;
    uint64_t n = 0;
    walk_scene(g, [&](const cgltf_node* node, const Mat4&) {
        if (node->mesh)
            for (size_t p = 0; p < node->mesh->primitives_count; ++p) n += triangle_count(node->mesh->primitives[p]);
    });
    cgltf_free(g);
    triangles = n;
    return true;
}
