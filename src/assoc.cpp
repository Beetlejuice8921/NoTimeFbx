// .fbx file association (per-user, no admin rights needed).
//
// Windows 10+ does not allow an app to silently make itself the default handler (UserChoice is
// hash-protected), so --register writes the ProgID/capabilities and then shows the system
// "How do you want to open .fbx files?" dialog where the user confirms the choice once.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

static const wchar_t kProgId[]   = L"NoTimeFbx.fbx";
static const wchar_t kAppKey[]   = L"Software\\NoTimeFbx";
static const wchar_t kCapsPath[] = L"Software\\NoTimeFbx\\Capabilities";

static bool set_value(const wchar_t* subkey, const wchar_t* name, const wchar_t* data, DWORD type = REG_SZ)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    DWORD bytes = type == REG_NONE ? 0 : (DWORD)((lstrlenW(data) + 1) * sizeof(wchar_t));
    LSTATUS st = RegSetValueExW(key, name, 0, type, type == REG_NONE ? nullptr : (const BYTE*)data, bytes);
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

// Registers `exePath` (the running executable when null) as a handler for .fbx.
bool register_association(const wchar_t* exePath)
{
    wchar_t exe[MAX_PATH];
    if (exePath) lstrcpynW(exe, exePath, MAX_PATH);
    else GetModuleFileNameW(nullptr, exe, MAX_PATH);

    wchar_t cmd[MAX_PATH + 16], icon[MAX_PATH + 8];
    wsprintfW(cmd, L"\"%s\" \"%%1\"", exe);
    wsprintfW(icon, L"\"%s\",0", exe);

    bool ok = true;
    // ProgID
    ok &= set_value(L"Software\\Classes\\NoTimeFbx.fbx", nullptr, L"FBX model");
    ok &= set_value(L"Software\\Classes\\NoTimeFbx.fbx\\DefaultIcon", nullptr, icon);
    ok &= set_value(L"Software\\Classes\\NoTimeFbx.fbx\\shell\\open\\command", nullptr, cmd);
    // Offer it for .fbx in "Open with"
    ok &= set_value(L"Software\\Classes\\.fbx\\OpenWithProgids", kProgId, nullptr, REG_NONE);
    ok &= set_value(L"Software\\Classes\\Applications\\NoTimeFbx.exe", L"FriendlyAppName", L"NoTime Fbx");
    ok &= set_value(L"Software\\Classes\\Applications\\NoTimeFbx.exe\\SupportedTypes", L".fbx", L"");
    ok &= set_value(L"Software\\Classes\\Applications\\NoTimeFbx.exe\\shell\\open\\command", nullptr, cmd);
    // Show up in Settings > Default apps
    ok &= set_value(kCapsPath, L"ApplicationName", L"NoTime Fbx");
    ok &= set_value(kCapsPath, L"ApplicationDescription", L"Fast FBX model viewer");
    ok &= set_value(L"Software\\NoTimeFbx\\Capabilities\\FileAssociations", L".fbx", kProgId);
    ok &= set_value(L"Software\\RegisteredApplications", L"NoTimeFbx", kCapsPath);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!ok) return false;

    // Let the user pick NoTimeFbx as the default ("Always use this app").
    OPENASINFO info = {};
    info.pcszFile = L"model.fbx";
    info.oaifInFlags = OAIF_REGISTER_EXT | OAIF_FORCE_REGISTRATION;
    if (FAILED(SHOpenWithDialog(nullptr, &info)))
        ShellExecuteW(nullptr, nullptr, L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

bool unregister_association()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\NoTimeFbx.fbx");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\NoTimeFbx.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.fbx\\OpenWithProgids", kProgId);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"NoTimeFbx");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}
