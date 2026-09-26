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

// Read the driver diag through a fresh control-device handle.  Used by [2f]
// to snapshot the counters before/after the RUN so the DELTAS show what this
// one pin's wakeup + copy path actually did (globals cannot distinguish pins).
static bool ReadDiagCtl(ISIGHTMIC_DIAG* out) {
    HANDLE h = CreateFileW(L"\\\\.\\IsightMicCtl", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = ReadDiag(h, out);
    CloseHandle(h);
    return ok;
}

static void PrintDiagDelta(const char* tag,
                           const ISIGHTMIC_DIAG* a, const ISIGHTMIC_DIAG* b) {
    say("    [2f] diag delta (%s):", tag);
    say("        dpc=%u notify=%u service=%u (full=%u)",
        b->DpcFires - a->DpcFires, b->NotifyCalls - a->NotifyCalls,
        b->ServiceCalls - a->ServiceCalls, b->ServiceFull - a->ServiceFull);
    say("        posgets=%u poslast=%u reqsvc=%u irpsdone=%u",
        b->PosGets - a->PosGets, b->PosLast,
        b->ReqSvc - a->ReqSvc, b->IrpDone - a->IrpDone);
    say("        dma: sysaddr=%u transfer=%u bufsize=%u copyfrom=%u"
        " copyto=%u phys=%u",
        b->DmaSysAddr - a->DmaSysAddr, b->DmaTransfer - a->DmaTransfer,
        b->DmaBufferSize - a->DmaBufferSize, b->DmaCopyFrom - a->DmaCopyFrom,
        b->DmaCopyTo - a->DmaCopyTo, b->DmaPhysAddr - a->DmaPhysAddr);
    say("        silence=%u posraw: linear=%u bufsize=%u (ring last=%u)",
        b->SilenceCalls - a->SilenceCalls, b->PosLinear, b->PosBufSize,
        b->PosLast);
    // The discriminator: silence>0 with copyfrom==0 means the port's
    // GetMapping failed every tick -> the queued IRP never entered the
    // port's irp queue.  silence==0 && copyfrom==0 means BufferLength was
    // 0 every tick -> ring-arithmetic problem instead.
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
    say("        position path        miniport GetPosition calls=%u last=%u"
        "  service(full)=%u irps done=%u",
        d.PosGets, d.PosLast, d.ServiceFull, d.IrpDone);
    say("        wakeup path          dpc->reqsvc=%u  (ReqSvc = IServiceGroup wakeups; the port services the stream + copies to the user IRP on these)",
        d.ReqSvc);
    say("        DMA methods          sysaddr=%u transfer=%u bufsize=%u alloc=%u"
        "  adapter=%u copyto=%u copyfrom=%u phys=%u",
        d.DmaSysAddr, d.DmaTransfer, d.DmaBufferSize, d.DmaAlloc,
        d.DmaAdapter, d.DmaCopyTo, d.DmaCopyFrom, d.DmaPhysAddr);

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

// ---------------------------------------------------------------------------
// [2f] the engine-faithful probe.  A bare RUN never advances the port's
// PlayOffset: ReactOS pinwc.cpp shows UpdateCommonBuffer only moves the
// position while IrpQueue::GetMapping() succeeds, i.e. only when the client
// has queued a stream buffer (IOCTL_KS_READ_STREAM).  The audio engine always
// queues its cyclic buffer before RUN -- so a position that stays flat even
// with a buffer queued is the real remaining wall.  The IRP submission may
// block on a synchronous pin handle, so it runs in a worker thread; the main
// thread drives ACQUIRE/PAUSE/RUN around it, exactly like audioses does.
// ---------------------------------------------------------------------------

// Use the SDK's real KSSTREAM_HEADER (miccheck already includes ks.h, and the
// CI builds with the same Windows SDK).  Verified against SDK 10.0.26100.0:
// KSTIME is EMBEDDED (16 bytes) and the x64 struct is 56 bytes, Data at
// offset 40.  Our hand-rolled 64-byte mirror put the fields at wrong offsets,
// so the kernel read Data from offset 32, got NULL, and rejected the IRP with
// ERROR_INVALID_USER_BUFFER (1784) -- the wrong-mirror lesson, again.
typedef char isight_hdr_size_check[(sizeof(KSSTREAM_HEADER) == 56) ? 1 : -1];

#define ISIGHT_2F_BUF   (192000)   // 1 s of 48 kHz stereo 16-bit
static __declspec(align(64)) unsigned char g_2f_buf[ISIGHT_2F_BUF];

static volatile LONG g_2f_phase = 0;   // 2 = DeviceIoControl returned
static DWORD g_2f_submit_err = 0;      // variant 1 (in+out buffers)
static DWORD g_2f_submit_err2 = 0;     // variant 0 (in-only, legacy)
static DWORD g_2f_got = 0;             // bytes the completed IRP reports

static DWORD WINAPI Probe2FSubmit(LPVOID arg) {
    HANDLE ph = (HANDLE)arg;
    KSSTREAM_HEADER hdr;
    ZeroMemory(&hdr, sizeof(hdr));
    hdr.Size        = (ULONG)sizeof(hdr);
    hdr.FrameExtent = ISIGHT_2F_BUF;
    hdr.Data        = g_2f_buf;
    DWORD got2 = 0;
    // Variant 1: headers in BOTH buffers.  IOCTL_KS_READ_STREAM is
    // METHOD_OUT_DIRECT; ReactOS' KsStreamIo/KsProbeStreamIrp reads the
    // length check against the OUTPUT buffer length and probes MdlAddress,
    // so a NULL out-buffer (len 0) trips the probe before the pin ever sees
    // the request -> ERROR_INVALID_USER_BUFFER (1784).  Passing the same
    // header memory as both in and out satisfies SystemBuffer copy AND mdl.
    BOOL ok = DeviceIoControl(ph, IOCTL_KS_READ_STREAM, &hdr, sizeof(hdr),
                              &hdr, sizeof(hdr), &got2, NULL);
    if (!ok) {
        g_2f_submit_err = GetLastError();
        // Variant 0: classic in-only submission (MSDN literal reading).
        ok = DeviceIoControl(ph, IOCTL_KS_READ_STREAM, &hdr, sizeof(hdr),
                             NULL, 0, &got2, NULL);
        g_2f_submit_err2 = ok ? 0 : GetLastError();
    }
    g_2f_got = got2;
    InterlockedExchange(&g_2f_phase, 2);
    return ok ? 0 : 1;
}

// Variant 2: submit a second read IRP while the pin is already in RUN.
// The PAUSE-state submission comes back "pending", yet the port never
// consumes it; if Windows portcls only wires stream IRPs into its service
// loop for IRPs queued after RUN, this variant gets consumed instead.
static volatile LONG g_2f_phase_r = 0;
static DWORD g_2f_submit_err_r = 0;
static DWORD g_2f_got_r = 0;

static DWORD WINAPI Probe2FSubmitRun(LPVOID arg) {
    HANDLE ph = (HANDLE)arg;
    KSSTREAM_HEADER hdr;
    ZeroMemory(&hdr, sizeof(hdr));
    hdr.Size        = (ULONG)sizeof(hdr);
    hdr.FrameExtent = ISIGHT_2F_BUF / 2;                  // second half
    hdr.Data        = g_2f_buf + ISIGHT_2F_BUF / 2;
    DWORD got2 = 0;
    BOOL ok = DeviceIoControl(ph, IOCTL_KS_READ_STREAM, &hdr, sizeof(hdr),
                              &hdr, sizeof(hdr), &got2, NULL);
    g_2f_submit_err_r = ok ? 0 : GetLastError();
    g_2f_got_r = got2;
    InterlockedExchange(&g_2f_phase_r, 2);
    return ok ? 0 : 1;
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

        // ---- [2d] what the audio engine actually does -------------------
        // The engine opens WaveCyclic capture pins with the LOOPED_STREAMING
        // interface (Id 1), not STREAMING (Id 0).  Our pin descriptor's
        // interface list is 0/NULL -- if that means "default (Id 0) only",
        // the engine's connect is rejected before the miniport ever sees it,
        // which matches "16 intersections, zero pin creations" exactly.
        conn.Interface.Id   = 1;    // KSINTERFACE_STANDARD_LOOPED_STREAMING
        CopyMemory(buf, &conn, sizeof(conn));
        HANDLE ph2 = NULL;
        LONG rc2 = pKsCreatePin(f, (PKSPIN_CONNECT)buf, GENERIC_READ, &ph2);
        if (rc2 == 0) {
            say("    [2d] KsCreatePin LOOPED_STREAMING (Id=1): SUCCESS");
            // node-targeted channel config -- what mix-format discovery asks
            KSP_NODE ksnode; ZeroMemory(&ksnode, sizeof(ksnode));
            ksnode.Property.Set   = KSPROPSETID_Audio;
            ksnode.Property.Id    = KSPROPERTY_AUDIO_CHANNEL_CONFIG;
            ksnode.Property.Flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_TOPOLOGY;
            ksnode.NodeId         = 0;   // the ADC node
            ULONG mask = 0;
            BOOL okc = DeviceIoControl(ph2, IOCTL_KS_PROPERTY, &ksnode,
                                       sizeof(ksnode), &mask, sizeof(mask),
                                       &got, NULL);
            say("    [2d] node channel config: %s mask=0x%lX (err=%u)",
                okc ? "OK" : "FAIL", okc ? mask : 0, okc ? 0 : GetLastError());

            // [2e] the engine's post-create validation: set RUN and wait for
            // AUDIO_POSITION to advance.  audioses tears the endpoint down
            // when the position stays flat -- matching "3 pins created, RUN
            // for ~200 ms, then ENDPOINT_CREATE_FAILED" in the trace.
            struct { KSPROPERTY p; ULONG st; } sb2;
            sb2.p.Set   = KSPROPSETID_Connection;
            sb2.p.Id    = KSPROPERTY_CONNECTION_STATE;
            sb2.p.Flags = KSPROPERTY_TYPE_SET;
            const char* nm2[4] = { "STOP", "ACQUIRE", "PAUSE", "RUN" };
            for (ULONG st2 = 1; st2 <= 3; st2++) {
                sb2.st = st2;
                BOOL oks = DeviceIoControl(ph2, IOCTL_KS_PROPERTY, &sb2,
                                           sizeof(sb2), &stopval,
                                           sizeof(stopval), &got, NULL);
                say("    [2e] set state %s: %s (err=%u)", nm2[st2],
                    oks ? "OK" : "FAIL", oks ? 0 : GetLastError());
            }
            KSPROPERTY gpos; ZeroMemory(&gpos, sizeof(gpos));
            gpos.Set = KSPROPSETID_Audio;
            gpos.Id  = KSPROPERTY_AUDIO_POSITION;
            gpos.Flags = KSPROPERTY_TYPE_GET;
            unsigned __int64 p0[2]; ZeroMemory(p0, sizeof(p0));
            BOOL og0 = DeviceIoControl(ph2, IOCTL_KS_PROPERTY, &gpos,
                                       sizeof(gpos), p0, sizeof(p0),
                                       &got, NULL);
            Sleep(500);
            unsigned __int64 p1[2]; ZeroMemory(p1, sizeof(p1));
            BOOL og1 = DeviceIoControl(ph2, IOCTL_KS_PROPERTY, &gpos,
                                       sizeof(gpos), p1, sizeof(p1),
                                       &got, NULL);
            say("    [2e] LOOPED position: first %s play=%llu write=%llu",
                og0 ? "OK" : "FAIL", og0 ? p0[0] : 0, og0 ? p0[1] : 0);
            say("    [2e] after 500 ms    %s play=%llu write=%llu (advanced=%s)",
                og1 ? "OK" : "FAIL", og1 ? p1[0] : 0, og1 ? p1[1] : 0,
                (og1 && p1[0] != p0[0]) ? "yes" : "NO");

            stopval = 0;
            DeviceIoControl(ph2, IOCTL_KS_PROPERTY, &cprop, sizeof(cprop),
                            &stopval, sizeof(stopval), &got, NULL);
            Sleep(100);
            CloseHandle(ph2);
        } else {
            say("    [2d] KsCreatePin LOOPED_STREAMING (Id=1): FAILED rc=0x%08X / %u",
                (DWORD)rc2, (DWORD)rc2);
            if (rc2 == 0xC000005B || (DWORD)rc2 == 0x80070057 || (DWORD)rc2 == 87)
                say("    -> Id=1 rejected while Id=0 works: the pin descriptor's");
            else
                say("    -> ");
            say("       NULL interface list is the wall; V32 must advertise both.");
        }

        // [2f] engine-faithful: LOOPED pin + queued read buffer + RUN.
        {
            say("    [2f] engine-faithful probe: LOOPED pin + queued read IRP + RUN");
            ISIGHT_PIN_CONNECT cf;
            ZeroMemory(&cf, sizeof(cf));
            cf.Interface.Set = KSINTERFACESETID_Standard;
            cf.Interface.Id  = 1;                 // LOOPED_STREAMING
            cf.Medium.Set    = KSMEDIUMSETID_Standard;
            cf.Medium.Id     = KSMEDIUM_STANDARD_DEVIO;
            cf.PinId         = (ULONG)capturePin;
            cf.PriorityClass    = 1;
            cf.PrioritySubclass = 1;
            BYTE buf3[sizeof(ISIGHT_PIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX)];
            ZeroMemory(buf3, sizeof(buf3));
            CopyMemory(buf3, &cf, sizeof(cf));
            KSDATAFORMAT_WAVEFORMATEX* f3 =
                (KSDATAFORMAT_WAVEFORMATEX*)(buf3 + sizeof(ISIGHT_PIN_CONNECT));
            f3->DataFormat.FormatSize  = sizeof(KSDATAFORMAT_WAVEFORMATEX);
            f3->DataFormat.SampleSize  = 4;
            f3->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
            f3->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
            f3->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
            f3->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
            f3->WaveFormatEx.nChannels       = 2;
            f3->WaveFormatEx.nSamplesPerSec  = 48000;
            f3->WaveFormatEx.nBlockAlign     = 4;
            f3->WaveFormatEx.wBitsPerSample  = 16;
            f3->WaveFormatEx.nAvgBytesPerSec = 48000 * 4;

            HANDLE ph3 = NULL;
            LONG rc3 = pKsCreatePin(f, (PKSPIN_CONNECT)buf3,
                                    GENERIC_READ | GENERIC_WRITE, &ph3);
            if (rc3 != 0) {
                say("    [2f] create LOOPED pin: FAIL rc=0x%08X", (DWORD)rc3);
            } else {
                ISIGHTMIC_DIAG d0, d1;
                bool haveD0 = ReadDiagCtl(&d0);
                KSPROPERTY gpos3;
                ZeroMemory(&gpos3, sizeof(gpos3));
                gpos3.Set   = KSPROPSETID_Audio;
                gpos3.Id    = KSPROPERTY_AUDIO_POSITION;
                gpos3.Flags = KSPROPERTY_TYPE_GET;
                for (ULONG st3 = 1; st3 <= 2; st3++) {
                    ULONG val3 = st3;
                    BOOL ok3 = DeviceIoControl(ph3, IOCTL_KS_PROPERTY, &cprop,
                                               sizeof(cprop), &val3,
                                               sizeof(val3), &got, NULL);
                    say("    [2f] set state %s: %s (err=%u)", names[st3],
                        ok3 ? "OK" : "FAIL", ok3 ? 0 : GetLastError());
                }
                // Queue the buffer at PAUSE (the engine's order).  A STOP-state
                // submission came back ERROR_BAD_COMMAND (22) -- portcls only
                // accepts stream IRPs once the pin is at least in PAUSE.
                HANDLE th = CreateThread(NULL, 0, Probe2FSubmit, ph3, 0, NULL);
                Sleep(300);
                say("    [2f] read IRP submit @PAUSE: %s (err=%u)",
                    g_2f_phase >= 2 ? "returned immediately" : "pending in flight",
                    g_2f_phase >= 2 ? g_2f_submit_err : 0);

                ULONG valr = 3;   // KSSTATE_RUN
                BOOL okr = DeviceIoControl(ph3, IOCTL_KS_PROPERTY, &cprop,
                                           sizeof(cprop), &valr, sizeof(valr),
                                           &got, NULL);
                say("    [2f] set state RUN: %s (err=%u)",
                    okr ? "OK" : "FAIL", okr ? 0 : GetLastError());
                // Variant 2: a second IRP queued AFTER RUN.  If the port only
                // consumes IRPs that arrive while the pin is in RUN, this one
                // gets serviced even if the PAUSE-queued one never does.
                HANDLE thr = NULL;
                if (okr) {
                    InterlockedExchange(&g_2f_phase_r, 0);
                    thr = CreateThread(NULL, 0, Probe2FSubmitRun, ph3, 0, NULL);
                    Sleep(200);
                    say("    [2f] read IRP submit @RUN: %s (err=%u)",
                        g_2f_phase_r >= 2 ? "returned immediately"
                                          : "pending in flight",
                        g_2f_phase_r >= 2 ? g_2f_submit_err_r : 0);
                }
                if (g_2f_phase >= 2 && g_2f_submit_err != 0 && th) {
                    // rejected at PAUSE: retry once, now that the pin RUNs
                    InterlockedExchange(&g_2f_phase, 0);
                    HANDLE th2 = CreateThread(NULL, 0, Probe2FSubmit, ph3,
                                              0, NULL);
                    if (th2) CloseHandle(th2);
                    Sleep(300);
                    say("    [2f] read IRP submit retry @RUN: %s (err=%u)",
                        g_2f_phase >= 2 ? "returned immediately"
                                        : "pending in flight",
                        g_2f_phase >= 2 ? g_2f_submit_err : 0);
                }
                unsigned __int64 prev = 0;
                int moved = 0;
                for (int i = 0; i < 10; i++) {
                    Sleep(100);
                    unsigned __int64 q[2];
                    ZeroMemory(q, sizeof(q));
                    BOOL og = DeviceIoControl(ph3, IOCTL_KS_PROPERTY, &gpos3,
                                              sizeof(gpos3), q, sizeof(q),
                                              &got, NULL);
                    if (og && q[0] != prev) moved++;
                    if (og) prev = q[0];
                    if (i == 0 || i == 4 || i == 9)
                        say("    [2f] poll %d: %s play=%llu write=%llu", i + 1,
                            og ? "OK" : "FAIL", og ? q[0] : 0, og ? q[1] : 0);
                }
                say("    [2f] position advanced in %d/10 polls -> %s", moved,
                    moved
                        ? "PORT COPIES DATA -- driver path proven end to end"
                        : "STILL FROZEN even with a buffer queued");

                if (haveD0 && ReadDiagCtl(&d1))
                    PrintDiagDelta("this pin's RUN window", &d0, &d1);
                else
                    say("    [2f] diag delta: unavailable");

                ULONG val0 = 0;   // KSSTATE_STOP
                DeviceIoControl(ph3, IOCTL_KS_PROPERTY, &cprop, sizeof(cprop),
                                &val0, sizeof(val0), &got, NULL);
                Sleep(200);
                CloseHandle(ph3);          // cancels the worker's pending IRP
                if (th) WaitForSingleObject(th, 3000);
                if (th) CloseHandle(th);
                if (thr) WaitForSingleObject(thr, 3000);
                if (thr) CloseHandle(thr);
                say("    [2f] worker exited (in+out err=%u, in-only err=%u,"
                    " completed bytes=%u)",
                    g_2f_submit_err, g_2f_submit_err2, g_2f_got);
                say("    [2f] RUN-variant exited (err=%u, completed bytes=%u)",
                    g_2f_submit_err_r, g_2f_got_r);
            }
        }
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
// [2g] differential probe against a WORKING capture filter.  The same machine
// runs Realtek's capture driver (rtmicinwave); the audio engine works with it
// and not with ours.  Open both wave filters, instantiate one capture pin on
// each (PAUSE, no data flows), run the identical property battery, and print
// the answers side by side.  Any property the two answer differently is a
// suspect for the 0x88890008 wall.  Pure user mode -- no driver change needed.
// ---------------------------------------------------------------------------
struct PropResult { bool ok; DWORD err; DWORD got; unsigned __int64 v[4]; };

static PropResult PinProp(HANDLE ph, const GUID& set, ULONG id, DWORD outLen) {
    PropResult r; r.ok = false; r.err = 0; r.got = 0;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = 0;
    KSPROPERTY p; ZeroMemory(&p, sizeof(p));
    p.Set = set; p.Id = id; p.Flags = KSPROPERTY_TYPE_GET;
    unsigned __int64 big[4] = { 0, 0, 0, 0 };
    if (outLen > sizeof(big)) outLen = (DWORD)sizeof(big);
    DWORD got = 0;
    r.ok = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &p, sizeof(p),
                           big, outLen, &got, NULL) ? true : false;
    if (!r.ok) r.err = GetLastError();
    r.got = got;
    for (int i = 0; i < 4; i++) r.v[i] = big[i];
    return r;
}

static bool FindStreamingCapturePin(HANDLE f, ULONG* pinId) {
    ULONG ctypes = 0; DWORD got = 0;
    if (!KsPinGet(f, 0, KSPROPERTY_PIN_CTYPES, &ctypes, sizeof(ctypes), &got))
        return false;
    for (ULONG id = 0; id < ctypes; id++) {
        ULONG df = 0, comm = 0; DWORD g1 = 0, g2 = 0, g3 = 0;
        GUID cat;
        bool okDf  = KsPinGet(f, id, KSPROPERTY_PIN_DATAFLOW, &df, sizeof(df), &g1);
        bool okCm  = KsPinGet(f, id, KSPROPERTY_PIN_COMMUNICATION, &comm, sizeof(comm), &g2);
        bool okCat = KsPinGet(f, id, KSPROPERTY_PIN_CATEGORY, &cat, sizeof(cat), &g3);
        // a capture streaming pin: data comes OUT of the filter, clients open
        // it (SINK or BOTH -- Bluetooth HFP uses BOTH), and it is tagged with
        // PIN_CATEGORY_CAPTURE ({FB6C4281-...}).
        if (okDf && okCm && okCat && df == KSPIN_DATAFLOW_OUT &&
            cat.Data1 == 0xFB6C4281 &&
            (comm == KSPIN_COMMUNICATION_SINK ||
             comm == KSPIN_COMMUNICATION_BOTH)) {
            *pinId = id;
            return true;
        }
    }
    return false;
}

static HANDLE CreateCapturePinAny(HANDLE f, ULONG pinId, int* usedFmt) {
    typedef LONG (WINAPI *PFN_KsCreatePin)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK,
                                           PHANDLE);
    PFN_KsCreatePin pKsCreatePin = NULL;
    HMODULE ksuser = GetModuleHandleW(L"ksuser.dll");
    if (!ksuser) ksuser = LoadLibraryW(L"ksuser.dll");
    if (ksuser) pKsCreatePin = (PFN_KsCreatePin)GetProcAddress(ksuser, "KsCreatePin");
    if (!pKsCreatePin) return NULL;
    static const struct { ULONG ch, rate; } fmts[4] = {
        { 1, 48000 }, { 2, 48000 }, { 1, 44100 }, { 2, 44100 }
    };
    for (int i = 0; i < 4; i++) {
        BYTE buf[sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX)];
        ZeroMemory(buf, sizeof(buf));
        KSPIN_CONNECT* pc = (KSPIN_CONNECT*)buf;
        pc->Interface.Set = KSINTERFACESETID_Standard;
        pc->Interface.Id  = 1;              // LOOPED_STREAMING
        pc->Medium.Set    = KSMEDIUMSETID_Standard;
        pc->Medium.Id     = KSMEDIUM_STANDARD_DEVIO;
        pc->PinId         = pinId;
        pc->Priority.PriorityClass    = KSPRIORITY_NORMAL;
        pc->Priority.PrioritySubClass = 1;
        KSDATAFORMAT_WAVEFORMATEX* wf =
            (KSDATAFORMAT_WAVEFORMATEX*)(buf + sizeof(KSPIN_CONNECT));
        wf->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        wf->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        wf->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
        wf->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        wf->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
        wf->WaveFormatEx.nChannels       = (WORD)fmts[i].ch;
        wf->WaveFormatEx.nSamplesPerSec  = fmts[i].rate;
        wf->WaveFormatEx.wBitsPerSample  = 16;
        wf->WaveFormatEx.nBlockAlign     = (WORD)(fmts[i].ch * 2);
        wf->WaveFormatEx.nAvgBytesPerSec = fmts[i].rate * fmts[i].ch * 2;
        HANDLE ph = NULL;
        if (pKsCreatePin(f, pc, GENERIC_READ | GENERIC_WRITE, &ph) == 0) {
            *usedFmt = i;
            return ph;
        }
    }
    return NULL;
}

static void BatteryOne(const WCHAR* path, const char* tag) {
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        say("    [%s] filter open failed (err=%u)", tag, GetLastError());
        return;
    }
    ULONG pinId = 0;
    if (!FindStreamingCapturePin(f, &pinId)) {
        say("    [%s] no OUT/SINK streaming pin", tag);
        CloseHandle(f);
        return;
    }
    int used = -1;
    HANDLE ph = CreateCapturePinAny(f, pinId, &used);
    if (!ph) {
        say("    [%s] pin create failed on all 4 formats", tag);
        CloseHandle(f);
        return;
    }
    static const ULONG fmtCh[4]  = { 1, 2, 1, 2 };
    static const ULONG fmtRate[4] = { 48000, 48000, 44100, 44100 };
    say("    [%s] pin%u created (%uch %uHz), battery:", tag, pinId,
        fmtCh[used], fmtRate[used]);

    struct { KSPROPERTY p; ULONG st; } sb;
    sb.p.Set = KSPROPSETID_Connection;
    sb.p.Id  = KSPROPERTY_CONNECTION_STATE;
    sb.p.Flags = KSPROPERTY_TYPE_SET;
    DWORD got = 0; ULONG dummy = 0;
    sb.st = 2;   // PAUSE
    DeviceIoControl(ph, IOCTL_KS_PROPERTY, &sb, sizeof(sb),
                    &dummy, sizeof(dummy), &got, NULL);

    PropResult r;
    r = PinProp(ph, KSPROPSETID_Audio, KSPROPERTY_AUDIO_LATENCY, 32);
    say("      LATENCY    : %s got=%u v=%llu/%llu (err=%u)",
        r.ok ? "OK" : "FAIL", r.got, r.v[0], r.v[1], r.err);
    r = PinProp(ph, KSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION, 16);
    say("      POSITION   : %s play=%llu write=%llu (err=%u)",
        r.ok ? "OK" : "FAIL", r.v[0], r.v[1], r.err);
    r = PinProp(ph, KSPROPSETID_Audio, KSPROPERTY_AUDIO_CHANNEL_CONFIG, 4);
    say("      CHANCFG    : %s mask=0x%llX (err=%u)",
        r.ok ? "OK" : "FAIL", r.ok ? r.v[0] : 0, r.err);
    r = PinProp(ph, KSPROPSETID_Connection,
                KSPROPERTY_CONNECTION_ALLOCATORFRAMING, 64);
    say("      ALLOCFRM   : %s got=%u v0=%llu v1=%llu (err=%u)",
        r.ok ? "OK" : "FAIL", r.got, r.v[0], r.v[1], r.err);

    // The engine reads position right after RUN, BEFORE its own buffers are
    // queued.  A hardware driver's position advances freely (DMA register);
    // ours can only move through the port's copy path.  Measure both.
    KSPROPERTY gp; ZeroMemory(&gp, sizeof(gp));
    gp.Set = KSPROPSETID_Audio; gp.Id = KSPROPERTY_AUDIO_POSITION;
    gp.Flags = KSPROPERTY_TYPE_GET;
    unsigned __int64 p0[2] = { 0, 0 }, p1[2] = { 0, 0 };
    BOOL o0 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gp, sizeof(gp),
                              p0, sizeof(p0), &got, NULL);
    Sleep(300);
    BOOL o1 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gp, sizeof(gp),
                              p1, sizeof(p1), &got, NULL);
    say("      pos@PAUSE(no IRP): %s p0=%llu p1=%llu advanced=%s",
        (o0 && o1) ? "OK" : "FAIL", o0 ? p0[0] : 0, o1 ? p1[0] : 0,
        (o0 && o1 && p1[0] != p0[0]) ? "YES" : "no");

    sb.st = 3;   // RUN, still nothing queued
    DeviceIoControl(ph, IOCTL_KS_PROPERTY, &sb, sizeof(sb),
                    &dummy, sizeof(dummy), &got, NULL);
    o0 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gp, sizeof(gp),
                         p0, sizeof(p0), &got, NULL);
    Sleep(500);
    o1 = DeviceIoControl(ph, IOCTL_KS_PROPERTY, &gp, sizeof(gp),
                         p1, sizeof(p1), &got, NULL);
    say("      pos@RUN (no IRP): %s p0=%llu p1=%llu advanced=%s",
        (o0 && o1) ? "OK" : "FAIL", o0 ? p0[0] : 0, o1 ? p1[0] : 0,
        (o0 && o1 && p1[0] != p0[0]) ? "YES" : "no");

    sb.st = 0;   // STOP
    DeviceIoControl(ph, IOCTL_KS_PROPERTY, &sb, sizeof(sb),
                    &dummy, sizeof(dummy), &got, NULL);
    Sleep(100);
    CloseHandle(ph);
    CloseHandle(f);
}

static void DiffProbe(void) {
    say("[2g] differential probe vs a working capture filter");
    HDEVINFO set = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        say("    SetupDi failed (err=%u)", GetLastError());
        return;
    }
    WCHAR ourPath[1024] = L"";
    static WCHAR refList[8][1024];
    int refCount = 0;
    for (DWORD i = 0; i < 96; i++) {
        SP_DEVICE_INTERFACE_DATA di;
        di.cbSize = sizeof(di);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &KSCATEGORY_AUDIO, i, &di))
            break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &di, NULL, 0, &need, NULL);
        if (!need) continue;
        BYTE* b = (BYTE*)malloc(need);
        if (!b) break;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* dd =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)b;
        dd->cbSize = sizeof(*dd);
        if (SetupDiGetDeviceInterfaceDetailW(set, &di, dd, need, NULL, NULL)) {
            size_t len = wcslen(dd->DevicePath);
            if (len > 5 && _wcsicmp(dd->DevicePath + len - 5, L"\\wave") == 0) {
                if (wcsstr(dd->DevicePath, L"isightmic")) {
                    if (!ourPath[0])
                        wcsncpy_s(ourPath, dd->DevicePath, _TRUNCATE);
                } else {
                    // keep a list of candidates; the first one that really has
                    // a capture (OUT/SINK) streaming pin becomes the reference
                    // (many wave filters belong to RENDER devices, whose
                    // streaming pin is DATAFLOW_IN -- e.g. the speakers).
                    if (refCount < 8)
                        wcsncpy_s(refList[refCount++], dd->DevicePath,
                                  _TRUNCATE);
                }
            }
        }
        free(b);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!ourPath[0] || !refCount) {
        say("    need ours + a capture reference \\wave filter (ours=%d refs=%d)",
            ourPath[0] ? 1 : 0, refCount);
        return;
    }
    // pick the first candidate that really owns a capture streaming pin
    const WCHAR* refPath = NULL;
    for (int i = 0; i < refCount && !refPath; i++) {
        HANDLE f = CreateFileW(refList[i], GENERIC_READ | GENERIC_WRITE, 0,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        ULONG ctypes = 0; DWORD g0 = 0;
        char ansi[1024];
        WideCharToMultiByte(CP_ACP, 0, refList[i], -1, ansi, sizeof(ansi),
                            NULL, NULL);
        if (!KsPinGet(f, 0, KSPROPERTY_PIN_CTYPES, &ctypes, sizeof(ctypes), &g0)) {
            say("    cand %d: %s -- CTYPES failed (err=%u)", i, ansi,
                GetLastError());
        } else {
            say("    cand %d: %s -- %u pins:", i, ansi, ctypes);
            for (ULONG id = 0; id < ctypes && id < 8; id++) {
                ULONG df = 0, comm = 0, catGot = 0;
                DWORD g1 = 0, g2 = 0;
                bool okDf = KsPinGet(f, id, KSPROPERTY_PIN_DATAFLOW, &df,
                                     sizeof(df), &g1);
                bool okCm = KsPinGet(f, id, KSPROPERTY_PIN_COMMUNICATION,
                                     &comm, sizeof(comm), &g2);
                GUID cat; bool okCat = KsPinGet(f, id, KSPROPERTY_PIN_CATEGORY,
                                                &cat, sizeof(cat), &catGot);
                char catS[64] = "?";
                if (okCat) {
                    if (cat.Data1 == 0xFB6C4281) strcpy_s(catS, "CAPTURE");
                    else if (cat.Data1 == 0x6994AD04) strcpy_s(catS, "AUDIO");
                    else strcpy_s(catS, "other");
                }
                say("      pin %u: df=%s comm=%s cat=%s", id,
                    okDf ? FlowName(df) : "ERR",
                    okCm ? CommName(comm) : "ERR", catS);
            }
            ULONG pid = 0;
            if (!refPath && FindStreamingCapturePin(f, &pid)) {
                refPath = refList[i];
                say("      -> capture streaming pin = pin %u, using as REF", pid);
            }
        }
        CloseHandle(f);
    }
    if (!refPath) {
        say("    none of the %d reference candidates has a capture pin", refCount);
        return;
    }
    BatteryOne(refPath, "REF");
    BatteryOne(ourPath, "OURS");
    say("    -> any row where REF and OURS disagree is a suspect.");
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
    DiffProbe();

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
