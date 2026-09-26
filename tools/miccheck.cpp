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
#include <setupapi.h>
// The KS GUIDs (KSCATEGORY_AUDIO, KSPROPSETID_Pin, ...) are declared by ks.h
// as extern references whose definitions live in the *kernel* ks.lib.  A user
// -mode exe cannot link those, so include initguid.h first: it flips INITGUID
// on and every DEFINE_GUIDSTRUCT in ks.h/ksmedia.h is then defined right here.
// Note ks.h/ksmedia.h ship with the Windows SDK, but ksuser.h does NOT (it is
// WDK-only, and the CI checker step compiles with SDK includes only) -- so
// KsCreatePin is declared locally and loaded from ksuser.dll at runtime.
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>

// ksuser.h equivalent, without the WDK header.
typedef LONG (WINAPI *PFN_KsCreatePin)(HANDLE FilterHandle,
                                       PKSPIN_CONNECT Connect,
                                       ACCESS_MASK DesiredAccess,
                                       PHANDLE ConnectionHandle);

// Layout-identical mirror of KSPIN_CONNECT.  The trailing PIN_DIRECTION member
// and the KSPIN_INTERFACE_STANDARD id macro are named differently between the
// WDK's ks.h and whatever ks.h the CI checker step picks up (error C2065 /
// C2039 in run 36161830770), so stop depending on either.  We do NOT use the
// SDK's KSPIN_INTERFACE/KSPIN_MEDIUM types either: the identifier pair is our
// own GUID+ULONG+ULONG, which is the wire format.  Arithmetic note (C2118 in
// run 36165821770): GUID alone is 16 bytes, so an identifier is 24, NOT 16 --
// real KSPIN_CONNECT on x64 = Interface(24) Medium(24) PinId(4) [pad 4]
// PinToHandle(8) Priority(8) = 72 bytes, and the appended KSDATAFORMAT must
// begin at offset 72.  Earlier guesses (40-byte header, then a 56-byte one)
// made the kernel parse format bytes as PinToHandle/Priority or read a bogus
// PinId -> E_FAIL.
typedef struct {
GUID Set;
ULONG Id;
ULONG Flags;
} ISIGHT_KS_IDENTIFIER;
typedef struct {
ISIGHT_KS_IDENTIFIER Interface; // {Set GUID, Id, Flags}   24 bytes
ISIGHT_KS_IDENTIFIER Medium;    // {Set GUID, Id, Flags}   24 bytes
ULONG           PinId;
ULONG           _pad;           // natural alignment before the HANDLE
HANDLE          PinToHandle;    // NULL: create a new pin instance
ULONG           PriorityClass;  // KSPRIORITY_NORMAL
ULONG           PrioritySubclass;
} ISIGHT_PIN_CONNECT;
// Compile-time guards: the appended KSDATAFORMAT must begin exactly where
// the real KSPIN_CONNECT ends (72 bytes on x64).
typedef char isight_guid_size_check[(sizeof(GUID) == 16) ? 1 : -1];
typedef char isight_ksid_size_check[(sizeof(ISIGHT_KS_IDENTIFIER) == 24) ? 1 : -1];
typedef char isight_pin_connect_size_check[(sizeof(ISIGHT_PIN_CONNECT) == 72) ? 1 : -1];
typedef char isight_fmt_offset_check[(offsetof(ISIGHT_PIN_CONNECT, PinToHandle) == 56) ? 1 : -1];
typedef char isight_pinid_offset_check[(offsetof(ISIGHT_PIN_CONNECT, PinId) == 48) ? 1 : -1];
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "isightmic.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "setupapi.lib")
// KsCreatePin is loaded from ksuser.dll at runtime (see the typedef above);
// ksuser.lib also exists, but only in the WDK lib set, not the SDK one.

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

static bool ReadDiag(HANDLE h, ISIGHTMIC_DIAG* out) {
    DWORD got = 0;
    return DeviceIoControl(h, IOCTL_ISIGHTMIC_GETDIAG, NULL, 0, out,
                           sizeof(*out), &got, NULL) && got >= sizeof(*out);
}

// ---------------------------------------------------------------------------
// The driver-side call trace
//
// WASAPI answers AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008) for a whole family
// of completely different failures: the audio stack never opened the filter,
// opened it but never asked about a data format, asked about formats but never
// instantiated the pin, or instantiated the pin and failed inside NewStream.
// All four look identical from the outside, and they need opposite fixes.
// These counters are the only place the difference is visible.  Read the
// columns in order: the first one still at zero is the step that never ran.
// ---------------------------------------------------------------------------
static void ProbeDiag(const char* when) {
    HANDLE h = CreateFileW(L"\\\\.\\IsightMicCtl", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        say("    call trace (%s): control device unavailable (err=%u)",
            when, GetLastError());
        return;
    }
    ISIGHTMIC_DIAG d;
    if (!ReadDiag(h, &d)) {
        say("    call trace (%s): GETDIAG failed (err=%u) -- this .sys predates v24",
            when, GetLastError());
        CloseHandle(h);
        return;
    }
    CloseHandle(h);

    say("    call trace (%s):", when);
    say("        filter instances     wave=%u topology=%u",
        d.WaveInitCalls, d.TopoInitCalls);
    say("        format intersection  wave=%u (length probes=%u) topology=%u",
        d.WaveIntersect, d.WaveIntersectProbe, d.TopoIntersect);
    say("        last intersection    pin=%u outLen=%u status=0x%08X reqSpec=%08X",
        d.WaveIntersectLastPin, d.WaveIntersectLastOutLen,
        d.WaveIntersectLastStatus, d.WaveIntersectReqSpec);
    say("        NewStream            entered=%u failed=%u"
        "  [dma=%u init=%u svcgrp=%u last=0x%08X]",
        d.NewStreamEntered, d.NewStreamFailed,
        d.FailDma, d.FailStreamInit, d.FailServiceGroup, d.LastFailStatus);
    say("        RUN chain            state=%u dpc=%u notify=%u service=%u"
        "  (state: 0=STOP 1=ACQ 2=PAUSE 3=RUN)",
        d.StateLast, d.DpcFires, d.NotifyCalls, d.ServiceCalls);

    if (d.WaveInitCalls == 0)
        say("    -> the audio stack never opened the wave filter.");
    else if (d.WaveIntersect == 0)
        say("    -> the filter was opened but never asked for a data format.");
    else if (d.NewStreamEntered == 0)
        say("    -> formats were negotiated, but PortCls never instantiated the pin.");
    else if (d.NewStreamFailed != 0)
        say("    -> the pin was instantiated and NewStream failed inside the driver.");
    else
        say("    -> a capture stream was created successfully.");
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

    // Which .sys is actually loaded.  Without this, "the driver is installed"
    // and "the driver is the one I just built" are the same sentence.
    char build[128] = "";
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_ISIGHTMIC_GETBUILD, NULL, 0, build,
                        sizeof(build) - 1, &got, NULL) && got > 0) {
        build[(got < sizeof(build)) ? got : (sizeof(build) - 1)] = 0;
        say("    driver build     : %s", build);
    } else {
        say("    driver build     : not reported (this .sys predates the GETBUILD ioctl)");
    }

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
    ProbeDiag("before");
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// 2b -- bypass the audio engine entirely: talk to the wave filter directly.
//
// V27 left us with a strange state: 684 format negotiations, every single one
// answered SUCCESS with a fully written format, and still NewStream entered=0.
// When PortCls instantiates a pin it first asks KSPROPERTY_PIN_GLOBALCINSTANCES
// for free slots; if that says 0, it refuses without ever calling NewStream --
// which is exactly the symptom we see.  This probe reads that property for
// every pin, then tries KsCreatePin on the capture host pin with the same
// Standard/DevIO interface-medium pair the audio engine uses.  Whatever
// happens, the answer is decisive:
//   - instances max=0            -> the pin descriptor is the bug (V28 fixes it)
//   - KsCreatePin fails err=N    -> the create path fails there, N names the layer
//   - KsCreatePin succeeds       -> the pin works; the fault is above (topology
//                                   bridge / engine side)
// ---------------------------------------------------------------------------
static const char* FlowName(ULONG f) {
    switch (f) {
    case KSPIN_DATAFLOW_IN:  return "IN";
    case KSPIN_DATAFLOW_OUT: return "OUT";
    default: return "?";
    }
}

static const char* CommName(ULONG c) {
    switch (c) {
    case KSPIN_COMMUNICATION_NONE:      return "NONE";
    case KSPIN_COMMUNICATION_SINK:      return "SINK";
    case KSPIN_COMMUNICATION_SOURCE:    return "SOURCE";
    case KSPIN_COMMUNICATION_BOTH:      return "BOTH";
    case KSPIN_COMMUNICATION_BRIDGE:    return "BRIDGE";
    default: return "?";
    }
}

static bool KsPinGet(HANDLE f, ULONG pinId, ULONG propId,
                     void* out, DWORD outLen, DWORD* got) {
    KSP_PIN kp;
    ZeroMemory(&kp, sizeof(kp));
    kp.Property.Set   = KSPROPSETID_Pin;
    kp.Property.Id    = propId;
    kp.Property.Flags = KSPROPERTY_TYPE_GET;
    kp.PinId          = pinId;
    return DeviceIoControl(f, IOCTL_KS_PROPERTY, &kp, sizeof(kp),
                           out, outLen, got, NULL) ? true : false;
}

static void DirectPinProbe(void) {
    // 1. find our wave filter among KSCATEGORY_AUDIO interfaces
    HDEVINFO set = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        say("[2b] direct pin probe: SetupDi failed (err=%u)", GetLastError());
        return;
    }

    WCHAR path[1024] = L"";
    bool  havePath = false;
    for (DWORD i = 0; i < 64 && !havePath; i++) {
        SP_DEVICE_INTERFACE_DATA di;
        di.cbSize = sizeof(di);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &KSCATEGORY_AUDIO, i, &di)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &di, NULL, 0, &need, NULL);
        if (!need) continue;
        BYTE* buf = (BYTE*)malloc(need);
        if (!buf) break;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* dd = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        dd->cbSize = sizeof(*dd);
        if (SetupDiGetDeviceInterfaceDetailW(set, &di, dd, need, NULL, NULL)) {
            // the wave filter's reference string ends the path with "\wave";
            // the topology filter ends with "\topology".  Ignore the latter.
            size_t len = wcslen(dd->DevicePath);
            if (len > 5 && _wcsicmp(dd->DevicePath + len - 5, L"\\wave") == 0 &&
                wcsstr(dd->DevicePath, L"isightmic") != NULL) {
                wcsncpy_s(path, dd->DevicePath, _TRUNCATE);
                havePath = true;
            }
        }
        free(buf);
    }
    SetupDiDestroyDeviceInfoList(set);

    if (!havePath) {
        say("[2b] direct pin probe: no isightmic \\wave filter under KSCATEGORY_AUDIO.");
        return;
    }
    {
        char ansi[1024];
        WideCharToMultiByte(CP_ACP, 0, path, -1, ansi, sizeof(ansi), NULL, NULL);
        say("[2b] direct pin probe: wave filter found");
        say("    %s", ansi);
    }

    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        say("    open failed (err=%u)", GetLastError());
        return;
    }

    // 2. per-pin properties
    ULONG ctypes = 0; DWORD got = 0;
    if (!KsPinGet(f, 0, KSPROPERTY_PIN_CTYPES, &ctypes, sizeof(ctypes), &got) || got != 4) {
        say("    KSPROPERTY_PIN_CTYPES failed (err=%u)", GetLastError());
        CloseHandle(f);
        return;
    }
    say("    pins: %u", ctypes);

    int capturePin = -1;
    for (ULONG id = 0; id < ctypes; id++) {
        ULONG df = 0, comm = 0; DWORD g1 = 0, g2 = 0, g3 = 0;
        KSPIN_CINSTANCES inst;
        ZeroMemory(&inst, sizeof(inst));
        bool okDf  = KsPinGet(f, id, KSPROPERTY_PIN_DATAFLOW, &df, sizeof(df), &g1);
        bool okCm  = KsPinGet(f, id, KSPROPERTY_PIN_COMMUNICATION, &comm, sizeof(comm), &g2);
        bool okIn  = KsPinGet(f, id, KSPROPERTY_PIN_GLOBALCINSTANCES, &inst,
                              sizeof(inst), &g3);
        say("      pin %u: dataflow=%s comm=%s  instances max=%u current=%u"
            "  (df=%u cm=%u in=%u)",
            id,
            okDf ? FlowName(df) : "ERR",
            okCm ? CommName(comm) : "ERR",
            okIn ? inst.PossibleCount : 0,
            okIn ? inst.CurrentCount : 0,
            okDf ? 1 : 0, okCm ? 1 : 0, okIn ? 1 : 0);
        if (okDf && df == KSPIN_DATAFLOW_OUT && capturePin < 0) capturePin = (int)id;
    }

    // 3. the decisive attempt: create the capture pin ourselves
    if (capturePin < 0) {
        say("    no dataflow=OUT pin found -- nothing to create.");
        CloseHandle(f);
        return;
    }
    say("    trying KsCreatePin on pin %u (Standard/DevIO, OUT, +48k/stereo/16 format) ...",
        capturePin);

    ISIGHT_PIN_CONNECT conn;
    ZeroMemory(&conn, sizeof(conn));
    conn.Interface.Set  = KSINTERFACESETID_Standard;
    conn.Interface.Id   = 0;            // KSPIN_INTERFACE_STANDARD
    conn.Interface.Flags = 0;
    conn.Medium.Set     = KSMEDIUMSETID_Standard;
    conn.Medium.Id      = KSMEDIUM_STANDARD_DEVIO;
    conn.Medium.Flags   = 0;
    conn.PinId          = (ULONG)capturePin;
    conn._pad           = 0;
    conn.PinToHandle    = NULL;             // new pin instance
    conn.PriorityClass  = 1;                // KSPRIORITY_NORMAL
    conn.PrioritySubclass = 1;

    // KsCreatePin's documented contract: the KSPIN_CONNECT is followed by a
    // KSDATAFORMAT.  The bare-connect attempt got ERROR_INVALID_USER_BUFFER
    // (1784) -- the create handler wants the format appended, which is also
    // exactly what the audio engine sends after its 18k successful format
    // negotiations.  Use 48 kHz / stereo / 16-bit, the shared-mode candidate.
    BYTE buf[sizeof(ISIGHT_PIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX)];
    ZeroMemory(buf, sizeof(buf));
    CopyMemory(buf, &conn, sizeof(conn));
    KSDATAFORMAT_WAVEFORMATEX* fmt =
        (KSDATAFORMAT_WAVEFORMATEX*)(buf + sizeof(ISIGHT_PIN_CONNECT));
    fmt->DataFormat.FormatSize  = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    fmt->DataFormat.Flags       = 0;
    fmt->DataFormat.SampleSize  = 4;                        // 2 ch * 2 bytes
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
    fmt->WaveFormatEx.nChannels       = 2;
    fmt->WaveFormatEx.nSamplesPerSec  = 48000;
    fmt->WaveFormatEx.nBlockAlign     = 4;
    fmt->WaveFormatEx.wBitsPerSample  = 16;
    fmt->WaveFormatEx.cbSize          = 0;
    fmt->WaveFormatEx.nAvgBytesPerSec = 48000 * 4;

    HANDLE ph = NULL;
    HMODULE ksuser = LoadLibraryW(L"ksuser.dll");
    if (!ksuser) {
        say("    ksuser.dll not loadable (err=%u) -- cannot attempt pin creation.",
            GetLastError());
        CloseHandle(f);
        return;
    }
    PFN_KsCreatePin pKsCreatePin =
        (PFN_KsCreatePin)GetProcAddress(ksuser, "KsCreatePin");
    if (!pKsCreatePin) {
        say("    KsCreatePin not found in ksuser.dll (err=%u).", GetLastError());
        FreeLibrary(ksuser);
        CloseHandle(f);
        return;
    }
    LONG rc = pKsCreatePin(f, (PKSPIN_CONNECT)buf, GENERIC_READ | GENERIC_WRITE, &ph);
    if (rc == 0) {
        say("    KsCreatePin SUCCESS -- the pin instantiates fine from user mode.");
        say("    -> the fault is NOT in the create path; engine/topology side.");

        // [2c] Replay the exact property sequence the audio engine runs on a
        // freshly created pin before it will report a working endpoint:
        //   KSPROPERTY_CONNECTION_STATE  ACQUIRE -> PAUSE -> RUN
        //   KSPROPERTY_AUDIO_POSITION    (twice -- it must advance)
        //   KSPROPERTY_AUDIO_LATENCY / CHANNEL_CONFIG
        // Whatever step fails here is the step that makes WASAPI answer
        // AUDCLNT_E_ENDPOINT_CREATE_FAILED (0x88890008).
        //
        // KS property convention (verified against the ReactOS portcls
        // PinWaveCyclicState): the property handler receives (Request, Data)
        // and Data is the OUTPUT buffer for BOTH get and set -- so a set
        // passes the value in lpOutBuffer, not appended to the input buffer.
        // Appending it to the input gave STATUS_BUFFER_TOO_SMALL (err=122).
        DWORD got = 0;
        KSPROPERTY cprop;
        cprop.Set   = KSPROPSETID_Connection;
        cprop.Id    = KSPROPERTY_CONNECTION_STATE;
        cprop.Flags = KSPROPERTY_TYPE_SET;
        const char* names[4] = { "STOP", "ACQUIRE", "PAUSE", "RUN" };
        for (ULONG st = 1; st <= 3; st++) {
            ULONG val = st;
            BOOL ok = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &cprop,
                                      sizeof(cprop), &val, sizeof(val), &got, NULL);
            say("    [2c] set state %s: %s (err=%u)", names[st],
                ok ? "OK" : "FAIL", ok ? 0 : GetLastError());
        }

        KSPROPERTY gprop;
        gprop.Set   = KSPROPSETID_Audio;
        gprop.Flags = KSPROPERTY_TYPE_GET;

        // KSAUDIO_POSITION / KSAUDIO_CHANNEL_CONFIG member names differ between
        // SDK versions (C2039/C2440 in run 36208095723), so read raw 64-bit
        // words: position is two ULONGLONGs (byte pos + 100ns pos), latency a
        // single value, channel config a bitmask.
        unsigned __int64 posBuf[2]; ZeroMemory(posBuf, sizeof(posBuf));
        gprop.Id = KSPROPERTY_AUDIO_POSITION;
        BOOL ok1 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gprop, sizeof(gprop),
                                   posBuf, sizeof(posBuf), &got, NULL);
        say("    [2c] get position: %s pos=%llu qs=%llu (err=%u)",
            ok1 ? "OK" : "FAIL", ok1 ? posBuf[0] : 0, ok1 ? posBuf[1] : 0,
            ok1 ? 0 : GetLastError());

        gprop.Id = KSPROPERTY_AUDIO_LATENCY;
        ULONG lat = 0;
        BOOL ok2 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gprop, sizeof(gprop),
                                   &lat, sizeof(lat), &got, NULL);
        say("    [2c] get latency: %s value=%lu (err=%u)",
            ok2 ? "OK" : "FAIL", ok2 ? lat : 0, ok2 ? 0 : GetLastError());

        ULONG cc = 0;
        gprop.Id = KSPROPERTY_AUDIO_CHANNEL_CONFIG;
        BOOL ok3 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gprop, sizeof(gprop),
                                   &cc, sizeof(cc), &got, NULL);
        say("    [2c] get channel config: %s mask=0x%lX (err=%u)",
            ok3 ? "OK" : "FAIL", ok3 ? cc : 0, ok3 ? 0 : GetLastError());

        if (ok1) {
            Sleep(300);
            unsigned __int64 pos2[2]; ZeroMemory(pos2, sizeof(pos2));
            gprop.Id = KSPROPERTY_AUDIO_POSITION;
            BOOL ok4 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gprop,
                                       sizeof(gprop), pos2, sizeof(pos2),
                                       &got, NULL);
            say("    [2c] position after 300 ms: %s pos=%llu (advanced=%s, err=%u)",
                ok4 ? "OK" : "FAIL", ok4 ? pos2[0] : 0,
                (ok4 && pos2[0] != posBuf[0]) ? "yes" : "NO",
                ok4 ? 0 : GetLastError());
        }

        // leave the pin stopped and close it so [4] still has its instance.
        ULONG stopval = 0;   // KSSTATE_STOP
        DeviceIoControl(ph, IOCTL_KS_PROPERTY, &cprop, sizeof(cprop),
                        &stopval, sizeof(stopval), &got, NULL);
        Sleep(150);
        CloseHandle(ph);
    } else {
        DWORD e = (DWORD)rc;
        say("    KsCreatePin FAILED (rc=0x%08X / %u)", e, e);
        if (e == 0xC0000044 || e == 0x1F)
            say("    -> out of free pin instances: the descriptor's instance counts are the bug.");
        else if (e == 0xC000000D || e == 0xC0000001 || e == 0x57 || e == 1)
            say("    -> interface/medium/format rejected before the miniport saw it.");
        else if (e == 0xC000008A || e == 0xC0000242)
            say("    -> pin slots exhausted or filter busy.");
        else
            say("    -> the NTSTATUS above names the failing layer precisely.");
    }
    FreeLibrary(ksuser);
    CloseHandle(f);
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
    DirectPinProbe();

    IMMDevice* dev = NULL;
    char name[256] = "";
    if (FindCaptureEndpoint(&dev, name, sizeof(name))) {
        say("    -> using \"%s\"", name);
        CaptureFrom(dev, seconds);
        dev->Release();
    }
    // Read the trace again now that we have just tried to open the endpoint the
    // way WeChat does.  The delta between this and the "before" reading is what
    // our own attempt touched -- and if it is all zeros, our attempt never
    // reached the driver either, which is itself the answer.
    ProbeDiag("after");

    say("=== done ===");
    if (g_rep) { fclose(g_rep); g_rep = NULL; }
    printf("\nreport written to miccheck.txt\n");
    CoUninitialize();
    return 0;
}
