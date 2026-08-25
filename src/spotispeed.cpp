// SpotiSpeed - in-process WASAPI rate control for the Spotify desktop client.
//
// Spotify decodes music natively and its player refuses speed changes for
// tracks ("not_supported_by_content_type"), so the only way to retime music is
// to sit between Spotify and the audio device. This DLL hooks the shared-mode
// WASAPI render path inside Spotify.exe: Spotify writes decoded audio into a
// staging buffer we own, we resample it, and we hand the device the retimed
// result. Reporting a virtual padding back to Spotify applies backpressure, so
// Spotify's decoder naturally speeds up or slows down to match.
//
// Pitch follows speed (plain rate change), which is what "slowed"/"sped up"
// is supposed to sound like.
//
// Safety: at exactly 1.0x we hand Spotify the real device buffer and get out of
// the way entirely, so normal listening is bit-identical to an unpatched client.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ksmedia.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include "resampler.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------- settings --
static const double kMinSpeed = 0.2;
static const double kMaxSpeed = 2.0;
static const double kHeadroom = 2.0;   // advertised buffer = device buffer * this
static const unsigned short kPort = 4381;

static std::atomic<double> g_speed(1.0);
// Ground truth for what the audio path is actually doing: source frames taken
// from Spotify vs frames handed to the device. Their ratio IS the play rate.
static std::atomic<long long> g_srcFrames(0);
static std::atomic<long long> g_outFrames(0);
static std::atomic<bool>   g_enabled(true);

static bool isUnity(double s) { return s > 0.9995 && s < 1.0005; }

// ------------------------------------------------------------------ logging --
static volatile LONG g_logLines = 0;
static void logf(const char* fmt, ...) {
#ifdef SS_DEBUG
    if (InterlockedIncrement(&g_logLines) > 400) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    char path[MAX_PATH];
    ExpandEnvironmentStringsA("%LOCALAPPDATA%\\SpotiSpeed\\spotispeed.log", path, MAX_PATH);
    FILE* f = NULL;
    if (fopen_s(&f, path, "a") == 0 && f) { fputs(buf, f); fclose(f); }
#else
    (void)fmt;
#endif
}

// ------------------------------------------------------------ stream state --
struct Stream {
    IAudioClient*       client    = NULL;
    IAudioRenderClient* render    = NULL;
    UINT32 devFrames  = 0;   // real device buffer, frames
    UINT32 virtFrames = 0;   // what we advertise to Spotify
    int    channels   = 0;
    int    bits       = 0;
    bool   isFloat    = false;
    int    frameBytes = 0;
    std::vector<BYTE>  give;      // buffer handed to Spotify when retiming
    std::vector<float> scratch;   // resampler output
    std::vector<float> ingest;    // format-conversion scratch
    Resampler rs;
    bool   active     = false;
    int    faults     = 0;       // consecutive faults in the pump
    bool   faulted    = false;   // this stream gave up; others keep working
};

static std::recursive_mutex g_mtx;
static std::unordered_map<IAudioClient*, Stream*> g_byClient;
static std::unordered_map<IAudioRenderClient*, Stream*> g_byRender;
static IAudioClient* g_bootstrapClient = NULL;   // our own probe: never hook

// The Windows audio engine calls the client's *virtual* methods from inside
// GetBuffer/ReleaseBuffer. Those land back in our hooks, so while we are
// driving the device ourselves every hook must step aside and use the
// originals - otherwise we hand the engine our virtual padding (and re-enter
// our own lock).
static thread_local int g_reentry = 0;
struct Reentry {
    Reentry()  { ++g_reentry; }
    ~Reentry() { --g_reentry; }
};

// --------------------------------------------------------- original vtable --
typedef HRESULT (STDMETHODCALLTYPE *Fn_Initialize)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
typedef HRESULT (STDMETHODCALLTYPE *Fn_GetBufferSize)(IAudioClient*, UINT32*);
typedef HRESULT (STDMETHODCALLTYPE *Fn_GetCurrentPadding)(IAudioClient*, UINT32*);
typedef HRESULT (STDMETHODCALLTYPE *Fn_GetService)(IAudioClient*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE *Fn_Stop)(IAudioClient*);
typedef HRESULT (STDMETHODCALLTYPE *Fn_Reset)(IAudioClient*);
typedef HRESULT (STDMETHODCALLTYPE *Fn_GetBuffer)(IAudioRenderClient*, UINT32, BYTE**);
typedef HRESULT (STDMETHODCALLTYPE *Fn_ReleaseBuffer)(IAudioRenderClient*, UINT32, DWORD);

static Fn_Initialize        o_Initialize        = NULL;
static Fn_GetBufferSize     o_GetBufferSize     = NULL;
static Fn_GetCurrentPadding o_GetCurrentPadding = NULL;
static Fn_GetService        o_GetService        = NULL;
static Fn_Stop              o_Stop              = NULL;
static Fn_Reset             o_Reset             = NULL;
static Fn_GetBuffer         o_GetBuffer         = NULL;
static Fn_ReleaseBuffer     o_ReleaseBuffer     = NULL;

// ------------------------------------------------------------- conversions --
static void toFloat(const BYTE* in, float* out, size_t n, int bits, bool isFloat) {
    if (isFloat) { memcpy(out, in, n * sizeof(float)); return; }
    if (bits == 16) {
        const short* p = (const short*)in;
        for (size_t i = 0; i < n; ++i) out[i] = p[i] / 32768.0f;
    } else if (bits == 32) {
        const int* p = (const int*)in;
        for (size_t i = 0; i < n; ++i) out[i] = (float)(p[i] / 2147483648.0);
    } else if (bits == 24) {
        for (size_t i = 0; i < n; ++i) {
            int v = (in[i*3] | (in[i*3+1] << 8) | ((signed char)in[i*3+2] << 16));
            out[i] = (float)(v / 8388608.0);
        }
    } else {
        memset(out, 0, n * sizeof(float));
    }
}

static void fromFloat(const float* in, BYTE* out, size_t n, int bits, bool isFloat) {
    if (isFloat) { memcpy(out, in, n * sizeof(float)); return; }
    if (bits == 16) {
        short* p = (short*)out;
        for (size_t i = 0; i < n; ++i) {
            float v = in[i]; if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
            p[i] = (short)(v * 32767.0f);
        }
    } else if (bits == 32) {
        int* p = (int*)out;
        for (size_t i = 0; i < n; ++i) {
            double v = in[i]; if (v > 1.0) v = 1.0; if (v < -1.0) v = -1.0;
            p[i] = (int)(v * 2147483647.0);
        }
    } else if (bits == 24) {
        for (size_t i = 0; i < n; ++i) {
            double v = in[i]; if (v > 1.0) v = 1.0; if (v < -1.0) v = -1.0;
            int s = (int)(v * 8388607.0);
            out[i*3]   = (BYTE)(s & 0xff);
            out[i*3+1] = (BYTE)((s >> 8) & 0xff);
            out[i*3+2] = (BYTE)((s >> 16) & 0xff);
        }
    }
}

static double clampSpeed(double s) {
    if (!(s > 0.0)) return 1.0;
    if (s < kMinSpeed) return kMinSpeed;
    if (s > kMaxSpeed) return kMaxSpeed;
    return s;
}

// Effective rate. Once we are staging a stream we must keep staging it: the
// advertised buffer size and the virtual padding are a single contract, and
// Spotify caches the buffer size at initialise time. Disabling therefore just
// pins the rate to 1.0 (bit-exact) rather than changing the contract.
static double speedFor(Stream* st) {
    if (!g_enabled.load()) return 1.0;
    if (st != NULL && st->faulted) return 1.0;   // only this stream stands down
    return clampSpeed(g_speed.load());
}

// ------------------------------------------------------------------- hooks --
static HRESULT STDMETHODCALLTYPE h_Initialize(IAudioClient* self, AUDCLNT_SHAREMODE mode, DWORD flags,
                                              REFERENCE_TIME dur, REFERENCE_TIME per,
                                              const WAVEFORMATEX* fmt, LPCGUID sess) {
    HRESULT hr = o_Initialize(self, mode, flags, dur, per, fmt, sess);
    if (FAILED(hr) || self == g_bootstrapClient || fmt == NULL) return hr;
    if (mode != AUDCLNT_SHAREMODE_SHARED) return hr;   // only shared mode is safe to retime

    Stream* st = new Stream();
    st->client     = self;
    st->channels   = fmt->nChannels;
    st->bits       = fmt->wBitsPerSample;
    st->frameBytes = fmt->nBlockAlign;
    st->isFloat    = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* we = (const WAVEFORMATEXTENSIBLE*)fmt;
        st->isFloat = (IsEqualGUID(we->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0);
    }
    UINT32 bufFrames = 0;
    if (FAILED(o_GetBufferSize(self, &bufFrames)) || bufFrames == 0 || st->channels <= 0) {
        delete st; return hr;
    }
    st->devFrames  = bufFrames;
    st->virtFrames = (UINT32)(bufFrames * kHeadroom);
    st->rs.configure(st->channels);
    st->give.resize((size_t)st->virtFrames * st->frameBytes + 256);
    st->scratch.resize((size_t)bufFrames * st->channels + 256);
    st->active = true;

    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    std::unordered_map<IAudioClient*, Stream*>::iterator it = g_byClient.find(self);
    if (it != g_byClient.end()) {
        // Drop every render-client entry that pointed at the old stream before
        // freeing it. COM recycles addresses, so a leftover mapping here means a
        // later GetBuffer/ReleaseBuffer would run against freed memory.
        Stream* old = it->second;
        for (std::unordered_map<IAudioRenderClient*, Stream*>::iterator r = g_byRender.begin();
             r != g_byRender.end(); ) {
            if (r->second == old) r = g_byRender.erase(r); else ++r;
        }
        delete old;
    }
    g_byClient[self] = st;
    logf("[SS] init client=%p ch=%d bits=%d flt=%d devBuf=%u evt=%d\n",
         (void*)self, st->channels, st->bits, (int)st->isFloat, bufFrames,
         (int)((flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0));
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_GetService(IAudioClient* self, REFIID riid, void** ppv) {
    HRESULT hr = o_GetService(self, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && IsEqualIID(riid, __uuidof(IAudioRenderClient))) {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        std::unordered_map<IAudioClient*, Stream*>::iterator it = g_byClient.find(self);
        if (it != g_byClient.end()) {
            it->second->render = (IAudioRenderClient*)*ppv;
            g_byRender[(IAudioRenderClient*)*ppv] = it->second;
            logf("[SS] render=%p bound to client=%p\n", *ppv, (void*)self);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_GetBufferSize(IAudioClient* self, UINT32* pn) {
    if (g_reentry) return o_GetBufferSize(self, pn);
    {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        std::unordered_map<IAudioClient*, Stream*>::iterator it = g_byClient.find(self);
        if (it != g_byClient.end() && it->second->active) {
            *pn = it->second->virtFrames;
            return S_OK;
        }
    }
    return o_GetBufferSize(self, pn);
}

// Virtual padding, expressed in source frames, so Spotify paces its decoder to us.
static HRESULT STDMETHODCALLTYPE h_GetCurrentPadding(IAudioClient* self, UINT32* pn) {
    if (g_reentry) return o_GetCurrentPadding(self, pn);
    UINT32 realPad = 0;
    HRESULT hr = o_GetCurrentPadding(self, &realPad);
    if (FAILED(hr)) return hr;

    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    std::unordered_map<IAudioClient*, Stream*>::iterator it = g_byClient.find(self);
    if (it == g_byClient.end() || !it->second->active) { *pn = realPad; return hr; }
    Stream* st = it->second;
    const double sp = speedFor(st);

    double queued = st->rs.pending() + (double)realPad * sp;
    if (queued < 0) queued = 0;
    UINT32 pad = (UINT32)(queued + 0.5);
    if (pad > st->virtFrames) pad = st->virtFrames;
    *pn = pad;
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_GetBuffer(IAudioRenderClient* self, UINT32 frames, BYTE** ppData) {
    if (g_reentry) return o_GetBuffer(self, frames, ppData);
    std::unique_lock<std::recursive_mutex> lk(g_mtx);
    std::unordered_map<IAudioRenderClient*, Stream*>::iterator it = g_byRender.find(self);
    if (it == g_byRender.end() || !it->second->active) {
        lk.unlock();
        return o_GetBuffer(self, frames, ppData);
    }
    Stream* st = it->second;
    const size_t need = (size_t)frames * st->frameBytes + 256;
    if (st->give.size() < need) st->give.resize(need);
    *ppData = st->give.data();
    return S_OK;
}

static volatile LONG g_relN = 0;

// C++ body: ingest Spotify's frames, then feed the device as much retimed audio
// as it will currently accept.
static HRESULT pumpBody(Stream* st, UINT32 frames, DWORD flags, double sp) {
    Reentry guard;
    const LONG n = InterlockedIncrement(&g_relN);
    if (isUnity(sp)) st->rs.snap();   // keep 1.0x on the sample grid => bit-exact

    if (frames > 0) {
        const size_t samples = (size_t)frames * st->channels;
        if (st->ingest.size() < samples) st->ingest.resize(samples);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(st->ingest.data(), 0, samples * sizeof(float));
        else toFloat(st->give.data(), st->ingest.data(), samples, st->bits, st->isFloat);
        st->rs.push(st->ingest.data(), frames);
        g_srcFrames.fetch_add((long long)frames);
    }
    if (n <= 8) logf("[SS] rel#%ld in=%u pend=%.0f sp=%.2f\n", n, frames, st->rs.pending(), sp);

    for (int guard = 0; guard < 128; ++guard) {
        if (n <= 8) logf("[SS]  loop g=%d render=%p client=%p\n", guard, (void*)st->render, (void*)st->client);
        if (st->render == NULL || st->client == NULL) return S_OK;
        UINT32 realPad = 0;
        HRESULT hr = o_GetCurrentPadding(st->client, &realPad);
        if (n <= 8) logf("[SS]  pad=%u dev=%u hr=0x%08X\n", realPad, st->devFrames, hr);
        if (FAILED(hr)) { if (n <= 8) logf("[SS]  padErr 0x%08X\n", hr); return S_OK; }
        if (realPad >= st->devFrames) { if (n <= 8) logf("[SS]  devFull pad=%u\n", realPad); return S_OK; }

        const UINT32 avail = st->devFrames - realPad;
        if (st->rs.pending() < sp * 4.0 + 4.0) {
            if (n <= 8) logf("[SS]  starve pend=%.0f avail=%u\n", st->rs.pending(), avail);
            return S_OK;
        }
        if (n <= 8) logf("[SS]  pre-resize avail=%u ch=%d scratch=%zu\n",
                         avail, st->channels, st->scratch.size());
        if (st->scratch.size() < (size_t)avail * st->channels)
            st->scratch.resize((size_t)avail * st->channels);

        if (n <= 8) logf("[SS]  pre-process\n");
        const size_t got = st->rs.process(st->scratch.data(), avail, sp);
        if (n <= 8) logf("[SS]  post-process got=%u\n", (unsigned)got);
        if (got == 0) { if (n <= 8) logf("[SS]  got0 avail=%u pend=%.0f\n", avail, st->rs.pending()); return S_OK; }

        BYTE* dst = NULL;
        hr = o_GetBuffer(st->render, (UINT32)got, &dst);
        if (FAILED(hr) || dst == NULL) {
            if (n <= 8) logf("[SS]  getBufErr 0x%08X got=%u avail=%u\n", hr, (unsigned)got, avail);
            return S_OK;
        }
        fromFloat(st->scratch.data(), dst, got * (size_t)st->channels, st->bits, st->isFloat);
        hr = o_ReleaseBuffer(st->render, (UINT32)got, 0);
        if (SUCCEEDED(hr)) g_outFrames.fetch_add((long long)got);
        if (n <= 8) logf("[SS]  wrote=%u avail=%u hr=0x%08X\n", (unsigned)got, avail, hr);
        if (FAILED(hr)) return S_OK;
        if (got < (size_t)avail) return S_OK;
    }
    return S_OK;
}

// No C++ objects in this frame, so SEH is legal: never let a fault reach Spotify.
static HRESULT pumpSafe(Stream* st, UINT32 frames, DWORD flags, double sp) {
    __try {
        HRESULT hr = pumpBody(st, frames, flags, sp);
        st->faults = 0;                 // a clean pass clears the streak
        return hr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("[SS] !! fault 0x%08lX in pump (streak %d)\n",
             (unsigned long)GetExceptionCode(), st->faults + 1);
        // Tolerate the odd hiccup. Only if a stream keeps faulting do we stand
        // down, and only for that stream - a stale one must never disable the
        // whole engine for the rest of the process's life.
        if (++st->faults >= 8) st->faulted = true;
        return S_OK;
    }
}

static HRESULT STDMETHODCALLTYPE h_ReleaseBuffer(IAudioRenderClient* self, UINT32 frames, DWORD flags) {
    if (g_reentry) return o_ReleaseBuffer(self, frames, flags);
    std::unique_lock<std::recursive_mutex> lk(g_mtx);
    std::unordered_map<IAudioRenderClient*, Stream*>::iterator it = g_byRender.find(self);
    if (it == g_byRender.end() || !it->second->active) {
        lk.unlock();
        return o_ReleaseBuffer(self, frames, flags);
    }
    Stream* st = it->second;
    return pumpSafe(st, frames, flags, speedFor(st));
}

static HRESULT STDMETHODCALLTYPE h_Stop(IAudioClient* self) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        std::unordered_map<IAudioClient*, Stream*>::iterator it = g_byClient.find(self);
        if (it != g_byClient.end()) it->second->rs.reset();
    }
    return o_Stop(self);
}

static HRESULT STDMETHODCALLTYPE h_Reset(IAudioClient* self) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        std::unordered_map<IAudioClient*, Stream*>::iterator it = g_byClient.find(self);
        if (it != g_byClient.end()) it->second->rs.reset();
    }
    return o_Reset(self);
}

// -------------------------------------------------------------- vtable set --
static bool patch(void** vtbl, int index, void* hook, void** orig) {
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return true;
}

static bool installHooks() {
    if (o_GetBuffer != NULL && o_ReleaseBuffer != NULL) return true;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = NULL;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || en == NULL) return false;
    IMMDevice* dev = NULL;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || dev == NULL) { en->Release(); return false; }

    IAudioClient* cl = NULL;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&cl)) || cl == NULL) {
        dev->Release(); en->Release(); return false;
    }
    g_bootstrapClient = cl;

    WAVEFORMATEX* mix = NULL;
    if (FAILED(cl->GetMixFormat(&mix)) || mix == NULL) { cl->Release(); dev->Release(); en->Release(); return false; }

    void** clv = *(void***)cl;
    patch(clv, 3,  (void*)&h_Initialize,        (void**)&o_Initialize);
    patch(clv, 4,  (void*)&h_GetBufferSize,     (void**)&o_GetBufferSize);
    patch(clv, 6,  (void*)&h_GetCurrentPadding, (void**)&o_GetCurrentPadding);
    patch(clv, 11, (void*)&h_Stop,              (void**)&o_Stop);
    patch(clv, 12, (void*)&h_Reset,             (void**)&o_Reset);
    patch(clv, 14, (void*)&h_GetService,        (void**)&o_GetService);

    // Initialise the probe through the ORIGINAL so we can reach IAudioRenderClient's vtable.
    if (SUCCEEDED(o_Initialize(cl, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, NULL))) {
        IAudioRenderClient* rc = NULL;
        if (SUCCEEDED(o_GetService(cl, __uuidof(IAudioRenderClient), (void**)&rc)) && rc != NULL) {
            void** rcv = *(void***)rc;
            patch(rcv, 3, (void*)&h_GetBuffer,     (void**)&o_GetBuffer);
            patch(rcv, 4, (void*)&h_ReleaseBuffer, (void**)&o_ReleaseBuffer);
            rc->Release();
        }
    }
    CoTaskMemFree(mix);
    cl->Release(); dev->Release(); en->Release();
    g_bootstrapClient = NULL;
    logf("[SS] hooks installed\n");
    return (o_GetBuffer != NULL && o_ReleaseBuffer != NULL);
}

// ------------------------------------------------------ control HTTP server --
static void sendAll(SOCKET s, const char* p, int n) {
    while (n > 0) { int k = send(s, p, n, 0); if (k <= 0) break; p += k; n -= k; }
}

static DWORD WINAPI serverThread(LPVOID) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return 0;
    BOOL yes = TRUE; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(ls, (sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 16) != 0) { closesocket(ls); return 0; }
    logf("[SS] control server on 127.0.0.1:%u\n", (unsigned)kPort);

    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        char buf[2048];
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            const char* q = strstr(buf, "/speed");
            if (q != NULL) {
                const char* v = strstr(q, "v=");
                if (v != NULL) { double s = atof(v + 2); if (s > 0) g_speed.store(clampSpeed(s)); }
            }
            if (strstr(buf, "/bypass") != NULL) {
                const char* v = strstr(buf, "v=");
                if (v != NULL) g_enabled.store(atoi(v + 2) == 0);
            }
            int streams = 0, faulted = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(g_mtx);
                streams = (int)g_byRender.size();
                for (std::unordered_map<IAudioRenderClient*, Stream*>::iterator r = g_byRender.begin();
                     r != g_byRender.end(); ++r) {
                    if (r->second != NULL && r->second->faulted) ++faulted;
                }
            }
            char body[320];
            int bl = _snprintf_s(body, sizeof(body), _TRUNCATE,
                "{\"ok\":true,\"speed\":%.4f,\"enabled\":%s,\"min\":%.2f,\"max\":%.2f,"
                "\"hooked\":%s,\"streams\":%d,\"faulted\":%d,\"src\":%lld,\"out\":%lld}",
                g_speed.load(), g_enabled.load() ? "true" : "false", kMinSpeed, kMaxSpeed,
                (o_ReleaseBuffer != NULL) ? "true" : "false", streams, faulted,
                g_srcFrames.load(), g_outFrames.load());
            char hdr[384];
            int hl = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n", bl);
            sendAll(c, hdr, hl);
            sendAll(c, body, bl);
        }
        closesocket(c);
    }
    closesocket(ls); WSACleanup();
    return 0;
}

static DWORD WINAPI initThread(LPVOID) {
    // Start the control server first so the knob can always see us, even while
    // the hooks are still coming up.
    CreateThread(NULL, 0, serverThread, NULL, 0, NULL);

    // Keep trying forever. Right after the machine wakes from sleep the default
    // audio endpoint can be missing for a while; giving up after a fixed number
    // of tries would leave the engine loaded but permanently deaf.
    int tries = 0;
    while (!installHooks()) {
        ++tries;
        Sleep(tries < 60 ? 500 : 5000);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(NULL, 0, initThread, NULL, 0, NULL);
    }
    return TRUE;
}
