// ssinject - loads spotispeed.dll into Spotify's main (browser) process.
//
// Spotify is Chromium-based and spawns several helper processes. WASAPI
// rendering lives in the *browser* process - the one that owns the UI window -
// so we target only that one. Injecting into the crashpad handler or the
// sandboxed helpers is useless and destabilises the app.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "shlwapi.lib")

struct FindCtx { DWORD pid; };

static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    FindCtx* ctx = (FindCtx*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == NULL) return TRUE;
    wchar_t path[MAX_PATH] = { 0 };
    DWORD n = MAX_PATH;
    bool isSpotify = false;
    if (QueryFullProcessImageNameW(p, 0, path, &n))
        isSpotify = (_wcsicmp(PathFindFileNameW(path), L"Spotify.exe") == 0);
    CloseHandle(p);

    if (isSpotify) { ctx->pid = pid; return FALSE; }
    return TRUE;
}

// The browser process owns Spotify's visible window.
static DWORD findMainSpotify() {
    FindCtx ctx; ctx.pid = 0;
    EnumWindows(enumProc, (LPARAM)&ctx);
    return ctx.pid;
}

static bool alreadyLoaded(DWORD pid, const wchar_t* dllName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
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

static bool injectInto(DWORD pid, const wchar_t* dllPath) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (p == NULL) return false;

    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(p, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == NULL) { CloseHandle(p); return false; }

    bool ok = false;
    if (WriteProcessMemory(p, remote, dllPath, bytes, NULL)) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(p, NULL, 0,
                        (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
        if (th != NULL) {
            WaitForSingleObject(th, 10000);
            DWORD code = 0; GetExitCodeThread(th, &code);
            ok = (code != 0);
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
        DWORD pid = findMainSpotify();
        if (pid == 0) { wprintf(L"[!] Spotify window not found - is it running?\n"); return 2; }
        if (alreadyLoaded(pid, name)) { wprintf(L"[=] already loaded in pid %u\n", pid); return 0; }
        bool ok = injectInto(pid, dll.c_str());
        wprintf(L"[%s] pid %u\n", ok ? L"+" : L"!", pid);
        return ok ? 0 : 3;
    }

    // daemon: keep the main process hooked across Spotify restarts
    for (;;) {
        DWORD pid = findMainSpotify();
        if (pid != 0 && !alreadyLoaded(pid, name)) injectInto(pid, dll.c_str());
        Sleep(1500);
    }
}
