// FBX -> flat world-space triangle list, via ufbx.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "mesh.h"
#include "ufbx.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cwchar>
#include <execution>

namespace {

// Read-only memory mapping of a whole file.
struct MappedFile
{
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const void* data = nullptr;
    size_t size = 0;

    ~MappedFile()
    {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    bool open(const wchar_t* path)
    {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(file, &sz)) return false;
        size = (size_t)sz.QuadPart;
        if (!size) return false;
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;
        data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        return data != nullptr;
    }
};

Vec3 to_vec3(ufbx_vec3 v) { return { (float)v.x, (float)v.y, (float)v.z }; }

std::wstring widen(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// ufbx thread pool on top of the Windows system thread pool. ufbx uses it to
// decompress and parse large arrays in parallel.
struct Pool
{
    struct Group
    {
        ufbx_thread_pool_context ctx;
        std::atomic<uint32_t> next;
        uint32_t end;
        std::atomic<uint32_t> workers;
    };
    Group groups[UFBX_THREAD_GROUP_COUNT];
    uint32_t maxWorkers;

    static void CALLBACK worker(PTP_CALLBACK_INSTANCE, void* p)
    {
        Group* g = (Group*)p;
        for (uint32_t i; (i = g->next.fetch_add(1)) < g->end;)
            ufbx_thread_pool_run_task(g->ctx, i);
        if (g->workers.fetch_sub(1) == 1) g->workers.notify_all();
    }

    static void run(void* user, ufbx_thread_pool_context ctx, uint32_t group, uint32_t start, uint32_t count)
    {
        Pool* pool = (Pool*)user;
        Group& g = pool->groups[group];
        g.ctx = ctx;
        g.next = start;
        g.end = start + count;
        uint32_t n = count < pool->maxWorkers ? count : pool->maxWorkers;
        g.workers = n;
        for (uint32_t i = 0; i < n; ++i)
            if (!TrySubmitThreadpoolCallback(worker, &g, nullptr))
                worker(nullptr, &g);   // fall back to running inline
    }

    static void wait(void* user, ufbx_thread_pool_context, uint32_t group, uint32_t)
    {
        Group& g = ((Pool*)user)->groups[group];
        for (uint32_t w; (w = g.workers.load()) != 0;)
            g.workers.wait(w);
    }
};

std::string narrow(const wchar_t* s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    return r;
}

} // namespace

void debug_log(const wchar_t* fmt, ...)
{
    static const bool enabled = GetEnvironmentVariableW(L"NOTIMEFBX_LOG", nullptr, 0) != 0;
    if (!enabled) return;
    static SRWLOCK lock = SRWLOCK_INIT;
    wchar_t line[2048];
    va_list args;
    va_start(args, fmt);
    int n = _vsnwprintf_s(line, _countof(line), _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) n = (int)wcslen(line);
    std::string utf8 = narrow(line) + "\r\n";

    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"NoTimeFbx.log");
    AcquireSRWLockExclusive(&lock);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(f);
    }
    ReleaseSRWLockExclusive(&lock);
}

double now_ms()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

bool load_mesh(const wchar_t* path, Mesh& mesh)
{
    double t0 = now_ms();
    if (cache_load(path, mesh)) {
        mesh.parseMs = now_ms() - t0;
        debug_log(L"load %s: from cache", path);
        return true;
    }

    MappedFile file;
    if (!file.open(path)) {
        mesh.error = file.file != INVALID_HANDLE_VALUE && file.size == 0 ? L"empty file" : L"cannot open file";
        return false;
    }

    const wchar_t* ext = wcsrchr(path, L'.');
    if (ext && !_wcsicmp(ext, L".stl")) return load_stl(file.data, file.size, mesh);

    ufbx_load_opts opts = {};
    opts.ignore_animation = true;
    opts.load_external_files = false;
    opts.skip_skin_vertices = true;
    opts.generate_missing_normals = true;
    opts.target_axes = ufbx_axes_right_handed_y_up;

    Pool pool = {};
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    pool.maxWorkers = si.dwNumberOfProcessors > 1 ? si.dwNumberOfProcessors : 1;
    opts.thread_opts.pool.run_fn = Pool::run;
    opts.thread_opts.pool.wait_fn = Pool::wait;
    opts.thread_opts.pool.user = &pool;

    // Lets ufbx resolve texture paths relative to the .fbx.
    std::string pathUtf8 = narrow(path);
    opts.filename.data = pathUtf8.c_str();
    opts.filename.length = pathUtf8.size();

    ufbx_error err;
    ufbx_scene* scene = ufbx_load_memory(file.data, file.size, &opts, &err);
    if (!scene) {
        char buf[512];
        ufbx_format_error(buf, sizeof(buf), &err);
        // Keep only the first line (the rest is a stack trace), without the "ufbx vX error: " prefix.
        for (char* p = buf; *p; ++p)
            if (*p == '\n') { *p = 0; break; }
        const char* msg = strstr(buf, "error: ");
        mesh.error = widen(msg ? msg + 7 : buf);
        return false;
    }

    double t1 = now_ms();
    mesh.parseMs = t1 - t0;

    // Global material slots: scene materials plus a default one for faces without a material.
    const size_t numMats = scene->materials.count + 1;
    const uint32_t defaultMat = (uint32_t)scene->materials.count;

    // Split all meshes into chunks of faces so that one huge mesh still uses every core.
    struct Job
    {
        const ufbx_node* node;
        size_t faceBegin, faceEnd;
        size_t vertBegin, vertEnd;   // face-corner range inside the mesh
        size_t outVert;              // offset into the vertex buffer
        std::vector<size_t> tris;    // triangles per material; then output cursor per material
        Vec3 bmin, bmax;
    };
    std::vector<Job> jobs;
    const size_t kChunkFaces = 32 * 1024;
    for (const ufbx_node* node : scene->nodes) {
        const ufbx_mesh* m = node->mesh;
        if (!m || !m->num_triangles) continue;
        for (size_t f = 0; f < m->faces.count; f += kChunkFaces) {
            Job j = {};
            j.node = node;
            j.faceBegin = f;
            j.faceEnd = f + kChunkFaces < m->faces.count ? f + kChunkFaces : m->faces.count;
            j.vertBegin = m->faces.data[j.faceBegin].index_begin;
            j.vertEnd = j.faceEnd < m->faces.count ? m->faces.data[j.faceEnd].index_begin : m->num_indices;
            jobs.push_back(std::move(j));
        }
    }
    if (jobs.empty()) {
        ufbx_free_scene(scene);
        mesh.error = L"no geometry in file";
        return false;
    }

    // Material of a face, as a global slot. Per-instance node materials override mesh materials.
    auto face_material = [defaultMat](const ufbx_node* node, const ufbx_mesh* m, size_t f) -> uint32_t {
        const ufbx_material_list& list = node->materials.count ? node->materials : m->materials;
        uint32_t local = m->face_material.count ? m->face_material.data[f] : 0;
        const ufbx_material* mat = local < list.count ? list.data[local] : nullptr;
        return mat ? mat->typed_id : defaultMat;
    };

    // Pass 1: count triangles per chunk and material.
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [&](Job& j) {
        const ufbx_mesh* m = j.node->mesh;
        j.tris.assign(numMats, 0);
        for (size_t f = j.faceBegin; f < j.faceEnd; ++f)
            if (m->faces.data[f].num_indices >= 3)
                j.tris[face_material(j.node, m, f)] += m->faces.data[f].num_indices - 2;
    });

    // Output offsets: triangles grouped by material (one draw each), chunks in order within a material.
    size_t totalVerts = 0, totalTris = 0;
    for (Job& j : jobs) {
        j.outVert = totalVerts;
        totalVerts += j.vertEnd - j.vertBegin;
    }
    for (uint32_t mat = 0; mat < numMats; ++mat) {
        size_t first = totalTris;
        for (Job& j : jobs) {
            size_t n = j.tris[mat];
            j.tris[mat] = totalTris;   // becomes this chunk's write cursor
            totalTris += n;
        }
        if (totalTris > first)
            mesh.draws.push_back({ (uint32_t)(first * 3), (uint32_t)((totalTris - first) * 3), mat });
    }
    mesh.vertices.resize(totalVerts);
    mesh.indices.resize(totalTris * 3);

    // Pass 2: fill vertices and indices.
    Vertex* outV = mesh.vertices.data();
    uint32_t* outI = mesh.indices.data();
    std::for_each(std::execution::par, jobs.begin(), jobs.end(), [&](Job& j) {
        const ufbx_node* node = j.node;
        const ufbx_mesh* m = node->mesh;
        const ufbx_matrix& world = node->geometry_to_world;
        ufbx_matrix normalMat = ufbx_matrix_for_normals(&world);
        bool mirrored = ufbx_matrix_determinant(&world) < 0;   // negative scale flips winding
        bool hasNormals = m->vertex_normal.exists;
        bool hasUV = m->vertex_uv.exists;

        // One output vertex per face corner; triangulation only adds indices.
        Vec3 bmin = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        Vertex* v = outV + j.outVert;
        for (size_t i = j.vertBegin; i < j.vertEnd; ++i, ++v) {
            v->pos = to_vec3(ufbx_transform_position(&world, ufbx_get_vertex_vec3(&m->vertex_position, i)));
            Vec3 n = hasNormals
                ? normalize(to_vec3(ufbx_transform_direction(&normalMat, ufbx_get_vertex_vec3(&m->vertex_normal, i))))
                : Vec3{ 0, 1, 0 };
            v->normal = pack_normal(n);
            if (hasUV) {
                ufbx_vec2 uv = ufbx_get_vertex_vec2(&m->vertex_uv, i);
                v->u = (float)uv.x;
                v->v = 1.0f - (float)uv.y;   // FBX V points up, D3D V points down
            } else {
                v->u = v->v = 0;
            }
            bmin = { std::fmin(bmin.x, v->pos.x), std::fmin(bmin.y, v->pos.y), std::fmin(bmin.z, v->pos.z) };
            bmax = { std::fmax(bmax.x, v->pos.x), std::fmax(bmax.y, v->pos.y), std::fmax(bmax.z, v->pos.z) };
        }
        j.bmin = bmin;
        j.bmax = bmax;

        // ufbx triangle indices are face-corner indices within the mesh.
        uint32_t base = (uint32_t)(j.outVert - j.vertBegin);
        std::vector<uint32_t> tri(m->max_face_triangles * 3);
        for (size_t f = j.faceBegin; f < j.faceEnd; ++f) {
            uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), m, m->faces.data[f]);
            if (!n) continue;
            size_t& cursor = j.tris[face_material(node, m, f)];
            uint32_t* out = outI + cursor * 3;
            cursor += n;
            for (uint32_t t = 0; t < n; ++t, out += 3) {
                out[0] = base + tri[t * 3];
                out[1] = base + tri[t * 3 + (mirrored ? 2 : 1)];
                out[2] = base + tri[t * 3 + (mirrored ? 1 : 2)];
            }
        }
    });

    Vec3 bmin = jobs[0].bmin, bmax = jobs[0].bmax;
    for (const Job& j : jobs) {
        bmin = { std::fmin(bmin.x, j.bmin.x), std::fmin(bmin.y, j.bmin.y), std::fmin(bmin.z, j.bmin.z) };
        bmax = { std::fmax(bmax.x, j.bmax.x), std::fmax(bmax.y, j.bmax.y), std::fmax(bmax.z, j.bmax.z) };
    }

    // Materials and the texture files they reference. Textures are only described here;
    // decoding happens later, after the first frame is on screen.
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    mesh.textures.resize(scene->texture_files.count);
    for (const ufbx_texture_file& tf : scene->texture_files) {
        TextureSource& ts = mesh.textures[tf.index];
        if (tf.content.size) {
            const uint8_t* p = (const uint8_t*)tf.content.data;
            ts.embedded.assign(p, p + tf.content.size);
        }
        // Candidate locations, best first: resolved relative path, stored absolute path,
        // and finally the bare file name next to the .fbx (for files moved between machines).
        for (const ufbx_string* str : { &tf.filename, &tf.absolute_filename, &tf.relative_filename })
            if (str->length) ts.paths.push_back(widen(str->data));
        if (!ts.paths.empty()) {
            std::wstring name = ts.paths.back();
            name = name.substr(name.find_last_of(L"\\/") + 1);
            ts.paths.push_back(dir + name);
            ts.paths.push_back(dir + L"textures\\" + name);
        }
    }

    mesh.materials.resize(numMats);
    for (const ufbx_material* mat : scene->materials) {
        Material& out = mesh.materials[mat->typed_id];
        const ufbx_material_map& base = mat->pbr.base_color;
        if (base.has_value)
            out.color = { (float)base.value_vec4.x, (float)base.value_vec4.y, (float)base.value_vec4.z };
        const ufbx_texture* tex = base.texture ? base.texture : mat->fbx.diffuse_color.texture;
        if (tex && tex->file_textures.count && tex->file_textures.data[0]->has_file)
            out.texture = (int)tex->file_textures.data[0]->file_index;
        // Cut-out transparency (foliage, fences): only when the material has an opacity map.
        out.alphaTest = mat->pbr.opacity.texture != nullptr || mat->fbx.transparency_factor.texture != nullptr;
    }

    debug_log(L"load %s: %zu materials, %zu texture files, %zu draws", path, scene->materials.count,
              scene->texture_files.count, mesh.draws.size());
    for (const ufbx_material* mat : scene->materials) {
        const Material& m = mesh.materials[mat->typed_id];
        debug_log(L"  material %u '%s': color %.2f %.2f %.2f texture %d alphaTest %d", mat->typed_id,
                  widen(mat->name.data).c_str(), m.color.x, m.color.y, m.color.z, m.texture, (int)m.alphaTest);
    }
    for (size_t i = 0; i < mesh.textures.size(); ++i) {
        debug_log(L"  texture %zu: embedded %zu bytes", i, mesh.textures[i].embedded.size());
        for (const std::wstring& c : mesh.textures[i].paths) debug_log(L"    candidate %s", c.c_str());
    }

    ufbx_free_scene(scene);
    mesh.buildMs = now_ms() - t1;

    if (mesh.indices.empty()) {
        mesh.error = L"no geometry in file";
        return false;
    }
    mesh.bmin = bmin;
    mesh.bmax = bmax;
    return true;
}
