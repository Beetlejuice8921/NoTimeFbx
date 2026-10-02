// STL (binary and ASCII) -> flat world-space triangle list.
//
// STL carries nothing but triangles and per-facet normals: no materials, textures or UVs, so the
// mesh gets one default material and one draw. Like the FBX path, every face corner becomes its
// own vertex (no welding); facet normals give flat shading, and a missing/zero normal is replaced
// by the face cross product. STL files are Z-up (3D printing convention); positions and normals
// are rotated upright to match the viewer's Y-up. Binary files are filled in parallel from the
// memory mapping; ASCII files are parsed in one sequential pass with a hand-rolled float parser.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "mesh.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>
#include <execution>
#include <vector>

namespace {

// STL is Z-up, the viewer is Y-up: rotate -90 degrees about X, (x, y, z) -> (x, z, -y). The
// rotation keeps handedness, so facet winding and normals survive it.
Vec3 y_up(Vec3 p) { return { p.x, p.z, -p.y }; }

// Powers of 10 for parse_float(), built by repeated multiply: the error stays far below float
// rounding even at the ends of the table.
double pow10(int e)
{
    static const std::array<double, 801> table = [] {
        std::array<double, 801> t = {};
        t[400] = 1.0;
        for (int i = 1; i <= 400; ++i) {
            t[400 + i] = t[400 + i - 1] * 10.0;
            t[400 - i] = t[400 - i + 1] * 0.1;
        }
        return t;
    }();
    return table[std::clamp(e, -400, 400) + 400];
}

const char* skip_spaces(const char* p, const char* end)
{
    while (p < end && *p <= ' ') ++p;
    return p;
}

// Fast decimal float parser for ASCII STL ("1.234567e+00"): strtod is locale-aware and an order
// of magnitude slower, and these files hold nine floats per facet. Leading whitespace is skipped,
// since the caller only knows that a number comes next.
bool parse_float(const char*& p, const char* end, float& out)
{
    p = skip_spaces(p, end);
    bool neg = false;
    if (p < end && (*p == '+' || *p == '-')) {
        neg = *p == '-';
        ++p;
    }
    uint64_t mant = 0;
    int exp10 = 0, digits = 0;
    bool dot = false;
    for (; p < end; ++p) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            // Keep at most 19 digits in the mantissa; anything past float precision is dropped.
            if (++digits <= 19) {
                mant = mant * 10 + (uint64_t)(c - '0');
                if (dot) --exp10;
            }
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            break;
        }
    }
    if (!digits) return false;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool eneg = false;
        if (q < end && (*q == '+' || *q == '-')) eneg = *q++ == '-';
        if (q < end && *q >= '0' && *q <= '9') {
            int e = 0;
            for (; q < end && *q >= '0' && *q <= '9'; ++q) e = std::min(e * 10 + (*q - '0'), 400);
            exp10 += eneg ? -e : e;
            p = q;
        }   // a bare e/E ends the number rather than failing the file
    }
    double v = (double)mant * pow10(exp10);
    out = neg ? -(float)v : (float)v;
    return true;
}

// ASCII keywords are lower case by convention, but match case-insensitively just in case.
bool is_word(const char* w, size_t len, const char* keyword)
{
    size_t n = strlen(keyword);
    return len == n && !_strnicmp(w, keyword, n);
}

// Parses vertices and facet normals. Returns false on a malformed number. `inverted` receives one
// flag per complete facet: whether its winding runs opposite to its stored normal.
bool parse_ascii(const char* p, const char* end, std::vector<Vertex>& verts, std::vector<char>& inverted)
{
    // Reserve for the facet count first so a large file doesn't grow the vector repeatedly.
    size_t facets = 0;
    for (const char* q = p; (q = skip_spaces(q, end)) < end;) {
        const char* w = q;
        while (q < end && *q > ' ') ++q;
        facets += is_word(w, q - w, "facet");
    }
    verts.reserve(facets * 3);

    const uint32_t zeroNormal = pack_normal({ 0, 0, 0 });
    Vec3 normal = { 0, 0, 0 };
    bool hasNormal = false;
    while ((p = skip_spaces(p, end)) < end) {
        const char* w = p;
        while (p < end && *p > ' ') ++p;
        size_t len = p - w;
        if (is_word(w, len, "vertex")) {
            float x, y, z;
            if (!parse_float(p, end, x) || !parse_float(p, end, y) || !parse_float(p, end, z))
                return false;
            Vec3 n = hasNormal ? y_up(normal) : Vec3{ 0, 0, 0 };
            if (hasNormal) n = normalize(n);
            verts.push_back({ y_up({ x, y, z }), hasNormal ? pack_normal(n) : zeroNormal, 0, 0 });
            if (verts.size() % 3 == 0) {   // facet complete: winding vs stored normal
                const Vertex* v = &verts[verts.size() - 3];
                Vec3 cn = cross(v[1].pos - v[0].pos, v[2].pos - v[0].pos);   // already world space
                inverted.push_back(hasNormal && dot(cn, cn) > 1e-20f &&
                                   dot(normalize(y_up(normal)), cn) < 0);
            }
        } else if (is_word(w, len, "facet")) {
            hasNormal = false;
        } else if (is_word(w, len, "normal")) {
            float x, y, z;
            if (parse_float(p, end, x) && parse_float(p, end, y) && parse_float(p, end, z)) {
                normal = { x, y, z };
                hasNormal = dot(normal, normal) > 1e-20f;
            }
        }   // "solid", "outer", "loop", "endloop", "endfacet", names: ignored
    }
    return true;
}

void load_ascii(const char* data, size_t size, Mesh& mesh, double t0)
{
    std::vector<Vertex> verts;
    std::vector<char> inverted;
    if (!parse_ascii(data, data + size, verts, inverted)) {
        mesh.error = L"corrupt ASCII STL";
        return;
    }
    double t1 = now_ms();
    mesh.parseMs = t1 - t0;

    verts.resize(verts.size() - verts.size() % 3);   // drop an incomplete trailing facet, if any

    // Patch facets that came without a usable normal, and collect bounds.
    const uint32_t zeroNormal = pack_normal({ 0, 0, 0 });
    Vec3 bmin = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (size_t i = 0; i < verts.size(); i += 3) {
        if (verts[i].normal == zeroNormal) {
            Vec3 n = normalize(cross(verts[i + 1].pos - verts[i].pos, verts[i + 2].pos - verts[i].pos));
            verts[i].normal = verts[i + 1].normal = verts[i + 2].normal = pack_normal(n);
        }
        for (size_t k = 0; k < 3; ++k) {
            const Vec3& v = verts[i + k].pos;
            bmin = { std::fmin(bmin.x, v.x), std::fmin(bmin.y, v.y), std::fmin(bmin.z, v.z) };
            bmax = { std::fmax(bmax.x, v.x), std::fmax(bmax.y, v.y), std::fmax(bmax.z, v.z) };
        }
    }

    mesh.vertices.resize(verts.size());
    if (verts.size()) memcpy(mesh.vertices.data(), verts.data(), verts.size() * sizeof(Vertex));
    mesh.indices.resize(verts.size());
    for (size_t f = 0; f < verts.size() / 3; ++f) {   // inverted facets reverse winding, see load_binary
        uint32_t b = (uint32_t)f * 3;
        bool flip = f < inverted.size() && inverted[f];
        mesh.indices[b] = flip ? b + 2 : b;
        mesh.indices[b + 1] = b + 1;
        mesh.indices[b + 2] = flip ? b : b + 2;
    }
    if (verts.empty()) {
        mesh.error = L"no geometry in file";
        return;
    }
    mesh.bmin = bmin;
    mesh.bmax = bmax;
    mesh.buildMs = now_ms() - t1;
}

void load_binary(const uint8_t* data, uint64_t tris, Mesh& mesh, double t0)
{
    double t1 = now_ms();
    mesh.parseMs = t1 - t0;   // binary STL has no separate parse stage to speak of

    mesh.vertices.resize(tris * 3);
    mesh.indices.resize(tris * 3);

    // One vertex per face corner with the facet normal; indices are sequential. Split into chunks
    // so that one huge file still uses every core.
    struct Job
    {
        size_t first, outVert;
        Vec3   bmin, bmax;
    };
    const size_t kChunkTris = 32 * 1024;
    std::vector<Job> jobs;
    for (size_t t = 0; t < tris; t += kChunkTris)
        jobs.push_back({ t, t * 3, { FLT_MAX, FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX, -FLT_MAX } });

    const uint8_t* base = data + 84;
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [&](Job& j) {
        Vertex* v = mesh.vertices.data() + j.outVert;
        uint32_t* idx = mesh.indices.data() + j.outVert;
        const uint8_t* tri = base + j.first * 50;
        Vec3 bmin = j.bmin, bmax = j.bmax;
        uint32_t vi = (uint32_t)j.outVert;
        for (size_t t = j.first, e = std::min(j.first + kChunkTris, tris); t < e; ++t) {
            float f[12];
            memcpy(f, tri, sizeof(f));
            tri += 50;
            Vec3 p0 = y_up({ f[3], f[4], f[5] }), p1 = y_up({ f[6], f[7], f[8] }),
                 p2 = y_up({ f[9], f[10], f[11] });
            // The fallback cross product follows the file's winding, so it is taken on the raw
            // corners and rotated once together with the stored normal below (the corners above
            // are already rotated; crossing them would give a world-space normal).
            Vec3 e1 = { f[6] - f[3], f[7] - f[4], f[8] - f[5] };
            Vec3 e2 = { f[9] - f[3], f[10] - f[4], f[11] - f[5] };
            Vec3 n = { f[0], f[1], f[2] };
            // Scans and converted files often carry facets whose winding runs opposite to their
            // stored normal. The pixel shader flips back-face normals, which would render such a
            // facet black; reversing its indices instead makes the rasterizer see it front-facing,
            // so the stored normal is used as-is.
            bool invert = false;
            if (dot(n, n) > 1e-20f) {
                invert = dot(n, cross(e1, e2)) < 0;
            } else {
                n = cross(e1, e2);
            }
            uint32_t pn = pack_normal(normalize(y_up(n)));
            *v++ = { p0, pn, 0, 0 };
            *v++ = { p1, pn, 0, 0 };
            *v++ = { p2, pn, 0, 0 };
            if (invert) {
                idx[0] = vi + 2;
                idx[1] = vi + 1;
                idx[2] = vi;
            } else {
                idx[0] = vi;
                idx[1] = vi + 1;
                idx[2] = vi + 2;
            }
            vi += 3;
            idx += 3;
            bmin = { std::fmin(bmin.x, p0.x), std::fmin(bmin.y, p0.y), std::fmin(bmin.z, p0.z) };
            bmax = { std::fmax(bmax.x, p0.x), std::fmax(bmax.y, p0.y), std::fmax(bmax.z, p0.z) };
            bmin = { std::fmin(bmin.x, p1.x), std::fmin(bmin.y, p1.y), std::fmin(bmin.z, p1.z) };
            bmax = { std::fmax(bmax.x, p1.x), std::fmax(bmax.y, p1.y), std::fmax(bmax.z, p1.z) };
            bmin = { std::fmin(bmin.x, p2.x), std::fmin(bmin.y, p2.y), std::fmin(bmin.z, p2.z) };
            bmax = { std::fmax(bmax.x, p2.x), std::fmax(bmax.y, p2.y), std::fmax(bmax.z, p2.z) };
        }
        j.bmin = bmin;
        j.bmax = bmax;
    });

    Vec3 bmin = jobs[0].bmin, bmax = jobs[0].bmax;
    for (const Job& j : jobs) {
        bmin = { std::fmin(bmin.x, j.bmin.x), std::fmin(bmin.y, j.bmin.y), std::fmin(bmin.z, j.bmin.z) };
        bmax = { std::fmax(bmax.x, j.bmax.x), std::fmax(bmax.y, j.bmax.y), std::fmax(bmax.z, j.bmax.z) };
    }
    mesh.bmin = bmin;
    mesh.bmax = bmax;
    mesh.buildMs = now_ms() - t1;
}

} // namespace

bool load_stl(const void* data, size_t size, Mesh& mesh)
{
    double t0 = now_ms();

    // Binary layout: an 80-byte header, a little-endian triangle count, then exactly 50 bytes per
    // triangle. The exact size match identifies binary even when the header begins with "solid",
    // which many writers put there. Trailing bytes beyond the last triangle are tolerated.
    auto starts_solid = [&] { return size >= 5 && !_strnicmp((const char*)data, "solid", 5); };
    if (size >= 84) {
        const uint8_t* u = (const uint8_t*)data;
        uint64_t count = u[80] | (uint64_t)u[81] << 8 | (uint64_t)u[82] << 16 | (uint64_t)u[83] << 24;
        if (84 + 50 * count == size) {
            load_binary((const uint8_t*)data, count, mesh, t0);
        } else if (starts_solid()) {
            load_ascii((const char*)data, size, mesh, t0);
        } else if (84 + 50 * count < size) {
            load_binary((const uint8_t*)data, count, mesh, t0);
        } else {
            mesh.error = L"not a valid STL file";
            return false;
        }
    } else if (starts_solid()) {
        load_ascii((const char*)data, size, mesh, t0);
    } else {
        mesh.error = L"not a valid STL file";
        return false;
    }
    if (!mesh.error.empty()) return false;

    mesh.materials.assign(1, Material{});   // default gray; STL has no materials of its own
    mesh.draws.push_back({ 0, (uint32_t)mesh.indices.size(), 0 });
    debug_log(L"load STL: %uk triangles", mesh.indices.size() / 3000);
    return true;
}
