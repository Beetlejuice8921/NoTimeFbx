// Self-installer. The same executable, named NoTimeFbx-setup.exe (or run with --install), copies
// itself to %LOCALAPPDATA%\Programs\NoTimeFbx, adds a Start menu shortcut, the .fbx/.stl file
// associations and an entry in Settings > Apps; --uninstall (what that entry runs) removes all of
// it, including the mesh cache. Per-user only: no admin rights needed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

bool register_association(const wchar_t* exePath);   // assoc.cpp
bool unregister_association();

namespace {

const wchar_t kTitle[] = L"NoTime Fbx";
const wchar_t kVersion[] = L"1.0";
const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NoTimeFbx";

std::wstring known_folder(REFKNOWNFOLDERID id)
{
    wchar_t* p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring install_dir() { return known_folder(FOLDERID_UserProgramFiles) + L"\\NoTimeFbx"; }
std::wstring installed_exe() { return install_dir() + L"\\NoTimeFbx.exe"; }
std::wstring shortcut_path() { return known_folder(FOLDERID_Programs) + L"\\NoTimeFbx.lnk"; }

bool create_shortcut(const std::wstring& target, const std::wstring& lnk)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(L"NoTime Fbx");
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
    if (file) file->Release();
    link->Release();
    return ok;
}

bool set_string(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool set_dword(HKEY key, const wchar_t* name, DWORD value)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS;
}

bool write_uninstall_entry(const std::wstring& exe, DWORD sizeKb)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    bool ok = set_string(key, L"DisplayName", kTitle) && set_string(key, L"DisplayVersion", kVersion) &&
              set_string(key, L"DisplayIcon", exe + L",0") && set_string(key, L"InstallLocation", install_dir()) &&
              set_string(key, L"UninstallString", L"\"" + exe + L"\" --uninstall") &&
              set_dword(key, L"NoModify", 1) && set_dword(key, L"NoRepair", 1) &&
              set_dword(key, L"EstimatedSize", sizeKb);
    RegCloseKey(key);
    return ok;
}

// The mesh cache lives in %LOCALAPPDATA%\NoTimeFbx\cache and holds only files written by cache.cpp.
void delete_cache()
{
    std::wstring root = known_folder(FOLDERID_LocalAppData) + L"\\NoTimeFbx";
    std::wstring dir = root + L"\\cache\\";
    for (const wchar_t* pattern : { L"*.bin", L"*.tmp" }) {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((dir + pattern).c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do DeleteFileW((dir + fd.cFileName).c_str());
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
    RemoveDirectoryW(root.c_str());   // only succeeds if nothing else is left there
}

// A running executable can't delete itself: let a hidden cmd.exe do it once this process exits.
void delete_after_exit(const std::wstring& exe, const std::wstring& dir)
{
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + exe + L"\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace

// True when this executable was started as the installer (NoTimeFbx-setup*.exe).
bool is_setup_exe()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t* name = self;
    for (const wchar_t* p = self; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    return _wcsnicmp(name, L"NoTimeFbx-setup", 15) == 0;
}

int run_install()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring dir = install_dir(), exe = installed_exe();
    std::wstring prompt = L"Установить NoTime Fbx в\n" + dir +
                          L"?\n\nБудут добавлены ярлык в меню «Пуск» и открытие файлов .fbx и .stl. "
                          L"Права администратора не нужны.";
    if (MessageBoxW(nullptr, prompt.c_str(), kTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 1;

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (lstrcmpiW(self, exe.c_str()) != 0 && !CopyFileW(self, exe.c_str(), FALSE)) {
        MessageBoxW(nullptr, L"Не удалось скопировать программу. Если NoTime Fbx открыт, закройте его и повторите.",
                    kTitle, MB_ICONERROR);
        return 1;
    }

    WIN32_FILE_ATTRIBUTE_DATA a = {};
    GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a);
    bool ok = create_shortcut(exe, shortcut_path());
    ok &= write_uninstall_entry(exe, (a.nFileSizeLow + 1023) / 1024);
    // Registers the association and shows Windows' "open .fbx files with" dialog.
    ok &= register_association(exe.c_str());

    MessageBoxW(nullptr,
                ok ? L"NoTime Fbx установлен.\n\nОн есть в меню «Пуск»; удалить его можно в «Параметры → Приложения»."
                   : L"NoTime Fbx скопирован, но часть настроек записать не удалось.",
                kTitle, ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    CoUninitialize();
    return ok ? 0 : 1;
}

int run_uninstall()
{
    if (MessageBoxW(nullptr, L"Удалить NoTime Fbx и его кэш моделей?", kTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return 1;
    unregister_association();
    DeleteFileW(shortcut_path().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    delete_cache();
    delete_after_exit(installed_exe(), install_dir());
    MessageBoxW(nullptr, L"NoTime Fbx удалён.", kTitle, MB_ICONINFORMATION);
    return 0;
}
