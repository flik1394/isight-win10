// miccheck.cpp - "did the virtual microphone actually work?" in one command.
//
// Answers four questions and writes the whole thing to miccheck.txt so it can
// be sent back as a single file:
//
//   1. Is the kernel driver reachable?      (\\.\IsightMicCtl + its counters)
//   2. Is PortCls actually streaming?       (Played must advance while RUN)
//   3. Did an endpoint appear?              (MMDevice enumeration, by name)
//   4. What comes BACK out?                 (WASAPI capture -> stats + WAV)
//
// (4) is the one that matters: it captures from "iSight Microphone (FireWire)"
// the same way WeChat would, and dumps miccheck-capture.wav.  If the feeder is
// pushing audio at the time, that file has it in it -- and the wav can be
// analysed offline with the same scripts that analysed the camera captures.
//
// Build (see the vmic job):
//   cl /nologo /O2 /W3 /EHsc /MD /I"drivers\isightmic" tools\miccheck.cpp ^
//      ole32.lib /Fe:isight-miccheck.exe
//
// Usage:
//   isight-miccheck.exe [seconds]     (default 3)

#include <windows.h>
#include <winioctl.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "isightmic.h"

#pragma comment(lib, "ole32.lib")

static FILE* g_rep = NULL;

static void say(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    if (g_rep) { fprintf(g_rep, "%s\n", buf); fflush(g_rep); }
}

// ---------------------------------------------------------------------------
// 1 + 2 -- the driver's control device and its counters
// ---------------------------------------------------------------------------
struct StatusProbe {
    bool     opened;
    DWORD    openError;
    ULONG    buffered, pushed, played, starved, streams, state, opens;
    ULONG    playedFirst, playedLast;
    int      samples;
    bool     advanced;      // Played moved during the poll -> PortCls is pulling
};

static const char* StateName(ULONG s) {
    switch (s) {
    case 0: return "STOP";
    case 1: return "ACQUIRE";
    case 2: return "PAUSE";
    case 3: return "RUN";
    default: return "?";
    }
}

static bool ReadStatus(HANDLE h, ISIGHTMIC_STATUS* out, DWORD* err) {
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_ISIGHTMIC_GETSTATUS, NULL, 0, out,
                         sizeof(*out), &got, NULL)) {
        if (err) *err = GetLastError();
        return false;
    }
    return true;
}

static void ProbeDriver(StatusProbe* p, int seconds) {
    ZeroMemory(p, sizeof(*p));
    HANDLE h = CreateFileW(L"\\\\.\\IsightMicCtl", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        p->opened = false;
        p->openError = GetLastError();
        say("[1] control device  : NOT reachable (\\\\.\\IsightMicCtl, error 0x%08X)",
            p->openError);
        say("    -> isightmic.sys is not loaded, or the device node was never created.");
        return;
    }
    p->opened = true;
    say("[1] control device  : open OK");

    ISIGHTMIC_STATUS st;
    DWORD err = 0;
    if (!ReadStatus(h, &st, &err)) {
        say("    GETSTATUS failed (0x%08X)", err);
        CloseHandle(h);
        return;
    }

    // Watch the counters for `seconds` so we can tell a live stream from a
    // device that merely exists.
    int ticks = seconds * 4;
    if (ticks < 4) ticks = 4;
    ULONG prevPlayed = st.Played;
    for (int i = 0; i < ticks; i++) {
        Sleep(250);
        ISIGHTMIC_STATUS s2;
        if (!ReadStatus(h, &s2, &err)) break;
        st = s2;
        p->samples++;
    }
    p->buffered = st.Buffered;
    p->pushed   = st.Pushed;
    p->played   = st.Played;
    p->starved  = st.Starved;
    p->streams  = st.Streams;
    p->state    = st.State;
    p->opens    = st.Opens;
    p->playedFirst = prevPlayed;
    p->playedLast  = st.Played;
    p->advanced    = (st.Played != prevPlayed);

    say("[2] driver counters : state=%s streams=%u opens=%u",
        StateName(st.State), st.Streams, st.Opens);
    say("                      pushed=%u buffered=%u played=%u starved=%u",
        st.Pushed, st.Buffered, st.Played, st.Starved);
    if (st.Streams == 0)
        say("    -> no stream has been created yet: nothing has opened the endpoint.");
    else if (st.Played != prevPlayed)
        say("    -> PortCls is pulling audio (played advanced by %u bytes).",
            st.Played - prevPlayed);
    else
        say("    -> stream exists but played did not advance: the endpoint is not in RUN.");
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// 3 -- enumerate active capture endpoints
// ---------------------------------------------------------------------------
static bool FindCaptureEndpoint(IMMDevice** out, char* nameOut, size_t nameLen) {
    *out = NULL;
    IMMDeviceEnumerator* en = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) { say("[3] MMDeviceEnumerator failed (0x%08X)", hr); return false; }

    IMMDeviceCollection* col = NULL;
    hr = en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr) || !col) { say("[3] EnumAudioEndpoints failed (0x%08X)", hr); en->Release(); return false; }

    UINT n = 0;
    col->GetCount(&n);
    say("[3] capture endpoints: %u active", n);

    bool found = false;
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = NULL;
        if (FAILED(col->Item(i, &dev)) || !dev) continue;
        IPropertyStore* ps = NULL;
        char friendly[256] = "<no name>";
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                WideCharToMultiByte(CP_ACP, 0, pv.pwszVal, -1, friendly, sizeof(friendly), NULL, NULL);
            }
            PropVariantClear(&pv);
            ps->Release();
        }
        // An ANSI C locale system shows CJK names as '?', so also match on the
        // device id, which always contains the INF's hardware id.
        LPWSTR id = NULL;
        bool byId = false;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            char ansi[512];
            WideCharToMultiByte(CP_ACP, 0, id, -1, ansi, sizeof(ansi), NULL, NULL);
            if (strstr(ansi, "ISIGHTMIC")) byId = true;
            CoTaskMemFree(id);
        }
        bool isOurs = byId || strstr(friendly, "iSight") != NULL ||
                      strstr(friendly, "ISight") != NULL;
        say("      %u %-46s %s", i, friendly, isOurs ? "<-- OURS" : "");
        if (isOurs && !found) {
            found = true;
            *out = dev;
            strncpy_s(nameOut, nameLen, friendly, _TRUNCATE);
            continue;                 // keep it, do not Release
        }
        dev->Release();
    }
    col->Release();
    en->Release();

    if (!found) {
        say("    -> \"iSight Microphone (FireWire)\" is NOT in the capture list.");
        say("       Check Device Manager for the driver, and that testsigning is on.");
    }
    return found;
}

// ---------------------------------------------------------------------------
// 4 -- capture from the endpoint and report what actually comes out
// ---------------------------------------------------------------------------
static void WriteWav16(const char* path, const short* s, UINT n);

static void CaptureFrom(IMMDevice* dev, int seconds) {
    IAudioClient* ac = NULL;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&ac);
    if (FAILED(hr) || !ac) { say("[4] Activate(IAudioClient) failed (0x%08X)", hr); return; }

    WAVEFORMATEX* mix = NULL;
    hr = ac->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { say("[4] GetMixFormat failed (0x%08X)", hr); ac->Release(); return; }
    bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                   (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 32);
    say("[4] endpoint format : %u Hz / %u ch / %u bit / %s", mix->nSamplesPerSec,
        mix->nChannels, mix->wBitsPerSample, isFloat ? "float" : "pcm");

    // 200 ms of buffering; shared mode lets the audio engine convert for us.
    REFERENCE_TIME dur = 2000000;
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, mix, NULL);
    if (FAILED(hr)) {
        say("    IAudioClient::Initialize failed (0x%08X) -- endpoint busy or disabled?", hr);
        CoTaskMemFree(mix); ac->Release(); return;
    }

    IAudioCaptureClient* cap = NULL;
    hr = ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    if (FAILED(hr) || !cap) { say("    GetService(IAudioCaptureClient) failed (0x%08X)", hr);
                              CoTaskMemFree(mix); ac->Release(); return; }

    hr = ac->Start();
    if (FAILED(hr)) { say("    IAudioClient::Start failed (0x%08X)", hr);
                      cap->Release(); CoTaskMemFree(mix); ac->Release(); return; }

    const UINT capFrames = (UINT)mix->nSamplesPerSec * (UINT)seconds;
    short*  mono16 = (short*)malloc((size_t)capFrames * 2);
    UINT    written = 0;
    double  sum2 = 0.0, sum = 0.0;
    long long n = 0;
    int     peak = 0;
    DWORD   t0 = GetTickCount();

    while (written < capFrames && (GetTickCount() - t0) < (DWORD)(seconds * 1000 + 3000)) {
        Sleep(20);
        UINT32 packet = 0;
        if (FAILED(cap->GetNextPacketSize(&packet))) break;
        while (packet > 0) {
            BYTE* data = NULL; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, NULL, NULL))) break;
            for (UINT32 f = 0; f < frames && written < capFrames; f++, written++) {
                double v;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    v = 0.0;
                } else if (isFloat) {
                    const float* fp = (const float*)data + (size_t)f * mix->nChannels;
                    double acc = 0; for (int c = 0; c < mix->nChannels; c++) acc += fp[c];
                    v = acc / mix->nChannels;
                } else {
                    const short* sp = (const short*)(data + (size_t)f * mix->nBlockAlign);
                    int acc = 0; for (int c = 0; c < mix->nChannels; c++) acc += sp[c];
                    v = (acc / (double)mix->nChannels) / 32768.0;
                }
                if (v > 1.0) v = 1.0; if (v < -1.0) v = -1.0;
                int s = (int)lround(v * 32767.0);
                if (abs(s) > peak) peak = abs(s);
                sum += v; sum2 += v * v; n++;
                mono16[written] = (short)s;
            }
            cap->ReleaseBuffer(frames);
            if (FAILED(cap->GetNextPacketSize(&packet))) { packet = 0; break; }
        }
    }

    cap->Release();
    ac->Stop();
    ac->Release();
    CoTaskMemFree(mix);

    if (n == 0) {
        say("    captured 0 frames -- the endpoint produced nothing.");
        free(mono16);
        return;
    }
    double rms = sqrt(sum2 / (double)n);
    double dc  = sum / (double)n;
    say("[4] captured %lld samples (%.2f s) from the endpoint", n, n / 48000.0);
    say("    peak = %d / 32767  (%.1f dBFS)   rms = %.0f  (%.1f dBFS)   dc = %.4f",
        peak, 20.0 * log10((peak ? peak : 1) / 32767.0),
        rms * 32767.0, 20.0 * log10(rms > 0 ? rms : 1e-9), dc);
    if (peak < 8)
        say("    -> SILENT. Either no feeder is pushing (isight-micsvc.exe), or the push"
            " is not reaching the endpoint.");
    else
        say("    -> AUDIO PRESENT. miccheck-capture.wav holds it.");

    WriteWav16("miccheck-capture.wav", mono16, written);
    say("    wrote miccheck-capture.wav (%u samples)", written);
    free(mono16);
}

static void WriteWav16(const char* path, const short* s, UINT n) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    UINT bytes = n * 2;
    struct { char riff[4]; DWORD size; char wave[4]; char fmt[4]; DWORD fmtSize;
             WORD tag; WORD ch; DWORD rate; DWORD brate; WORD align; WORD bits;
             char data[4]; DWORD dsize; } h;
    memcpy(h.riff, "RIFF", 4); h.size = 36 + bytes; memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);  h.fmtSize = 16;      h.tag = 1; h.ch = 1;
    h.rate = 48000; h.brate = 96000; h.align = 2; h.bits = 16;
    memcpy(h.data, "data", 4); h.dsize = bytes;
    fwrite(&h, 1, sizeof(h), f);
    fwrite(s, 1, bytes, f);
    fclose(f);
}

int main(int argc, char** argv) {
    int seconds = (argc > 1) ? atoi(argv[1]) : 3;
    if (seconds < 1) seconds = 1;
    if (seconds > 30) seconds = 30;

    g_rep = fopen("miccheck.txt", "w");
    say("=== iSight virtual microphone check ===  %s", __DATE__);
    say("driver binary tag: %s", "isightmic.sys (PortCls WaveCyclic, mono 48k/16)");

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    StatusProbe p;
    ProbeDriver(&p, 2);

    IMMDevice* dev = NULL;
    char name[256] = "";
    if (FindCaptureEndpoint(&dev, name, sizeof(name))) {
        say("    -> using \"%s\"", name);
        CaptureFrom(dev, seconds);
        dev->Release();
    }

    say("=== done ===");
    if (g_rep) { fclose(g_rep); g_rep = NULL; }
    printf("\nreport written to miccheck.txt\n");
    CoUninitialize();
    return 0;
}
