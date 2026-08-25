// ssinject - loads spotispeed.dll into Spotify's main (browser) process.
//
// Spotify is Chromium-based and spawns several helper processes. WASAPI
// rendering lives in the *browser* process, so we target only that one.
//
// Timing matters. Our hooks only pick up an audio stream if we are already
// loaded when Spotify calls IAudioClient::Initialize, which happens the moment
// playback starts. Waiting for Spotify's window to appear is too late if
// playback auto-resumes, so we identify the browser process by parentage
// instead (its parent is not another Spotify.exe) and poll quickly.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <set>

#pragma comment(lib, "shlwapi.lib")

static bool g_verbose = false;
static void diag(const wchar_t* stage) {
    if (g_verbose) wprintf(L"      %s failed, GetLastError=%lu\n", stage, GetLastError());
}

// The browser process is the Spotify.exe whose parent is not itself a Spotify.exe.
// Helper processes (renderer, gpu, utility, crashpad) are all spawned by it.
static DWORD findMainSpotify() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    struct Ent { DWORD pid, parent; ULONGLONG order; };
    std::vector<Ent> spotify;
    std::set<DWORD>  pids;

    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        ULONGLONG i = 0;
        do {
            if (_wcsicmp(pe.szExeFile, L"Spotify.exe") == 0) {
                Ent e; e.pid = pe.th32ProcessID; e.parent = pe.th32ParentProcessID; e.order = i++;
                spotify.push_back(e);
                pids.insert(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    for (size_t i = 0; i < spotify.size(); ++i) {
        if (pids.find(spotify[i].parent) == pids.end()) return spotify[i].pid;
    }
    return spotify.empty() ? 0 : spotify[0].pid;
}

static bool alreadyLoaded(DWORD pid, const wchar_t* dllName) {
    // Retry: the module snapshot fails with ERROR_BAD_LENGTH while the target
    // is still building its module list, and a false "not loaded" would make us
    // inject a second copy.
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
    if (p == NULL) { diag(L"OpenProcess"); return false; }

    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(p, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == NULL) { diag(L"VirtualAllocEx"); CloseHandle(p); return false; }

    bool ok = false;
    if (!WriteProcessMemory(p, remote, dllPath, bytes, NULL)) {
        diag(L"WriteProcessMemory");
    } else {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(p, NULL, 0,
                        (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
        if (th == NULL) {
            diag(L"CreateRemoteThread");
        } else {
            DWORD w = WaitForSingleObject(th, 15000);
            DWORD code = 0; GetExitCodeThread(th, &code);
            ok = (w == WAIT_OBJECT_0 && code != 0);
            if (g_verbose && !ok) {
                // exit code 0 means LoadLibraryW returned NULL inside the target
                wprintf(L"      LoadLibraryW returned %lu (wait=%lu)\n", code, w);
            }
            CloseHandle(th);
        }
    }
    VirtualFreeEx(p, remote, 0, MEM_RELEASE);
    CloseHandle(p);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    bool watch = false;
    std::wstring dll;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--watch") == 0) watch = true;
        else if (_wcsicmp(argv[i], L"-v") == 0) g_verbose = true;
        else dll = argv[i];
    }
    if (dll.empty()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        PathRemoveFileSpecW(self);
        dll = std::wstring(self) + L"\\spotispeed.dll";
    }
    if (!PathFileExistsW(dll.c_str())) {
        wprintf(L"[!] DLL not found: %s\n", dll.c_str());
        return 1;
    }
    const wchar_t* name = PathFindFileNameW(dll.c_str());

    if (!watch) {
        g_verbose = true;
        DWORD pid = findMainSpotify();
        if (pid == 0) { wprintf(L"[!] Spotify is not running\n"); return 2; }
        if (alreadyLoaded(pid, name)) { wprintf(L"[=] already loaded in pid %u\n", pid); return 0; }
        bool ok = injectInto(pid, dll.c_str());
        wprintf(L"[%s] pid %u\n", ok ? L"+" : L"!", pid);
        return ok ? 0 : 3;
    }

    // Only one watcher at a time.
    HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\SpotiSpeedInjector");
    if (once != NULL && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Daemon. Poll fast so we beat Spotify to its first audio stream, and never
    // stop retrying: a failure right after the machine wakes should heal itself
    // without the user doing anything.
    DWORD lastPid = 0;
    int   failures = 0;
    for (;;) {
        DWORD pid = findMainSpotify();
        if (pid == 0) {
            lastPid = 0; failures = 0;
            Sleep(1000);
            continue;
        }
        if (pid != lastPid) { lastPid = pid; failures = 0; }   // new Spotify, fresh start

        if (!alreadyLoaded(pid, name)) {
            if (injectInto(pid, dll.c_str())) {
                failures = 0;
            } else if (failures < 100000) {
                ++failures;
            }
            // back off a little after repeated failures so we do not spin hot on
            // a process that will never accept us, but keep trying regardless
            Sleep(failures > 20 ? 3000 : 250);
        } else {
            failures = 0;
            Sleep(1500);
        }
    }
}
