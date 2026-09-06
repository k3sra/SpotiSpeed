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
#include <map>

#pragma comment(lib, "shlwapi.lib")

static bool g_verbose = false;

static void say(const wchar_t* fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

// Persistent log. The watcher runs with no window, so without this there is no
// way to find out why an injection did not happen.
static void wlog(const char* fmt, ...) {
    char path[MAX_PATH];
    if (!ExpandEnvironmentStringsA("%LOCALAPPDATA%\\SpotiSpeed\\watcher.log", path, MAX_PATH)) return;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow > 256 * 1024) {
        DeleteFileA(path);   // keep it small; this is a rolling breadcrumb trail
    }
    FILE* f = NULL;
    if (fopen_s(&f, path, "a") != 0 || f == NULL) return;

    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
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
// There can be more than one independent Spotify tree at once - most often a
// leftover elevated instance sitting next to the normal one. Parentage cannot
// tell them apart, and committing to the wrong root means retrying a process we
// are not allowed to open (ACCESS_DENIED) forever while the Spotify the user is
// actually listening to never gets hooked. So return every plausible browser
// process, the one owning a visible window first, and try them all.

static BOOL CALLBACK collectWindowPids(HWND hwnd, LPARAM lp) {
    std::set<DWORD>* out = (std::set<DWORD>*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == NULL) return TRUE;
    wchar_t path[MAX_PATH] = { 0 };
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n) &&
        _wcsicmp(PathFindFileNameW(path), L"Spotify.exe") == 0) {
        out->insert(pid);
    }
    CloseHandle(p);
    return TRUE;
}

static std::vector<DWORD> findSpotifyTargets() {
    std::vector<DWORD> targets;
    std::set<DWORD> pids, taken;
    std::vector<std::pair<DWORD, DWORD> > all;   // pid, parent

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return targets;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Spotify.exe") == 0) {
                all.push_back(std::make_pair(pe.th32ProcessID, pe.th32ParentProcessID));
                pids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (all.empty()) return targets;

    // the instance the user can actually see comes first
    std::set<DWORD> windowed;
    EnumWindows(collectWindowPids, (LPARAM)&windowed);
    for (std::set<DWORD>::iterator w = windowed.begin(); w != windowed.end(); ++w)
        if (pids.count(*w) && !taken.count(*w)) { targets.push_back(*w); taken.insert(*w); }

    // then every tree root: a Spotify.exe not spawned by another Spotify.exe
    for (size_t i = 0; i < all.size(); ++i)
        if (!pids.count(all[i].second) && !taken.count(all[i].first)) {
            targets.push_back(all[i].first);
            taken.insert(all[i].first);
        }
    return targets;
}

// kept for the housekeeping checks that only need "is Spotify running at all"
static DWORD findMainSpotify() {
    std::vector<DWORD> t = findSpotifyTargets();
    return t.empty() ? 0 : t[0];
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
    if (p == NULL) {
        DWORD e = GetLastError();
        say(L"      OpenProcess failed (%lu)\n", e);
        wlog("inject pid=%lu: OpenProcess failed err=%lu", pid, e);
        return false;
    }

    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(p, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == NULL) {
        DWORD e = GetLastError();
        say(L"      VirtualAllocEx failed (%lu)\n", e);
        wlog("inject pid=%lu: VirtualAllocEx failed err=%lu", pid, e);
        CloseHandle(p);
        return false;
    }

    bool ok = false;
    if (!WriteProcessMemory(p, remote, dllPath, bytes, NULL)) {
        DWORD e = GetLastError();
        say(L"      WriteProcessMemory failed (%lu)\n", e);
        wlog("inject pid=%lu: WriteProcessMemory failed err=%lu", pid, e);
    } else {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
        if (th == NULL) {
            DWORD e = GetLastError();
            say(L"      CreateRemoteThread failed (%lu)\n", e);
            wlog("inject pid=%lu: CreateRemoteThread failed err=%lu%s", pid, e,
                 e == ERROR_ACCESS_DENIED ? " (blocked by security software)" : "");
        } else {
            DWORD w = WaitForSingleObject(th, 15000);
            DWORD code = 0; GetExitCodeThread(th, &code);
            ok = (w == WAIT_OBJECT_0 && code != 0);
            if (!ok) {
                say(L"      LoadLibraryW returned %lu (wait=%lu)\n", code, w);
                wlog("inject pid=%lu: LoadLibraryW returned %lu wait=%lu (0 = target refused the load)",
                     pid, code, w);
            } else {
                wlog("inject pid=%lu: OK", pid);
            }
            CloseHandle(th);
        }
    }
    VirtualFreeEx(p, remote, 0, MEM_RELEASE);
    CloseHandle(p);
    return ok;
}

// Same attempt, but says nothing. Used once a process has proved it will not
// let us in, so the log stays readable.
static bool injectQuiet(DWORD pid, const wchar_t* dllPath) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (p == NULL) return false;
    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(p, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == NULL) { CloseHandle(p); return false; }
    bool ok = false;
    if (WriteProcessMemory(p, remote, dllPath, bytes, NULL)) {
        FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
        if (th != NULL) {
            DWORD w = WaitForSingleObject(th, 15000);
            DWORD code = 0; GetExitCodeThread(th, &code);
            ok = (w == WAIT_OBJECT_0 && code != 0);
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
// A read-only file was not enough: Spotify's updater simply cleared the
// attribute and deleted it, then updated and wiped the knob. So we create the
// file and HOLD IT OPEN with no sharing for as long as the watcher lives.
// Windows will not let anyone delete, rename or open a file that is held with
// dwShareMode = 0, so the updater has nowhere to unpack and gives up.
static HANDLE g_updGuard = INVALID_HANDLE_VALUE;

static void blockUpdates() {
    std::wstring upd = envPath(L"LOCALAPPDATA", L"\\Spotify\\Update");
    if (upd.empty()) return;

    if (g_updGuard != INVALID_HANDLE_VALUE) {
        if (GetFileAttributesW(upd.c_str()) != INVALID_FILE_ATTRIBUTES) return;  // still holding
        CloseHandle(g_updGuard);
        g_updGuard = INVALID_HANDLE_VALUE;
        wlog("update blocker was lost, re-arming");
    }

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
            SetFileAttributesW(upd.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(upd.c_str());
        }
    }

    g_updGuard = CreateFileW(upd.c_str(), GENERIC_WRITE, 0 /* deny all sharing */, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (g_updGuard != INVALID_HANDLE_VALUE) wlog("update blocker armed (exclusive handle held)");
    else wlog("update blocker FAILED err=%lu", GetLastError());
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
// Does Spotify's UI bundle still carry the knob?
//
// After Spicetify patches, Apps/ holds extracted xpui/ and login/ folders and
// no .spa archives. When Spotify updates it drops fresh xpui.spa back in, so
// the mere presence of that archive means a new, unpatched bundle has landed.
// The old check only looked at index.html and bailed out when the folder was
// missing, which is exactly the post-update state - so it never repaired.
static bool needsKnobRepair() {
    std::wstring spa = envPath(L"APPDATA", L"\\Spotify\\Apps\\xpui.spa");
    if (!spa.empty() && PathFileExistsW(spa.c_str())) return true;

    std::wstring index = envPath(L"APPDATA", L"\\Spotify\\Apps\\xpui\\index.html");
    if (index.empty() || !PathFileExistsW(index.c_str())) return false;

    HANDLE h = CreateFileW(index.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192]; DWORD got = 0;
    bool patched = false;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        buf[got] = 0;
        patched = (strstr(buf, "spicetifyWrapper") != NULL);
    }
    CloseHandle(h);
    return !patched;
}

static bool runHidden(const std::wstring& cmd, DWORD waitMs) {
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);
    if (!CreateProcessW(NULL, mutableCmd.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, waitMs);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

// Re-apply Spicetify after Spotify replaces its bundle. Only ever called while
// Spotify is closed, so we never pull the UI out from under a playing track.
static void ensureKnob() {
    if (!needsKnobRepair()) return;
    wlog("Spotify replaced its UI bundle - re-applying the knob");

    std::wstring spice = envPath(L"LOCALAPPDATA", L"\\spicetify\\spicetify.exe");
    if (spice.empty() || !PathFileExistsW(spice.c_str())) {
        wlog("cannot repair: spicetify.exe not found");
        return;
    }

    // "backup apply" refuses if a stale backup of the previous version is still
    // sitting there ("Failed to clear current backup"), so clear it ourselves.
    std::wstring backup = envPath(L"APPDATA", L"\\spicetify\\Backup");
    if (!backup.empty() && PathFileExistsW(backup.c_str())) {
        SHFILEOPSTRUCTW op; ZeroMemory(&op, sizeof(op));
        std::wstring from = backup; from.push_back(L'\0'); from.push_back(L'\0');
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        SHFileOperationW(&op);
    }

    runHidden(L"\"" + spice + L"\" backup apply", 180000);
    wlog(needsKnobRepair() ? "re-apply did not take" : "knob restored");
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
    if (once_ != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        wlog("another watcher is already running; exiting");
        return 0;
    }
    wlog("watcher started");

    // pid -> attempt count, or -1 once that process is loaded / done with
    std::map<DWORD, int> seen;
    int tick = 0;
    for (;;) {
        // housekeeping roughly every 30 seconds
        if (tick % 120 == 0) {
            blockUpdates();
            ensureAutostart(exePath.c_str(), dll.c_str());
            if (findMainSpotify() == 0) ensureKnob();
        }
        ++tick;

        std::vector<DWORD> targets = findSpotifyTargets();
        if (targets.empty()) {
            seen.clear();
            Sleep(250);
            continue;
        }

        bool anyPending = false;
        for (size_t i = 0; i < targets.size(); ++i) {
            const DWORD pid = targets[i];
            if (seen.find(pid) == seen.end()) {
                seen[pid] = 0;
                wlog("spotify process pid=%lu", pid);
            }
            if (alreadyLoaded(pid, name)) { seen[pid] = -1; continue; }   // -1 = done

            if (seen[pid] < 0) continue;
            // An instance we are not allowed to open (an elevated one, say) must
            // never stall the instance we can. Keep retrying it in case it
            // restarts unelevated, but slow down and stop filling the log.
            const int tries = seen[pid];
            if (tries > 5 && (tick % 120) != 0) { anyPending = true; continue; }

            const bool quiet = (tries >= 3);
            const bool done  = quiet ? injectQuiet(pid, dll.c_str())
                                     : injectInto(pid, dll.c_str());
            if (done) {
                seen[pid] = -1;
                wlog("inject pid=%lu: OK", pid);
            } else {
                if (tries == 2) wlog("inject pid=%lu: still refusing, will keep retrying quietly", pid);
                ++seen[pid];
                anyPending = true;
            }
        }
        Sleep(anyPending ? 1000 : 250);
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
