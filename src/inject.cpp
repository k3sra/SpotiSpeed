// ssinject - the SpotiSpeed watcher.
//
// Runs quietly from HKCU\...\Run and keeps three things true:
//   1. spotispeed.dll is loaded into Spotify whenever Spotify is running
//   2. Spotify cannot update itself out from under the patch
//   3. its own autostart entry still exists
//
// This used to be a Startup-folder .vbs launching PowerShell launching this
// exe. That chain had four links and the Startup folder silently never fired,
// which left the knob dead after a reboot. It is now one native process in the
// same Run key Spotify itself uses.
//
// Built as a Windows-subsystem app so nothing flashes on screen. Pass --once to
// inject a single time and print what happened to the parent console.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <set>

#pragma comment(lib, "shlwapi.lib")

static bool g_verbose = false;

static void say(const wchar_t* fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static std::wstring envPath(const wchar_t* var, const wchar_t* tail) {
    wchar_t buf[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(var, buf, MAX_PATH) == 0) return L"";
    return std::wstring(buf) + tail;
}

// ------------------------------------------------------------ find Spotify --
// The browser process is the Spotify.exe whose parent is not itself a
// Spotify.exe. Helpers (renderer, gpu, utility, crashpad) are all its children.
// Deliberately does not look for a window: waiting for the UI to appear loses
// the race against Spotify opening its audio stream.
static DWORD findMainSpotify() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    struct Ent { DWORD pid, parent; };
    std::vector<Ent> spotify;
    std::set<DWORD>  pids;

    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Spotify.exe") == 0) {
                Ent e; e.pid = pe.th32ProcessID; e.parent = pe.th32ParentProcessID;
                spotify.push_back(e);
                pids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    for (size_t i = 0; i < spotify.size(); ++i)
        if (pids.find(spotify[i].parent) == pids.end()) return spotify[i].pid;
    return spotify.empty() ? 0 : spotify[0].pid;
}

static bool alreadyLoaded(DWORD pid, const wchar_t* dllName) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_BAD_LENGTH) { Sleep(50); continue; }
            return false;
        }
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        bool found = false;
        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, dllName) == 0) { found = true; break; }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }
    return false;
}

static bool injectInto(DWORD pid, const wchar_t* dllPath) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (p == NULL) { say(L"      OpenProcess failed (%lu)\n", GetLastError()); return false; }

    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(p, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == NULL) { say(L"      VirtualAllocEx failed (%lu)\n", GetLastError()); CloseHandle(p); return false; }

    bool ok = false;
    if (!WriteProcessMemory(p, remote, dllPath, bytes, NULL)) {
        say(L"      WriteProcessMemory failed (%lu)\n", GetLastError());
    } else {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
        if (th == NULL) {
            say(L"      CreateRemoteThread failed (%lu)\n", GetLastError());
        } else {
            DWORD w = WaitForSingleObject(th, 15000);
            DWORD code = 0; GetExitCodeThread(th, &code);
            ok = (w == WAIT_OBJECT_0 && code != 0);
            if (!ok) say(L"      LoadLibraryW returned %lu (wait=%lu)\n", code, w);
            CloseHandle(th);
        }
    }
    VirtualFreeEx(p, remote, 0, MEM_RELEASE);
    CloseHandle(p);
    return ok;
}

// ----------------------------------------------------------- housekeeping --
// Spotify stages updates into %LOCALAPPDATA%\Spotify\Update. A read-only file
// on that path means the directory can never be created, so the updater has
// nowhere to unpack and gives up. An update would replace the UI bundle and
// take the knob with it.
static void blockUpdates() {
    std::wstring upd = envPath(L"LOCALAPPDATA", L"\\Spotify\\Update");
    if (upd.empty()) return;

    DWORD attr = GetFileAttributesW(upd.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            SHFILEOPSTRUCTW op; ZeroMemory(&op, sizeof(op));
            std::wstring from = upd; from.push_back(L'\0'); from.push_back(L'\0');
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            SHFileOperationW(&op);
        } else {
            return;   // already a file; nothing to do
        }
    }
    HANDLE h = CreateFileW(upd.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        SetFileAttributesW(upd.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN);
    }
}

// Put ourselves back in Run if anything clears it. Same mechanism Spotify uses
// for its own autostart, so if Spotify can come back after a reboot, so can we.
static void ensureAutostart(const wchar_t* exePath, const wchar_t* dllPath) {
    std::wstring want = L"\"";
    want += exePath; want += L"\" --watch \""; want += dllPath; want += L"\"";

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;

    wchar_t cur[1024] = { 0 };
    DWORD sz = sizeof(cur), type = 0;
    bool same = (RegQueryValueExW(k, L"SpotiSpeed", NULL, &type, (LPBYTE)cur, &sz) == ERROR_SUCCESS)
                && type == REG_SZ && want == cur;
    if (!same) {
        RegSetValueExW(k, L"SpotiSpeed", 0, REG_SZ, (const BYTE*)want.c_str(),
                       (DWORD)((want.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
}

// If Spotify ever replaces its UI bundle, the knob is gone until Spicetify is
// re-applied. Only done while Spotify is closed so we never yank it out from
// under the user mid-song.
static void ensureKnob() {
    std::wstring index = envPath(L"APPDATA", L"\\Spotify\\Apps\\xpui\\index.html");
    if (index.empty()) return;
    HANDLE h = CreateFileW(index.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;          // still a packed .spa

    char buf[8192]; DWORD got = 0;
    bool patched = false;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        buf[got] = 0;
        patched = (strstr(buf, "spicetifyWrapper") != NULL);
    }
    CloseHandle(h);
    if (patched) return;

    std::wstring spice = envPath(L"LOCALAPPDATA", L"\\spicetify\\spicetify.exe");
    if (spice.empty() || !PathFileExistsW(spice.c_str())) return;

    std::wstring cmd = L"\"" + spice + L"\" backup apply";
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);
    if (CreateProcessW(NULL, mutableCmd.data(), NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

// ------------------------------------------------------------------- main --
static int run(int argc, wchar_t** argv) {
    bool watch = false, once = false;
    std::wstring dll;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--watch") == 0)      watch = true;
        else if (_wcsicmp(argv[i], L"--once") == 0)  once = true;
        else if (argv[i][0] != L'-')                 dll = argv[i];
    }

    wchar_t self[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring exePath = self;
    if (dll.empty()) {
        wchar_t dir[MAX_PATH];
        wcscpy_s(dir, MAX_PATH, self);
        PathRemoveFileSpecW(dir);
        dll = std::wstring(dir) + L"\\spotispeed.dll";
    }
    if (!PathFileExistsW(dll.c_str())) {
        say(L"[!] DLL not found: %s\n", dll.c_str());
        return 1;
    }
    const wchar_t* name = PathFindFileNameW(dll.c_str());

    if (once || !watch) {
        g_verbose = true;
        DWORD pid = findMainSpotify();
        if (pid == 0) { say(L"[!] Spotify is not running\n"); return 2; }
        if (alreadyLoaded(pid, name)) { say(L"[=] already loaded in pid %u\n", pid); return 0; }
        bool ok = injectInto(pid, dll.c_str());
        say(L"[%s] pid %u\n", ok ? L"+" : L"!", pid);
        return ok ? 0 : 3;
    }

    HANDLE once_ = CreateMutexW(NULL, TRUE, L"Local\\SpotiSpeedWatcher");
    if (once_ != NULL && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    DWORD lastPid = 0;
    int   failures = 0, tick = 0;
    for (;;) {
        // housekeeping roughly every 30 seconds
        if (tick % 120 == 0) {
            blockUpdates();
            ensureAutostart(exePath.c_str(), dll.c_str());
            if (findMainSpotify() == 0) ensureKnob();
        }
        ++tick;

        DWORD pid = findMainSpotify();
        if (pid == 0) {
            lastPid = 0; failures = 0;
            Sleep(250);
            continue;
        }
        if (pid != lastPid) { lastPid = pid; failures = 0; }

        if (!alreadyLoaded(pid, name)) {
            if (injectInto(pid, dll.c_str())) failures = 0;
            else ++failures;
            // Never stop trying. A refusal right after the machine wakes should
            // heal on its own, without the user doing anything.
            Sleep(failures > 20 ? 3000 : 250);
        } else {
            failures = 0;
            Sleep(250);
        }
    }
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool wantsConsole = false;
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--watch") != 0) wantsConsole = true;

    if (wantsConsole && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = NULL;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    int rc = run(argc, argv);
    if (argv) LocalFree(argv);
    return rc;
}
