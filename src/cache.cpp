// On-disk cache of fully built meshes: a second open of a large .fbx skips parsing entirely.
//
// Location: %LOCALAPPDATA%\NoTimeFbx\cache\<hash of path>.bin. An entry is valid only if the source
// file's size and modification time match, and the format version matches. Entries are written in the
// background after the model is on screen, atomically (temp file + rename), and the directory is
// trimmed to kMaxCacheBytes by evicting the least recently used entries.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include "mesh.h"

#include <algorithm>

#pragma comment(lib, "shell32.lib")

namespace {

const uint32_t kVersion = 1 + (uint32_t)sizeof(Vertex) * 1000;   // bump when the layout changes
const uint64_t kMinSourceBytes = 8ull << 20;                     // small files load fast anyway
const uint64_t kMaxCacheBytes = 2ull << 30;

struct Header
{
    char     magic[8];
    uint32_t version;
    uint32_t reserved;
    uint64_t srcSize;
    uint64_t srcTime;
    Vec3     bmin, bmax;
    uint64_t numVerts, numIndices, numDraws, numMaterials, numTextures;
};

struct DiskMaterial
{
    Vec3     color;
    int32_t  texture;
    uint32_t alphaTest;
};

bool source_info(const wchar_t* path, uint64_t& size, uint64_t& time)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return false;
    size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    time = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

std::wstring cache_dir()
{
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\NoTimeFbx\\cache\\";
        CoTaskMemFree(base);
    }
    return dir;
}

std::wstring cache_file(const wchar_t* path)
{
    std::wstring dir = cache_dir();
    if (dir.empty()) return dir;
    // FNV-1a over the case-folded full path.
    wchar_t full[MAX_PATH * 2];
    if (!GetFullPathNameW(path, _countof(full), full, nullptr)) return L"";
    CharLowerW(full);
    uint64_t h = 1469598103934665603ull;
    for (const wchar_t* p = full; *p; ++p) {
        h ^= (uint16_t)*p;
        h *= 1099511628211ull;
    }
    wchar_t name[32];
    swprintf(name, 32, L"%016llx.bin", (unsigned long long)h);
    return dir + name;
}

bool disabled()
{
    static const bool off = GetEnvironmentVariableW(L"NOTIMEFBX_NOCACHE", nullptr, 0) != 0;
    return off;
}

// Buffered sequential file I/O with a sticky error flag.
struct File
{
    HANDLE h = INVALID_HANDLE_VALUE;
    bool ok = true;

    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }

    void read(void* dst, uint64_t bytes)
    {
        for (uint8_t* p = (uint8_t*)dst; ok && bytes;) {
            DWORD chunk = (DWORD)std::min<uint64_t>(bytes, 1u << 30), got = 0;
            ok = ReadFile(h, p, chunk, &got, nullptr) && got == chunk;
            p += chunk;
            bytes -= chunk;
        }
    }

    void write(const void* src, uint64_t bytes)
    {
        for (const uint8_t* p = (const uint8_t*)src; ok && bytes;) {
            DWORD chunk = (DWORD)std::min<uint64_t>(bytes, 1u << 30), put = 0;
            ok = WriteFile(h, p, chunk, &put, nullptr) && put == chunk;
            p += chunk;
            bytes -= chunk;
        }
    }

    template <class T> void read(T& v) { read(&v, sizeof(T)); }
    template <class T> void write(const T& v) { write(&v, sizeof(T)); }
};

void trim_cache(const std::wstring& dir)
{
    struct Entry { std::wstring path; uint64_t size, time; };
    std::vector<Entry> entries;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"*.bin").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        uint64_t time = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        entries.push_back({ dir + fd.cFileName, size, time });
        total += size;
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const Entry& e : entries) {
        if (total <= kMaxCacheBytes) break;
        if (DeleteFileW(e.path.c_str())) total -= e.size;
    }
}

// Opens the cache entry for `path` and reads its header; false unless the entry is valid for the
// current source file. Readers share delete access so that a concurrent cache_store() can replace it.
bool open_entry(const wchar_t* path, DWORD access, File& f, Header& hdr)
{
    uint64_t srcSize, srcTime;
    if (disabled() || !source_info(path, srcSize, srcTime) || srcSize < kMinSourceBytes) return false;
    std::wstring name = cache_file(path);
    if (name.empty()) return false;

    f.h = CreateFileW(name.c_str(), access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) return false;
    f.read(hdr);
    return f.ok && memcmp(hdr.magic, "FBXVCACH", 8) == 0 && hdr.version == kVersion && hdr.srcSize == srcSize &&
           hdr.srcTime == srcTime;
}

} // namespace

bool cache_peek(const wchar_t* path, uint64_t& triangles)
{
    File f;
    Header hdr;
    if (!open_entry(path, GENERIC_READ, f, hdr)) return false;
    triangles = hdr.numIndices / 3;
    return true;
}

bool cache_load(const wchar_t* path, Mesh& mesh)
{
    File f;
    Header hdr;
    if (!open_entry(path, GENERIC_READ | FILE_WRITE_ATTRIBUTES, f, hdr)) return false;

    Mesh m;
    m.bmin = hdr.bmin;
    m.bmax = hdr.bmax;
    m.vertices.resize(hdr.numVerts);
    m.indices.resize(hdr.numIndices);
    m.draws.resize(hdr.numDraws);
    f.read(m.vertices.data(), hdr.numVerts * sizeof(Vertex));
    f.read(m.indices.data(), hdr.numIndices * sizeof(uint32_t));
    f.read(m.draws.data(), hdr.numDraws * sizeof(Draw));

    m.materials.resize(hdr.numMaterials);
    for (Material& mat : m.materials) {
        DiskMaterial dm;
        f.read(dm);
        mat.color = dm.color;
        mat.texture = dm.texture;
        mat.alphaTest = dm.alphaTest != 0;
    }
    m.textures.resize(hdr.numTextures);
    for (TextureSource& t : m.textures) {
        uint64_t embedded = 0;
        uint32_t numPaths = 0;
        f.read(embedded);
        if (!f.ok || embedded > (1ull << 31)) return false;
        t.embedded.resize((size_t)embedded);
        f.read(t.embedded.data(), embedded);
        f.read(numPaths);
        if (!f.ok || numPaths > 64) return false;
        t.paths.resize(numPaths);
        for (std::wstring& p : t.paths) {
            uint32_t len = 0;
            f.read(len);
            if (!f.ok || len > 32768) return false;
            p.resize(len);
            f.read(p.data(), len * sizeof(wchar_t));
        }
    }
    if (!f.ok) return false;

    // Mark as recently used for LRU trimming.
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f.h, nullptr, nullptr, &now);

    m.fromCache = true;
    mesh = std::move(m);
    return true;
}

void cache_store(const wchar_t* path, const Mesh& mesh)
{
    uint64_t srcSize, srcTime;
    if (disabled() || mesh.fromCache || !mesh.error.empty() || !source_info(path, srcSize, srcTime) ||
        srcSize < kMinSourceBytes)
        return;
    std::wstring name = cache_file(path);
    if (name.empty()) return;
    std::wstring dir = cache_dir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    // The thread id keeps concurrent writers apart: a preview job and the main open of the same
    // file can both finish on different threads at the same time.
    std::wstring tmp = name + L".tmp" + std::to_wstring(GetCurrentThreadId());
    {
        File f;
        f.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f.h == INVALID_HANDLE_VALUE) return;

        Header hdr = {};
        memcpy(hdr.magic, "FBXVCACH", 8);
        hdr.version = kVersion;
        hdr.srcSize = srcSize;
        hdr.srcTime = srcTime;
        hdr.bmin = mesh.bmin;
        hdr.bmax = mesh.bmax;
        hdr.numVerts = mesh.vertices.size();
        hdr.numIndices = mesh.indices.size();
        hdr.numDraws = mesh.draws.size();
        hdr.numMaterials = mesh.materials.size();
        hdr.numTextures = mesh.textures.size();
        f.write(hdr);
        f.write(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
        f.write(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
        f.write(mesh.draws.data(), mesh.draws.size() * sizeof(Draw));
        for (const Material& mat : mesh.materials)
            f.write(DiskMaterial{ mat.color, mat.texture, mat.alphaTest ? 1u : 0u });
        for (const TextureSource& t : mesh.textures) {
            f.write((uint64_t)t.embedded.size());
            f.write(t.embedded.data(), t.embedded.size());
            f.write((uint32_t)t.paths.size());
            for (const std::wstring& p : t.paths) {
                f.write((uint32_t)p.size());
                f.write(p.data(), p.size() * sizeof(wchar_t));
            }
        }
        if (!f.ok) {
            CloseHandle(f.h);
            f.h = INVALID_HANDLE_VALUE;
            DeleteFileW(tmp.c_str());
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return;
    }
    trim_cache(dir);
}
