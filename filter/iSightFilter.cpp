// v6 changes  (the version that finally streams inside real hosts)
//   * Active() no longer initialises the camera. CMU's InitCamera walks the
//     whole capability table (~15 s here) and Active() runs inside the
//     host's Pause()/Run() call, so applications timed out with "device
//     detected but cannot open". Bring-up now happens on the streaming
//     thread; black frames are delivered until it completes.
//   * ConfigureVideo() no longer takes the first mode the driver reports.
//     It used to select Mode 1 (320x240) while advertising 640x480, which
//     made every AcquireImageEx return -16 (CAM_ERROR_FRAME_TIMEOUT): no
//     picture ever appeared anywhere. It now verifies the frame size of
//     each candidate and only accepts 640x480.
//   * StartImageAcquisitionEx(8, 3000) instead of the 6/1000 ms default:
//     the first frame legitimately takes 1-2 s after the stream starts.
//   * the filter implements IAMFilterMiscFlags (IS_SOURCE) -- a capture
//     filter that does not answer this query gets treated as a file reader.
//   * every QueryInterface on the filter and on the pin is logged, so the
//     log shows exactly which interface a host wanted when it refused to
//     open the device.
//=====================================================================
// iSightFilter.cpp
//
// DirectShow Video Capture Source for the Apple FireWire iSight (2003).
// Wraps the CMU 1394 Digital Camera Driver user library (C1394Camera,
// LGPL 2.1, (c) CMU Robotics Institute) which talks to the signed
// 1394Camera.sys kernel driver. Both library and kernel driver are
// statically/vendor-included; nothing here touches the kernel.
//
// Registers itself under CLSID_VideoInputDeviceCategory, so OBS,
// Zoom, WeChat, Telegram etc. see it as a normal webcam named
// "Apple iSight (FireWire)".
//
// Native format: Format 0 / Mode 2 (640x480 YUV 4:2:2) converted by the
// CMU library's getDIB() to bottom-up BGR.  Off the pin we offer RGB24
// (native), YUY2 and RGB32 (converted) because many applications --
// WeChat / QQ among them -- insist on YUY2.
//
// v5 additions
//   * file log at %LOCALAPPDATA%\iSightCam.log (host process name, every
//     media-type request, camera bring-up result, capture errors). This is
//     the only channel that survives inside a host we do not control.
//   * media type negotiation relaxed: three subtypes, AvgTimePerFrame is
//     always accepted (the camera physically delivers 15 fps on S100),
//     SetFormat(NULL) resets to default instead of failing.
//   * the CMU device handle is fully released on Inactive() so the same
//     camera can be opened again and again by the host application.
//
// v7 additions -- the two reasons "the device shows up but the video never
// starts" in WeChat / QQ / OBS:
//   * the output pin now implements IKsPropertySet
//     (AMPROPSETID_Pin / AMPROPERTY_PIN_CATEGORY -> PIN_CATEGORY_CAPTURE).
//     ICaptureGraphBuilder2::FindPin() asks for exactly that before it will
//     render a source, and returns E_INVALIDARG when no pin answers -- which
//     is what the mainstream hosts do first, long before any media type is
//     negotiated.
//   * the camera bring-up no longer walks the whole feature/control register
//     table: on this driver that walk is ~14 s of synchronous IOCTLs, far
//     longer than a host is willing to wait for a first frame
//     (g_bISightFastBringUp in the CMU library).
//
// v8 additions
//   * YUY2 rows are written top-down and biHeight is declared negative, so
//     both kinds of consumer (those that honour the sign and WeChat, which
//     ignores it) end up with an upright picture.
//   * %LOCALAPPDATA%\iSightCam.ini: orientation and debug options that can
//     be changed without rebuilding the filter.
//
// v9 additions -- recovering from a bus reset (the camera was switched off
// and on again while a host was streaming):
//   * a bus monitor thread watches IOCTL_GET_GENERATION_COUNT on the camera
//     device and also notices when the device disappears from the bus, so a
//     power cycle is seen within ~200 ms instead of after 12 acquire
//     timeouts (24 s+). The generation counter is the authoritative signal:
//     every plug/unplug resets the bus and bumps it.
//   * on such an event the CMU handle is dropped and a *fresh* C1394Camera
//     is created after a short delay (the camera needs ~2 s to boot), then
//     re-enumerated, re-configured and restarted. The stale handle is the
//     trap: after a bus reset the node address changes and the old one
//     never produces another frame.
//   * bring-up retries now run on a 1 s cadence (they used to be 10 s
//     apart) and the acquire-failure threshold dropped from 12 to 3, so a
//     host that is merely streaming (no bus monitor available) still
//     recovers in a few seconds.
//   * every per-QI log line is throttled and the log file is kept open.
//     QQ asks for IKsPropertySet ~11 000 times during one enumeration and a
//     line + fopen/fclose per probe wrote 3.8 MB in three minutes, which
//     both hid the real events and slowed the host down.
//
// v10 additions -- all of it about *what shape the host ends up showing*, and
// all of it changeable at runtime through iSightCam.ini, because a host's
// display behaviour cannot be tested from inside the filter:
//   * [layout] target=WxH -- some hosts do not letterbox a 4:3 camera into
//     their own window: they cut the picture down to their frame shape.  The
//     image arrives complete, upright and 640x480 (verified in the log) and
//     is then cropped/magnified, which is why a WeChat call shows a giant
//     head while QQ, which fits the whole frame, looks normal.  With
//     target=480x480 (or 360x480, 270x480 ...) the camera picture is scaled
//     down into that rectangle in the middle of the frame, the rest is black,
//     and the host's crop lands on our rectangle: the whole scene is visible
//     again at its normal size.  target=0 (the default) sends the frame
//     untouched.  The cost is resolution -- whatever box is chosen is
//     magnified back up by the host.
//   * [format] types=rgb24,yuy2,rgb32 -- which subtypes the pin offers and in
//     what order.  This moves a host onto a different subtype without a
//     rebuild, which is how we test whether a host's picture depends on the
//     subtype it negotiated (QQ takes RGB32, WeChat takes RGB24).
//   * [orientation] yuy2=4 -- new mode: rows are written top-down but the
//     height is declared *positive*.  Hosts that ignore the sign (WeChat)
//     then get an upright picture without the negative height that makes it
//     skip the YUY2 type altogether (v7 behaved differently from v8+ for
//     exactly that reason).
//   * rcSource / rcTarget are filled in with the real frame rectangle.  They
//     used to be left zeroed, which is a deviation from every sample source
//     and could make a host compute its own scaling from an empty rect.
//   * the IAMStreamConfig enumeration (GetStreamCaps / GetFormat /
//     GetNumberOfCapabilities) and the negotiated sample size are logged, so
//     the log shows which capability a host picked and how big its buffers
//     really are.
//
// v11 additions -- making the [layout] box usable without guessing:
//   * [layout] hosts=...  The box is applied *only* inside the named
//     processes (default "Weixin.exe,WeChat.exe"), so QQ and every other
//     host keep the untouched full frame.  An empty list applies it
//     everywhere.
//   * [layout] guide=1 draws a marker into the frame itself: a yellow
//     border around the full 640x480 frame, a red border exactly on the
//     layout box, and a white cross at its centre.  Whats visible in the
//     host's window then tells us exactly how much the host crops, and the
//     box can be matched to it (or the screenshot read back offline).
//     The markers are symmetric, so a vertical flip does not move them.
//   * orientation / layout / guide (and dump) are re-read from the ini
//     while the stream is running, at most twice a second.  Editing the
//     file reshapes the picture inside a second, so the box can be tuned
//     live with the call window open instead of restarting WeChat per
//     attempt.  [format] types= and [recovery] still need a fresh graph,
//     because they are answered during connection setup.
//
// v12 additions -- filling the box, and measuring the crop exactly:
//   * the v11 measurement is in: WeChat's call window keeps the *full*
//     height of the frame and cuts the middle out horizontally, so the
//     320x480 box does land where it should -- but a 4:3 picture fitted
//     into a 2:3 box only uses half the box height, so half of what the
//     call shows was black (the v11 report: "上下边有黑色区域了").
//   * [layout] mode=  now picks how the box is filled:
//       fit     whole scene centred in the box, black around it (v11)
//       blur    whole scene on top of a blurred, dimmed copy of the same
//               frame -- edge to edge, nothing cropped, nothing distorted.
//               This is the default now.
//       fill    scene cropped to the box shape and magnified to fill it --
//               no black, but the sides are gone and the face is magnified
//               (exactly what the host does on its own)
//       stretch whole scene squeezed into the box shape -- no black, no
//               crop, but distorted
//   * [layout] blur=/dim= tune the backdrop (block size in source pixels,
//     and how much of its brightness survives).
//   * [layout] guide= is now a level: 1 = the coloured boxes (plus a green
//     box on the target rectangle, red on the part the picture fills),
//     2 = a ruler drawn into the frame (thin lines every 40 px, colour
//     coded landmarks at 160/320/480), 3 = both.  The ruler is what makes
//     the host's crop measurable to the pixel from a screenshot.
//
// v13 additions -- "after the TI card was swapped, nothing comes up at all":
//   * probes, because this failure is invisible from the outside.  The
//     standalone CMU tool (isight-diag.exe) pulls 90 frames off the camera at
//     the very same moment the filter's stream starts and never delivers one,
//     and nothing in the log says where the streaming thread stopped.  So:
//       - every FillBuffer entry is counted, and the count alongside the
//         camera state is logged every 2 s by the bus-monitor thread.  A
//         count that stops growing means DirectShow no longer calls us; a
//         count that keeps growing means we are stuck inside the driver.
//       - every AcquireImageEx is timed and its result logged, so a call
//         that blocks (instead of returning CAM_ERROR_FRAME_TIMEOUT after
//         2 s) is visible as such.
//   * self-heal: if the stream is marked running and no frame has arrived
//     four seconds later, the stream is invalidated and brought up again
//     (fresh handle, fresh node address) instead of waiting for a timeout
//     that never comes.  See the [recovery] notes -- this is the same path a
//     bus reset takes, so it cannot make a working camera worse.
//
// v14 additions -- the probe answered, and it was not the camera:
//   * the v13 probe walked straight into the real failure: with the stock
//     NullRenderer the filter's FillBuffer entry count freezes at 1, zero
//     frames are delivered, and the renderer reports EC_COMPLETE while the
//     graph is still Running.  Reading the base classes (Win7Samples
//     baseclasses/source.cpp) explains it: CSourceStream::DoBufferProcessing
//     Loop() returns -- and the worker thread parks on its request queue for
//     good -- as soon as Deliver() returns anything other than S_OK.  The
//     camera is then never asked for another frame.  The host sees a
//     connected camera that never produces a picture, with no error, and the
//     only cure is for the host to throw the graph away and build it again.
//     That is exactly what QQ.exe left in the log on the previous build
//     (connected twice, 0 frames).
//   * so we take the loop over: a rejected delivery is retried (up to ~1.2 s)
//     instead of being fatal, GetDeliveryBuffer failures are logged, and the
//     loop's entry/exit is logged, which separates "the host stopped calling
//     us" from "we are stuck in the driver" without guesswork.
//   * the bus-monitor heartbeat now also reports loopRuns and shouts
//     "STREAM PARKED" when the FillBuffer counter stops moving, so a dead
//     graph is identifiable from a log alone.
//=====================================================================

#include <windows.h>
#include <winioctl.h>       // CTL_CODE for the 1394 generation-count IOCTL
#include <streams.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The CMU library objects are compiled INTO this DLL, so treat the
// class as dllexport for this build (same define used by cmu objects).
#ifndef MY1394CAMERA_EXPORTS
#define MY1394CAMERA_EXPORTS
#endif

#include "1394camapi.h"
#include "debug.h"
#include "1394Camera.h"

// {73912CE1-84DD-4BF4-8693-FF4603D7369F}
static const GUID CLSID_ISightFireWireCam =
{ 0x73912ce1, 0x84dd, 0x4bf4, { 0x86, 0x93, 0xff, 0x46, 0x03, 0xd7, 0x36, 0x9f } };

// Identifies the build inside the binary.  install-all.bat greps for this
// string to prove that the file it just registered is really this version --
// a silently failed copy (the .ax is mapped by a running host and the copy
// is refused) has burned this project more than once.
#define ISIGHT_BUILD_TAG "ISIGHTFILTER-BUILD-V17-20260922-STRIP"

// Public release number.  The build tag above changes on every internal
// iteration (and install-all.bat greps for its "V14" prefix); this one is
// what a user sees and what the GitHub release is named after.
#define ISIGHT_RELEASE   "1.0.0"

//---------------------------------------------------------------------
// AMPROPSETID_Pin -- the pin category property set.
//
// ICaptureGraphBuilder2::FindPin() asks a pin for its category through
// IKsPropertySet(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY) before it will
// render it.  RenderStream(PIN_CATEGORY_CAPTURE, ...) returns E_INVALIDARG
// when no pin answers that query -- which is exactly how "the device shows
// up in the list but the video never starts" presents itself in WeChat, QQ
// and OBS.  The base classes do not implement this property set (CSource /
// CSourceStream are just CBaseFilter / CBaseOutputPin), so the pin has to
// do it itself.
//
// The registry-side FilterData is not enough: hosts query the running pin.
//---------------------------------------------------------------------
static const GUID kAMPROPSETID_Pin =
{ 0x9b00f101, 0x1567, 0x11d1, { 0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5 } };

#define AMPROPERTY_PIN_CATEGORY_LOCAL 0     // AM_PROPERTY_PIN_CATEGORY
#define AMPROPERTY_PIN_MEDIUM_LOCAL   1     // AM_PROPERTY_PIN_MEDIUM

// ks.h / strmif.h do not always bring these in with a plain <streams.h>
// build, so make sure they exist.
#ifndef KSPROPERTY_SUPPORT_GET
#define KSPROPERTY_SUPPORT_GET     0x00000001
#endif
#ifndef E_PROP_ID_UNSUPPORTED
#define E_PROP_ID_UNSUPPORTED      ((HRESULT)0x80070490L)
#endif
#ifndef E_PROP_SET_UNSUPPORTED
#define E_PROP_SET_UNSUPPORTED     ((HRESULT)0x80070492L)
#endif


// normally defined in the base-classes dllentry.cpp, which we do not link;
// still referenced by dllsetup.obj (AMovieDllRegisterServer).
// Recovered at registration time via GetModuleHandleExW (see DllRegisterServer) —
// if left NULL, dllsetup stores the host executable path instead of this DLL's.
HINSTANCE g_hInst = NULL;

static const WCHAR g_wszFilterName[] = L"Apple iSight (FireWire)";

// DirectShow reference clock = 10,000,000 units/sec
static const REFERENCE_TIME kFrameDurations[] =
{
    5333333,   // 0 :  1.875 fps
    2666667,   // 1 :  3.75  fps
    1333333,   // 2 :  7.5   fps
     666667,   // 3 : 15     fps
     333667,   // 4 : 30     fps
     166667    // 5 : 60     fps
};
static const int kMaxRateIndex = 3;          // never ask the camera for more than 15 fps

#define ISIGHT_WIDTH  640
#define ISIGHT_HEIGHT 480
#define ISIGHT_BPP     24
#define ISIGHT_DIB_BYTES  (ISIGHT_WIDTH * ISIGHT_HEIGHT * 3)
// v12: room for the tiny copy of the frame the blur backdrop is built from
// (a 4 px block over 640x480 is the finest setting we allow: 160x120x6 BGR)
#define kBlurScratchBytes (96 * 1024)

//---------------------------------------------------------------------
// file log — the filter lives inside whatever process opens the camera,
// so console/debug output is unreachable; a plain file survives.
//---------------------------------------------------------------------
static const char *HostExeName()
{
    static char s_name[MAX_PATH] = "";
    if (s_name[0] == 0)
    {
        char full[MAX_PATH] = "";
        GetModuleFileNameA(NULL, full, MAX_PATH);
        const char *p = strrchr(full, '\\');
        strncpy_s(s_name, sizeof(s_name), p ? p + 1 : full, _TRUNCATE);
    }
    return s_name;
}

static const char *LogPath()
{
    static char s_path[MAX_PATH] = "";
    if (s_path[0] == 0)
    {
        char dir[MAX_PATH] = "";
        if (GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH) == 0)
            GetTempPathA(MAX_PATH, dir);
        _snprintf_s(s_path, sizeof(s_path), _TRUNCATE, "%s\\iSightCam.log", dir);
    }
    return s_path;
}

static void BuildLogLine(char *out, size_t cb, const char *msg)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(out, cb, _TRUNCATE, "[%02d:%02d:%02d.%03d pid=%lu %s] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentProcessId(), HostExeName(), msg);
}

// one critical section for the log and for the throttle table; the file
// handle is opened once and kept open (fopen/fclose per line was a real
// cost in the hot path -- see the v9 notes at the top of this file).
static CRITICAL_SECTION &LogLock()
{
    static CRITICAL_SECTION s_cs;
    static LONG s_once = 0;
    if (InterlockedCompareExchange(&s_once, 1, 0) == 0)
        InitializeCriticalSection(&s_cs);
    return s_cs;
}

static void LogWrite(const char *msg)
{
    char line[1200];
    BuildLogLine(line, sizeof(line), msg);

    EnterCriticalSection(&LogLock());
    static FILE *s_f = NULL;
    static LONG s_tried = 0;
    if (s_f == NULL && InterlockedCompareExchange(&s_tried, 1, 0) == 0)
        s_f = fopen(LogPath(), "a");
    if (s_f)
    {
        fputs(line, s_f);
        fflush(s_f);
    }
    LeaveCriticalSection(&LogLock());
}

void FLog(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    LogWrite(msg);
}

// Throttled log: the first few occurrences of a "key" are written, then
// only every 200th. Hosts probe interfaces in tight loops (see v9 notes),
// and a per-probe line buries the events that actually matter.
void FLogT(const char *key, const char *fmt, ...)
{
    static struct { char key[48]; LONG n; } s_tab[128];
    static int s_used = 0;
    LONG n = 1;

    EnterCriticalSection(&LogLock());
    for (int i = 0; i < s_used; ++i)
    {
        if (strcmp(s_tab[i].key, key) == 0)
        {
            n = InterlockedIncrement(&s_tab[i].n);
            break;
        }
    }
    if (n == 1 && s_used < (int)(sizeof(s_tab) / sizeof(s_tab[0])))
    {
        strncpy_s(s_tab[s_used].key, key, _TRUNCATE);
        s_tab[s_used].n = 1;
        ++s_used;
    }
    LeaveCriticalSection(&LogLock());

    if (n > 3 && (n % 200) != 0)
        return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 3)
    {
        size_t len = strlen(msg);
        _snprintf_s(msg + len, sizeof(msg) - len, _TRUNCATE, "  [x%ld]", n);
    }
    LogWrite(msg);
}

static DWORD TickMs() { return (DWORD)GetTickCount64(); }

// name the interfaces hosts ask for; __uuidof() keeps us free of any
// dependency on which IID symbols a given SDK/lib happens to export.
static const char *GuidName(REFIID g)
{
    if (g == __uuidof(IUnknown))              return "IUnknown";
    if (g == __uuidof(IClassFactory))         return "IClassFactory";
    if (g == __uuidof(IBaseFilter))           return "IBaseFilter";
    if (g == __uuidof(IMediaFilter))          return "IMediaFilter";
    if (g == __uuidof(IPersist))              return "IPersist";
    if (g == __uuidof(IPersistStream))        return "IPersistStream";
    if (g == __uuidof(IAMStreamConfig))       return "IAMStreamConfig";
    if (g == __uuidof(IAMFilterMiscFlags))    return "IAMFilterMiscFlags";
    if (g == __uuidof(ISpecifyPropertyPages)) return "ISpecifyPropertyPages";
    if (g == __uuidof(IKsPropertySet))        return "IKsPropertySet";
    if (g == __uuidof(IAMBufferNegotiation))  return "IAMBufferNegotiation";
    if (g == __uuidof(IAMVideoProcAmp))       return "IAMVideoProcAmp";
    if (g == __uuidof(IAMCameraControl))      return "IAMCameraControl";
    if (g == __uuidof(IReferenceClock))       return "IReferenceClock";
    if (g == __uuidof(IQualityControl))       return "IQualityControl";
    if (g == __uuidof(IPin))                  return "IPin";
    if (g == __uuidof(IMemInputPin))          return "IMemInputPin";
    return "?";
}

//---------------------------------------------------------------------
// orientation + debug options  (%LOCALAPPDATA%\iSightCam.ini)
//
// getDIB() hands us a *bottom-up* BGR DIB (row 0 = bottom scanline) and
// biHeight = +480 says exactly that.  RGB consumers (QQ, the DirectShow
// renderers) honour the sign and show the picture upright -- but WeChat
// feeds a YUY2 buffer straight into its own converter as if it were
// top-down, no matter what the media type declares, so it shows the same
// frame upside down.
//
// yuy2=vflip fixes that the robust way: the YUY2 buffer is written
// top-down and biHeight is declared *negative*.  A host that honours the
// sign renders correctly, and a host that ignores it also gets it right,
// because the data is already top-down.
//
// Everything is overridable at runtime -- no rebuild needed to try a
// different orientation, which matters because we cannot test the host's
// expectation from here:
//
//   [orientation]
//   yuy2=1      0 none | 1 vertical flip | 2 horizontal flip | 3 rotate 180
//   rgb=0       same values
//   [debug]
//   dump=0      1 = write the first two frames of each process to
//               %LOCALAPPDATA%\iSightCam-dump\*.raw for inspection
//
// Edit the ini and restart the host application to apply.
//---------------------------------------------------------------------
enum OrientMode
{
    ORIENT_NONE   = 0,
    ORIENT_VFLIP  = 1,
    ORIENT_HFLIP  = 2,
    ORIENT_ROT180 = 3,
    // v10: rows top-down like ORIENT_VFLIP, but the height is deliberately
    // declared *positive* (bottom-up).  For hosts that always read the buffer
    // top-down no matter what the media type says -- WeChat does that -- this
    // is the only way to be upright through the YUY2 path, because the
    // consistent mode (ORIENT_VFLIP) declares -480 and WeChat then skips the
    // whole subtype.  A host that *does* honour the sign will show it
    // mirrored vertically, which is exactly why this is opt-in.
    ORIENT_VFLIP_POS = 4
};

static const char *OrientName(int m)
{
    switch (m)
    {
    case ORIENT_VFLIP:     return "vflip";
    case ORIENT_HFLIP:     return "hflip";
    case ORIENT_ROT180:    return "rot180";
    case ORIENT_VFLIP_POS: return "vflip+";     // data flipped, sign left positive
    default:               return "none";
    }
}

// Are the rows written top-down?
static bool OrientDataTopDown(int m)
{
    return m == ORIENT_VFLIP || m == ORIENT_ROT180 || m == ORIENT_VFLIP_POS;
}
// Is biHeight declared negative?  Only for the self-consistent modes: the
// data order and the declared sign always have to agree for a host that
// honours the sign, and ORIENT_VFLIP_POS trades that away on purpose.
static bool OrientDeclareNegative(int m)
{
    return m == ORIENT_VFLIP || m == ORIENT_ROT180;
}
static bool OrientMirror (int m) { return m == ORIENT_HFLIP || m == ORIENT_ROT180; }

static const char *IniPath()
{
    static char s_path[MAX_PATH] = "";
    if (s_path[0] == 0)
    {
        char dir[MAX_PATH] = "";
        if (GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH) == 0)
            GetTempPathA(MAX_PATH, dir);
        _snprintf_s(s_path, sizeof(s_path), _TRUNCATE, "%s\\iSightCam.ini", dir);
    }
    return s_path;
}

static int ClampOrient(int v) { return (v < 0 || v > 4) ? ORIENT_NONE : v; }

struct ISightOptions
{
    int  orientYUY2;
    int  orientRGB;
    bool dump;
    int  bootDelayMs;   // wait after a bus reset before touching the camera
    bool busMon;        // watch the bus generation counter
    // v10 -- what the pin offers, and how the picture is laid out inside the
    // frame for hosts that cut it to their own shape
    int  layoutW, layoutH;          // [layout] target=WxH, 0 = untouched
    int  typeOrder[3];              // [format] types=...  (indices into kAllSubs)
    int  typeCount;
    // v11 -- who the layout box applies to, and a visible marker for
    // calibrating it (see the v11 notes at the top of this file)
    char hosts[160];                // [layout] hosts=..., empty = every host
    int  guide;                     // [layout] guide= 0 off / 1 boxes / 2 ruler / 3 both
    // v12 -- how the box is filled (see the v12 notes at the top)
    int  layoutMode;                // [layout] mode= fit | blur | fill | stretch
    int  blurCells;                 // [layout] blur=  backdrop block, source pixels
    int  blurShade;                 // [layout] dim=   backdrop brightness, 100 = off
    // v15 -- which processes this filter is willing to come up in at all.
    // Some hosts crash while merely enumerating a capture filter they cannot
    // drive (measured: QQ.exe died every time its audio settings page was
    // opened with this filter registered).  Refusing to instantiate keeps the
    // filter invisible to them without unregistering it, so the camera stays
    // available to the hosts that do work.
    char allow[256];                // [host] allow=...  empty = every host
    char deny[256];                 // [host] deny=...   never instantiates here
    // v16 -- the microphone.  The iSight's audio unit is a second unit on the
    // same 1394 node and it transmits on the SAME isochronous stream as the
    // video (measured: see docs/av-merge.md), so with the video engine already
    // running the only thing missing is telling the audio unit to start.
    // Nothing here opens a second stream -- it cannot, the CMU driver allows
    // one per device -- so the microphone costs no extra isochronous channel.
    int  audioOn;           // [audio] enable=   0 = video only
    int  audioGain;         // [audio] gain=     raw 1..43, 0 = camera default
    int  audioWav;          // [audio] wav=      1 = write the PCM to disk
    int  audioMaxMB;        // [audio] maxmb=    0 = no limit
    int  audioLog;          // [audio] log=      0/1/2, 2 = per-packet
    int  audioStrip;        // [audio] strip=    1 = cut audio packets out of
                            //     the frame buffer before the picture is
                            //     converted (fixes the flickering bands);
                            //     0 leaves the buffer untouched (v16 behaviour)
};

enum LayoutMode
{
    LAYOUT_FIT     = 0,     // whole scene, centred, black around it (v10/v11)
    LAYOUT_BLUR    = 1,     // whole scene over a blurred copy of itself (default)
    LAYOUT_FILL    = 2,     // scene cropped to the box shape and magnified
    LAYOUT_STRETCH = 3      // whole scene squeezed into the box shape
};

static const char *ModeName(int m)
{
    switch (m)
    {
    case LAYOUT_BLUR:    return "blur";
    case LAYOUT_FILL:    return "fill";
    case LAYOUT_STRETCH: return "stretch";
    default:             return "fit";
    }
}

static int ParseLayoutMode(const char *v)
{
    if (v == NULL || v[0] == '\0')
        return LAYOUT_BLUR;
    if (_stricmp(v, "fit") == 0 || _stricmp(v, "letterbox") == 0)
        return LAYOUT_FIT;
    if (_stricmp(v, "blur") == 0 || _stricmp(v, "soft") == 0)
        return LAYOUT_BLUR;
    if (_stricmp(v, "fill") == 0 || _stricmp(v, "crop") == 0 || _stricmp(v, "cover") == 0)
        return LAYOUT_FILL;
    if (_stricmp(v, "stretch") == 0 || _stricmp(v, "squash") == 0)
        return LAYOUT_STRETCH;
    const int n = atoi(v);
    return (n >= 0 && n <= 3) ? n : LAYOUT_BLUR;
}

// the three subtypes we can produce, in the order they used to be advertised
static const ISightOptions &Opts();          // defined below, used by SubTypeEnabled
// v11 helper the option reader itself uses (defined right after Opts).
// HostExeName() already exists above (the log prefix uses it).
static bool HostInList(const char *list);

static const GUID *const kAllSubs[3] =
{
    &MEDIASUBTYPE_RGB24, &MEDIASUBTYPE_YUY2, &MEDIASUBTYPE_RGB32
};

static int SubIndex(const GUID *sub)
{
    if (!sub) return -1;
    for (int i = 0; i < 3; ++i)
        if (*kAllSubs[i] == *sub) return i;
    return -1;
}

// the pin offers RGB24 / YUY2 for 24-bit consumers and RGB32 as well, because
// hosts differ: QQ enumerates all three and takes RGB32, WeChat takes RGB24.
// [format] types=... overrides the list (and the order) at runtime.
static bool SubTypeEnabled(const GUID *sub)
{
    int idx = SubIndex(sub);
    if (idx < 0) return false;
    const ISightOptions &o = Opts();
    for (int i = 0; i < o.typeCount; ++i)
        if (o.typeOrder[i] == idx) return true;
    return false;
}

// hand the user a commented file on first use, so changing the
// orientation is a two-second text edit instead of a rebuild
static void WriteDefaultIni(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return;
    // written in text mode, so plain \n turns into the CRLF Notepad wants
    fprintf(f,
        "; iSightCam DirectShow filter -- runtime options\n"
        "; Edit a value, then restart WeChat / QQ / the capture app.\n"
        ";\n"
        "; orientation: 0=none  1=vertical flip  2=horizontal flip  3=rotate 180\n"
        ";              4 = vflip+, like 1 but the height is declared positive.\n"
        "; The camera hands out a bottom-up image and RGB consumers honour that,\n"
        "; so rgb=0 is right for QQ. WeChat reads YUY2 as top-down, hence yuy2=1.\n"
        "[orientation]\n"
        "yuy2=1\n"
        "rgb=0\n"
        ";\n"
        "; debug: dump=1 writes the first two frames of every process to\n"
        "; <LOCALAPPDATA>\\iSightCam-dump\\frame-*.raw\n"
        "[debug]\n"
        "dump=0\n"
        ";\n"
        "; recovery: what to do when the camera is switched off while a call\n"
        "; is running.  busmon=1 watches the 1394 generation counter and\n"
        "; restarts the stream as soon as the camera is back; bootdelay is how\n"
        "; long the camera needs to boot before it answers again (ms).\n"
        "[recovery]\n"
        "busmon=1\n"
        "bootdelay=2000\n"
        ";\n"
        "; format: which subtypes the pin offers, and in which order.\n"
        ";[format]\n"
        ";types=rgb24,yuy2,rgb32\n"
        ";\n"
        "; layout: some hosts do not letterbox a 4:3 camera into their window,\n"
        "; they cut the picture down to their own shape, so the face fills the\n"
        "; whole window (WeChat's video call does this; QQ does not).  Set\n"
        "; target to the rectangle the host actually shows and the picture is\n"
        "; put inside that rectangle -- the host's crop then lands on it and\n"
        "; the whole scene is visible again.  0 = off (send the frame as it\n"
        "; comes).  WeChat's call window keeps the full height and cuts the\n"
        "; middle out horizontally, so 320x480 is the usual answer.\n"
        ";\n"
        "; mode decides how the rectangle is filled:\n"
        ";   blur    whole scene, with a blurred copy of it behind (default;\n"
        ";           nothing cropped, nothing distorted, no black bars)\n"
        ";   fit     whole scene, black where the scene does not reach\n"
        ";   fill    the scene cropped to the box shape and magnified to fill\n"
        ";           it -- no black bars, but the sides are gone\n"
        ";   stretch the whole scene squeezed into the box shape -- distorted\n"
        "; blur / dim tune the blurred backdrop: block size in pixels and how\n"
        "; much brightness is left (100 = none).\n"
        ";\n"
        "; hosts limits the box to those processes, so other hosts (QQ) keep\n"
        "; the full frame; empty = every host.\n"
        "; guide draws markers into the picture, for measuring what the host\n"
        "; really shows: 1 = coloured boxes and a centre cross, 2 = a ruler\n"
        "; (thin lines every 40 px, coloured landmarks at 160/320/480),\n"
        "; 3 = both.  Turn it off for normal calls.\n"
        "; orientation/layout/guide are re-read while a call is running.\n"
        ";[layout]\n"
        ";target=0\n"
        ";mode=blur\n"
        ";blur=16\n"
        ";dim=70\n"
        ";hosts=Weixin.exe,WeChat.exe\n"
        ";guide=0\n"
        ";\n"
        "; host: which processes this filter comes up in AT ALL.  Some hosts\n"
        "; crash while merely enumerating a capture filter they cannot drive --\n"
        "; QQ.exe died every time its audio settings page was opened while this\n"
        "; filter was registered, and unregistering it was the only cure.  So\n"
        "; instead of uninstalling the camera, refuse to instantiate it inside\n"
        "; those processes: they see the device, ask for it, get a plain\n"
        "; failure, and carry on.  No camera code runs in a denied process at\n"
        "; all -- no CMU library, no 1394 handle, no worker threads.\n"
        ";\n"
        ";   deny   never instantiate in these processes (checked first)\n"
        ";   allow  instantiate ONLY in these processes; empty = every host\n"
        ";\n"
        "; Process names, case-insensitive, comma separated.  Remove a name\n"
        "; (or set deny=) to let that host use the camera again.\n"
        "[host]\n"
        "deny=QQ.exe,TIM.exe,QQExternal.exe,QQProtect.exe\n"
        ";allow=Weixin.exe,WeChat.exe,obs64.exe\n"
        ";\n"
        "; audio: the microphone (v16).  iSight's mic is a second unit on the\n"
        "; same FireWire node and it rides the SAME isochronous stream as the\n"
        "; picture, so turning it on costs no extra channel: the filter tells\n"
        "; the audio unit to transmit and picks the audio packets out of the\n"
        "; capture buffer it is already reading.\n"
        ";\n"
        ";   enable  1 = transmit audio alongside the video (default)\n"
        ";   gain    raw 1..43 (about -30 dB .. +12 dB); 0 = camera default\n"
        ";   wav     1 = write 48 kHz / 16 bit stereo PCM to\n"
        ";           <LOCALAPPDATA>\\iSightAudio.wav  (for checking the mic)\n"
        ";   maxmb   stop the wav file at this size, 0 = no limit\n"
        ";   log     1 = a summary line every 100 packets, 2 = every packet\n"
        ";\n"
        "; If turning the mic on ever disturbs the picture, set enable=0 --\n"
        "; the video path then behaves exactly as it did before v16.\n"
        "[audio]\n"
        "enable=1\n"
        "gain=0\n"
        "wav=1\n"
        "maxmb=20\n"
        "log=1\n"
        "; v17: cut the audio packets out of the frame buffer again before\n"
        "; the picture is converted.  The 1394 driver splices them into the\n"
        "; image as if they were video bytes (flickering seams + a rainbow\n"
        "; band at the bottom).  strip=0 restores the v16 behaviour.\n"
        "strip=1\n");
    fclose(f);
}

static const ISightOptions &Opts()
{
    static ISightOptions s_o;
    static LONG s_once = 0;
    if (InterlockedCompareExchange(&s_once, 1, 0) == 0)
    {
        const char *ini = IniPath();
        if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES)
            WriteDefaultIni(ini);       // hand the user a file to edit

        s_o.orientYUY2 = ClampOrient(GetPrivateProfileIntA("orientation", "yuy2", ORIENT_VFLIP, ini));
        s_o.orientRGB  = ClampOrient(GetPrivateProfileIntA("orientation", "rgb",  ORIENT_NONE,   ini));
        s_o.dump       = GetPrivateProfileIntA("debug", "dump", 0, ini) != 0;
        s_o.bootDelayMs = GetPrivateProfileIntA("recovery", "bootdelay", 2000, ini);
        if (s_o.bootDelayMs < 0)     s_o.bootDelayMs = 0;
        if (s_o.bootDelayMs > 20000) s_o.bootDelayMs = 20000;
        s_o.busMon     = GetPrivateProfileIntA("recovery", "busmon", 1, ini) != 0;

        // [format] types=rgb24,yuy2,rgb32 -- what the pin offers, in order.
        // Anything not listed is neither advertised nor accepted, which is how
        // a host gets moved onto another subtype without a rebuild.
        char types[128] = "";
        GetPrivateProfileStringA("format", "types", "rgb24,yuy2,rgb32", types, sizeof(types), ini);
        s_o.typeCount = 0;
        for (char *p = strtok(types, ",; \t"); p != NULL && s_o.typeCount < 3; p = strtok(NULL, ",; \t"))
        {
            int idx = -1;
            if      (_stricmp(p, "rgb24") == 0) idx = 0;
            else if (_stricmp(p, "yuy2")  == 0) idx = 1;
            else if (_stricmp(p, "rgb32") == 0) idx = 2;
            if (idx < 0)
            {
                FLog("options: [format] types= has an unknown entry '%s'", p);
                continue;
            }
            bool dup = false;
            for (int i = 0; i < s_o.typeCount; ++i)
                if (s_o.typeOrder[i] == idx) dup = true;
            if (!dup) s_o.typeOrder[s_o.typeCount++] = idx;
        }
        if (s_o.typeCount == 0)
        {
            s_o.typeOrder[0] = 0; s_o.typeOrder[1] = 1; s_o.typeOrder[2] = 2;
            s_o.typeCount = 3;
        }

        // [layout] target=WxH -- the rectangle the host is expected to show.
        // 0 / empty / "full" leaves the frame alone.
        char lay[64] = "";
        s_o.layoutW = s_o.layoutH = 0;
        GetPrivateProfileStringA("layout", "target", "0", lay, sizeof(lay), ini);
        int lw = 0, lh = 0;
        if (sscanf_s(lay, "%dx%d", &lw, &lh) == 2 && lw > 0 && lh > 0)
        {
            if (lw < 64)  lw = 64;
            if (lw > ISIGHT_WIDTH)  lw = ISIGHT_WIDTH;
            if (lh < 64)  lh = 64;
            if (lh > ISIGHT_HEIGHT) lh = ISIGHT_HEIGHT;
            s_o.layoutW = lw;
            s_o.layoutH = lh;
        }

        // [layout] hosts=...  and  [layout] guide=N  (v11)
        GetPrivateProfileStringA("layout", "hosts", "", s_o.hosts, sizeof(s_o.hosts), ini);
        s_o.guide = GetPrivateProfileIntA("layout", "guide", 0, ini);
        if (s_o.guide < 0) s_o.guide = 0;
        if (s_o.guide > 3) s_o.guide = 3;

        // [layout] mode= / blur= / dim=  (v12)
        char md[32] = "";
        GetPrivateProfileStringA("layout", "mode", "blur", md, sizeof(md), ini);
        s_o.layoutMode = ParseLayoutMode(md);
        s_o.blurCells  = GetPrivateProfileIntA("layout", "blur", 16, ini);
        if (s_o.blurCells < 4)  s_o.blurCells = 4;
        if (s_o.blurCells > 64) s_o.blurCells = 64;
        s_o.blurShade  = GetPrivateProfileIntA("layout", "dim", 70, ini);
        if (s_o.blurShade < 10)  s_o.blurShade = 10;
        if (s_o.blurShade > 100) s_o.blurShade = 100;

        // [host] allow= / deny=  (v15)  -- see the note on the option itself.
        // deny wins over allow: it is the switch that gets a crashing host
        // working again, so it must not be accidentally overridden.
        GetPrivateProfileStringA("host", "allow", "", s_o.allow, sizeof(s_o.allow), ini);
        GetPrivateProfileStringA("host", "deny",
                                 "QQ.exe,TIM.exe,QQExternal.exe,QQProtect.exe",
                                 s_o.deny, sizeof(s_o.deny), ini);

        // [audio] (v16) -- the microphone, see the note on the option itself.
        s_o.audioOn    = GetPrivateProfileIntA("audio", "enable", 1, ini) != 0;
        s_o.audioGain  = GetPrivateProfileIntA("audio", "gain", 0, ini);
        if (s_o.audioGain < 0)   s_o.audioGain = 0;
        if (s_o.audioGain > 43)  s_o.audioGain = 43;
        s_o.audioWav   = GetPrivateProfileIntA("audio", "wav", 1, ini) != 0;
        s_o.audioMaxMB = GetPrivateProfileIntA("audio", "maxmb", 20, ini);
        if (s_o.audioMaxMB < 0)    s_o.audioMaxMB = 0;
        if (s_o.audioMaxMB > 2000) s_o.audioMaxMB = 2000;
        s_o.audioLog   = GetPrivateProfileIntA("audio", "log", 1, ini);
        if (s_o.audioLog < 0) s_o.audioLog = 0;
        if (s_o.audioLog > 2) s_o.audioLog = 2;
        s_o.audioStrip = GetPrivateProfileIntA("audio", "strip", 1, ini) != 0;

        FLog("options: audio %s (gain=%d wav=%d maxmb=%d log=%d)",
             s_o.audioOn ? "ON" : "off", s_o.audioGain, s_o.audioWav,
             s_o.audioMaxMB, s_o.audioLog);

        FLog("options: yuy2=%s rgb=%s dump=%d busmon=%d bootdelay=%dms (ini=%s)",
             OrientName(s_o.orientYUY2), OrientName(s_o.orientRGB),
             s_o.dump ? 1 : 0, s_o.busMon ? 1 : 0, s_o.bootDelayMs, ini);
        {
            char order[64] = "";
            for (int i = 0; i < s_o.typeCount; ++i)
            {
                const char *n = (s_o.typeOrder[i] == 0) ? "rgb24"
                              : (s_o.typeOrder[i] == 1) ? "yuy2" : "rgb32";
                if (i) strncat_s(order, sizeof(order), ",", _TRUNCATE);
                strncat_s(order, sizeof(order), n, _TRUNCATE);
            }
            if (s_o.layoutW > 0)
                FLog("options: types=%s layout target=%dx%d mode=%s blur=%d dim=%d%%",
                     order, s_o.layoutW, s_o.layoutH, ModeName(s_o.layoutMode),
                     s_o.blurCells, s_o.blurShade);
            else
                FLog("options: types=%s layout target=off (full 640x480 frame)", order);
            FLog("options: host='%s' layout hosts='%s' guide=%d -> layout %s",
                 HostExeName(), s_o.hosts, s_o.guide,
                 (s_o.layoutW > 0 && HostInList(s_o.hosts)) ? "ACTIVE" : "inactive");
        }
    }
    return s_o;
}

//---------------------------------------------------------------------
// v11: the layout box exists for hosts that cut the frame down to their
// own window shape; QQ and the rest must keep the untouched picture, so
// the box is limited to a list of process names.
//---------------------------------------------------------------------
// [layout] hosts=weixin.exe,wechat.exe -- case-insensitive, comma/semicolon
// separated.  An empty list means "every host".
static bool ContainsNoCase(const char *hay, const char *needle)
{
    if (hay == NULL || needle == NULL || needle[0] == '\0')
        return false;
    const size_t nl = strlen(needle);
    for (const char *p = hay; *p != '\0'; ++p)
        if (_strnicmp(p, needle, nl) == 0)
            return true;
    return false;
}

static bool HostInList(const char *list)
{
    if (list == NULL || list[0] == '\0')
        return true;

    char tmp[256] = "";
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s", list);
    const char *me = HostExeName();
    for (char *p = strtok(tmp, ",; \t"); p != NULL; p = strtok(NULL, ",; \t"))
        if (ContainsNoCase(me, p))
            return true;
    return false;
}

// v15: may this process instantiate the filter at all?
//
// deny wins over allow.  It exists for hosts that crash on a capture filter
// they cannot drive: refusing to instantiate is what keeps QQ's settings
// page alive while the camera stays available to everything else.
static bool HostAllowed()
{
    const ISightOptions &o = Opts();
    if (o.deny[0] != '\0' && HostInList(o.deny))
        return false;
    if (o.allow[0] != '\0' && !HostInList(o.allow))
        return false;
    return true;
}

// the rectangle the layout applies to right now, or 0/0 when it is off
static void LayoutTarget(int *pw, int *ph)
{
    const ISightOptions &o = Opts();
    if (o.layoutW > 0 && o.layoutH > 0 && HostInList(o.hosts))
    {
        *pw = o.layoutW;
        *ph = o.layoutH;
    }
    else
    {
        *pw = *ph = 0;
    }
}

//---------------------------------------------------------------------
// v11: re-read the settings that act *inside* the frame while the stream
// is running, so the layout box can be tuned with the call window open
// instead of restarting the host for every attempt.  Checked on the
// streaming thread, at most every 500 ms, and only through the file
// stamp, so the cost is a stat() twice a second.  [format] types= and
// [recovery] are deliberately left out: they are answered while the graph
// is being built and cannot change on a live connection.
//---------------------------------------------------------------------
static void MaybeReloadTunables()
{
    static FILETIME s_stamp;
    static bool     s_haveStamp = false;
    static DWORD    s_nextCheck = 0;

    const DWORD now = GetTickCount();
    if (s_nextCheck != 0 && (LONG)(now - s_nextCheck) < 0)
        return;
    s_nextCheck = now + 500;

    const char *ini = IniPath();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(ini, GetFileExInfoStandard, &fad) == 0)
        return;
    if (s_haveStamp &&
        fad.ftLastWriteTime.dwLowDateTime  == s_stamp.dwLowDateTime &&
        fad.ftLastWriteTime.dwHighDateTime == s_stamp.dwHighDateTime)
        return;
    s_stamp = fad.ftLastWriteTime;
    s_haveStamp = true;

    ISightOptions &o = const_cast<ISightOptions &>(Opts());

    const int  yuy2  = ClampOrient(GetPrivateProfileIntA("orientation", "yuy2", o.orientYUY2, ini));
    const int  rgb   = ClampOrient(GetPrivateProfileIntA("orientation", "rgb",  o.orientRGB,  ini));
    const bool dump  = GetPrivateProfileIntA("debug", "dump", o.dump ? 1 : 0, ini) != 0;
    int guide = GetPrivateProfileIntA("layout", "guide", o.guide, ini);
    if (guide < 0) guide = 0;
    if (guide > 3) guide = 3;

    // v12: the box filler, re-read live as well
    char md[32] = "";
    GetPrivateProfileStringA("layout", "mode", ModeName(o.layoutMode), md, sizeof(md), ini);
    const int mode  = ParseLayoutMode(md);
    int cells = GetPrivateProfileIntA("layout", "blur", o.blurCells, ini);
    if (cells < 4)  cells = 4;
    if (cells > 64) cells = 64;
    int shade = GetPrivateProfileIntA("layout", "dim", o.blurShade, ini);
    if (shade < 10)  shade = 10;
    if (shade > 100) shade = 100;

    int lw = o.layoutW, lh = o.layoutH;
    char lay[64] = "";
    GetPrivateProfileStringA("layout", "target", "", lay, sizeof(lay), ini);
    if (lay[0] != '\0')
    {
        int a = 0, b = 0;
        if (sscanf_s(lay, "%dx%d", &a, &b) == 2 && a > 0 && b > 0)
        {
            if (a < 64) a = 64;
            if (a > ISIGHT_WIDTH)  a = ISIGHT_WIDTH;
            if (b < 64) b = 64;
            if (b > ISIGHT_HEIGHT) b = ISIGHT_HEIGHT;
            lw = a;
            lh = b;
        }
    }
    if (_stricmp(lay, "0") == 0 || _stricmp(lay, "off") == 0)
        lw = lh = 0;

    char hosts[sizeof(o.hosts)] = "";
    GetPrivateProfileStringA("layout", "hosts", o.hosts, hosts, sizeof(hosts), ini);

    const bool changed =
        (yuy2 != o.orientYUY2) || (rgb != o.orientRGB) || (dump != o.dump) ||
        (guide != o.guide) || (lw != o.layoutW) || (lh != o.layoutH) ||
        (mode != o.layoutMode) || (cells != o.blurCells) || (shade != o.blurShade) ||
        (strcmp(hosts, o.hosts) != 0);

    o.orientYUY2 = yuy2;
    o.orientRGB  = rgb;
    o.dump       = dump;
    o.guide      = guide;
    o.layoutW    = lw;
    o.layoutH    = lh;
    o.layoutMode = mode;
    o.blurCells  = cells;
    o.blurShade  = shade;
    _snprintf_s(o.hosts, sizeof(o.hosts), _TRUNCATE, "%s", hosts);

    if (changed)
        FLog("options: reloaded -> yuy2=%s rgb=%s dump=%d guide=%d layout=%dx%d mode=%s blur=%d dim=%d%% hosts='%s'",
             OrientName(yuy2), OrientName(rgb), dump ? 1 : 0, guide,
             lw, lh, ModeName(mode), cells, shade, hosts);
}

//---------------------------------------------------------------------
// format helpers
//---------------------------------------------------------------------
struct SubTypeInfo
{
    const GUID *subtype;
    int   bpp;              // bits per pixel in the DShow buffer
    DWORD compression;      // biCompression value used in the media type
};

static SubTypeInfo SubTypeFor(const GUID *sub)
{
    SubTypeInfo si;
    if (sub && *sub == MEDIASUBTYPE_YUY2)
    {
        si.subtype = &MEDIASUBTYPE_YUY2; si.bpp = 16;
        si.compression = MAKEFOURCC('Y', 'U', 'Y', '2');
    }
    else if (sub && *sub == MEDIASUBTYPE_RGB32)
    {
        si.subtype = &MEDIASUBTYPE_RGB32; si.bpp = 32; si.compression = BI_RGB;
    }
    else
    {
        si.subtype = &MEDIASUBTYPE_RGB24; si.bpp = 24; si.compression = BI_RGB;
    }
    return si;
}

static const char *SubTypeName(const GUID *sub)
{
    if (!sub) return "none";
    if (*sub == MEDIASUBTYPE_RGB24) return "RGB24";
    if (*sub == MEDIASUBTYPE_RGB32) return "RGB32";
    if (*sub == MEDIASUBTYPE_YUY2)  return "YUY2";
    return "other";
}

static ULONG FrameBytesFor(const SubTypeInfo &si)
{
    return (ULONG)(ISIGHT_WIDTH * ISIGHT_HEIGHT * si.bpp / 8);
}

// which orientation the host of this subtype needs (see Opts())
static int OrientForSubType(const GUID *sub)
{
    SubTypeInfo si = SubTypeFor(sub);
    return (*si.subtype == MEDIASUBTYPE_YUY2) ? Opts().orientYUY2 : Opts().orientRGB;
}

//---------------------------------------------------------------------
// row writers.  src is one BGR scanline of the getDIB buffer (bottom-up
// over the whole frame); mirror reverses the pixel order inside a row.
//---------------------------------------------------------------------
static inline ULONG SrcX(ULONG outX, ULONG width, bool mirror)
{
    return mirror ? (width - 1 - outX) : outX;
}

// BGR -> RGB24
static void RowToRGB24(const BYTE *s, BYTE *d, ULONG width, bool mirror)
{
    for (ULONG x = 0; x < width; ++x)
    {
        const BYTE *p = s + (size_t)SrcX(x, width, mirror) * 3;
        *d++ = p[0]; *d++ = p[1]; *d++ = p[2];
    }
}

// BGR -> RGB32 (alpha left opaque, the usual convention)
static void RowToRGB32(const BYTE *s, BYTE *d, ULONG width, bool mirror)
{
    for (ULONG x = 0; x < width; ++x)
    {
        const BYTE *p = s + (size_t)SrcX(x, width, mirror) * 3;
        *d++ = p[0]; *d++ = p[1]; *d++ = p[2]; *d++ = 0xFF;
    }
}

// BGR -> YUY2 (BT.601, chroma averaged over each pair, layout Y0 U Y1 V)
static void RowToYUY2(const BYTE *s, BYTE *d, ULONG width, bool mirror)
{
    for (ULONG x = 0; x < width; x += 2)
    {
        const BYTE *p0 = s + (size_t)SrcX(x,     width, mirror) * 3;
        const BYTE *p1 = s + (size_t)SrcX(x + 1, width, mirror) * 3;
        int B0 = p0[0], G0 = p0[1], R0 = p0[2];
        int B1 = p1[0], G1 = p1[1], R1 = p1[2];
        int Rc = (R0 + R1) >> 1, Gc = (G0 + G1) >> 1, Bc = (B0 + B1) >> 1;

        int Y0 = ((66 * R0 + 129 * G0 + 25 * B0 + 128) >> 8) + 16;
        int Y1 = ((66 * R1 + 129 * G1 + 25 * B1 + 128) >> 8) + 16;
        int U  = ((-38 * Rc - 74 * Gc + 112 * Bc + 128) >> 8) + 128;
        int V  = ((112 * Rc - 94 * Gc - 18 * Bc + 128) >> 8) + 128;

        d[0] = (BYTE)(Y0 < 0 ? 0 : (Y0 > 255 ? 255 : Y0));
        d[1] = (BYTE)(U  < 0 ? 0 : (U  > 255 ? 255 : U));
        d[2] = (BYTE)(Y1 < 0 ? 0 : (Y1 > 255 ? 255 : Y1));
        d[3] = (BYTE)(V  < 0 ? 0 : (V  > 255 ? 255 : V));
        d += 4;
    }
}

//---------------------------------------------------------------------
// copy a whole frame from the getDIB BGR buffer into the outgoing
// buffer, applying the orientation the host for this subtype needs.
//---------------------------------------------------------------------
static void EmitFrame(const BYTE *src, BYTE *dst, ULONG width, ULONG height,
                      int orient, const SubTypeInfo &si)
{
    const ULONG sstride = width * 3;
    const ULONG dstride = width * (ULONG)si.bpp / 8;
    const bool  vf      = OrientDataTopDown(orient);
    const bool  hf      = OrientMirror(orient);
    const bool  yuy2    = (*si.subtype == MEDIASUBTYPE_YUY2);
    const bool  rgb32   = (*si.subtype == MEDIASUBTYPE_RGB32);

    for (ULONG y = 0; y < height; ++y)
    {
        const BYTE *srow = src + (size_t)(vf ? (height - 1 - y) : y) * sstride;
        BYTE *drow = dst + (size_t)y * dstride;

        if (yuy2)       RowToYUY2(srow, drow, width, hf);
        else if (rgb32) RowToRGB32(srow, drow, width, hf);
        else            RowToRGB24(srow, drow, width, hf);
    }
}

//---------------------------------------------------------------------
// The box filler.
//
// Some hosts do not letterbox a 4:3 camera into their own window, they cut
// the picture down to their window shape.  The image we hand over is
// complete and upright (the log proves that), so the only way to get the
// whole scene back into such a host is to put the scene inside the very
// rectangle the host is going to cut out: whatever box it is configured for
// ([layout] target=WxH) is what survives, and the host magnifies it.
//
// v10/v11 fitted the scene into the box and left the remainder black.  A
// 4:3 picture in a 2:3 box uses half the height, so half of the call window
// was black.  v12 keeps all four ways of filling the box:
//
//   mode=fit      whole scene, centred in the box, black around it
//   mode=blur     whole scene on top of a blurred, dimmed copy of the same
//                 frame -- edge to edge, nothing cropped, nothing distorted
//                 (default)
//   mode=fill     scene cropped to the box shape and magnified to fill it --
//                 no black, but the sides are gone and the face is magnified
//   mode=stretch  whole scene squeezed into the box shape -- no black, no
//                 crop, distorted
//
// Everything below works in *image* coordinates (top-down) and writes into
// bottom-up BGR buffers, exactly like the rest of this file.
//---------------------------------------------------------------------

// rectangle-to-rectangle bilinear blit; shade<100 dims what is written
static void BlitScaled(const BYTE *src, ULONG sw, ULONG sh,
                       ULONG sx0, ULONG sy0, ULONG scw, ULONG sch,
                       BYTE *dst, ULONG dw, ULONG dh,
                       ULONG dx0, ULONG dy0, ULONG dcw, ULONG dch,
                       int shade)
{
    if (sw < 2 || sh < 2 || dw < 2 || dh < 2)
        return;
    if (scw < 1) scw = 1;
    if (sch < 1) sch = 1;
    if (dcw < 1) dcw = 1;
    if (dch < 1) dch = 1;
    if ((unsigned long long)sx0 + scw > sw) sx0 = (scw >= sw) ? 0 : sw - scw;
    if ((unsigned long long)sy0 + sch > sh) sy0 = (sch >= sh) ? 0 : sh - sch;
    if (scw > sw) scw = sw;
    if (sch > sh) sch = sh;
    if (dx0 >= dw || dy0 >= dh) return;
    if ((unsigned long long)dx0 + dcw > dw) dcw = dw - dx0;
    if ((unsigned long long)dy0 + dch > dh) dch = dh - dy0;

    const int sh8 = (shade < 0) ? 0 : (shade > 100 ? 100 : shade);
    const ULONG sstride = sw * 3;
    const ULONG dstride = dw * 3;
    const long  maxsx = (long)sw - 2;                 // bilinear needs a neighbour
    const long  maxsy = (long)sh - 2;

    for (ULONG dy = 0; dy < dch; ++dy)
    {
        const ULONG syF = (ULONG)(((unsigned long long)dy * sch * 65536ULL) / dch);
        long sy = (long)sy0 + (long)(syF >> 16);
        if (sy > maxsy) sy = maxsy;
        if (sy < 0)     sy = 0;
        const ULONG fy = (syF >> 8) & 0xFF;

        const BYTE *r0 = src + (size_t)(sh - 1 - (ULONG)sy) * sstride;
        const BYTE *r1 = src + (size_t)(sh - 2 - (ULONG)sy) * sstride;
        BYTE *drow = dst + (size_t)(dh - 1 - (dy0 + dy)) * dstride + (size_t)dx0 * 3;

        for (ULONG dx = 0; dx < dcw; ++dx)
        {
            const ULONG sxF = (ULONG)(((unsigned long long)dx * scw * 65536ULL) / dcw);
            long sx = (long)sx0 + (long)(sxF >> 16);
            if (sx > maxsx) sx = maxsx;
            if (sx < 0)     sx = 0;
            const ULONG fx = (sxF >> 8) & 0xFF;

            const BYTE *p00 = r0 + (size_t)sx * 3;
            const BYTE *p01 = r1 + (size_t)sx * 3;
            BYTE *d = drow + (size_t)dx * 3;
            for (int c = 0; c < 3; ++c)
            {
                const ULONG top = (ULONG)p00[c] * (256 - fx) + (ULONG)p00[c + 3] * fx;
                const ULONG bot = (ULONG)p01[c] * (256 - fx) + (ULONG)p01[c + 3] * fx;
                ULONG v = (top * (256 - fy) + bot * fy) >> 16;
                if (sh8 < 100)
                    v = (v * (ULONG)sh8) / 100;
                d[c] = (BYTE)v;
            }
        }
    }
}

// the centred source rectangle whose shape matches the box (used to fill it
// without black bands: the picture is cropped, not distorted)
static void CoverSrcRect(ULONG sw, ULONG sh, ULONG boxW, ULONG boxH,
                         ULONG *px, ULONG *py, ULONG *pw, ULONG *ph)
{
    ULONG w = sw, h = sh;
    if ((unsigned long long)sw * boxH > (unsigned long long)sh * boxW)
        w = (ULONG)(((unsigned long long)sh * boxW) / boxH);   // crop the sides
    else
        h = (ULONG)(((unsigned long long)sw * boxH) / boxW);   // crop top/bottom
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    if (w > sw) w = sw;
    if (h > sh) h = sh;
    *px = (sw - w) / 2;
    *py = (sh - h) / 2;
    *pw = w;
    *ph = h;
}

// build the frame a shape-cropping host will magnify
static void ComposeBox(const BYTE *src, ULONG sw, ULONG sh,
                       BYTE *dst, ULONG dw, ULONG dh,
                       ULONG boxW, ULONG boxH, int mode,
                       BYTE *blurBuf, ULONG blurBytes, int cells, int shade)
{
    if (sw < 2 || sh < 2 || dw < 2 || dh < 2)
        return;
    if (boxW < 2 || boxH < 2)
        return;
    if (boxW > dw) boxW = dw;
    if (boxH > dh) boxH = dh;
    const ULONG bx = (dw - boxW) / 2;
    const ULONG by = (dh - boxH) / 2;

    if (mode == LAYOUT_BLUR && blurBuf != NULL)
    {
        // a tiny copy of the frame (one sample every `cells` pixels) ...
        const int cell = (cells < 4) ? 4 : (cells > 64 ? 64 : cells);
        ULONG gw = dw / (ULONG)cell;
        ULONG gh = dh / (ULONG)cell;
        if (gw < 2) gw = 2;
        if (gh < 2) gh = 2;
        while (gw > 2 && gh > 2 && (unsigned long long)gw * gh * 3 > blurBytes)
        {
            gw = (gw + 1) / 2;
            gh = (gh + 1) / 2;
        }
        if ((unsigned long long)gw * gh * 3 <= blurBytes)
        {
            BlitScaled(src, sw, sh, 0, 0, sw, sh, blurBuf, gw, gh, 0, 0, gw, gh, 100);
            // ... stretched back over the whole frame and dimmed: a soft
            // backdrop for the sharp picture that goes on top
            BlitScaled(blurBuf, gw, gh, 0, 0, gw, gh, dst, dw, dh, 0, 0, dw, dh, shade);
        }
    }

    if (mode == LAYOUT_STRETCH)
    {
        BlitScaled(src, sw, sh, 0, 0, sw, sh, dst, dw, dh, bx, by, boxW, boxH, 100);
        return;
    }

    if (mode == LAYOUT_FILL)
    {
        ULONG cx, cy, cw, ch;
        CoverSrcRect(sw, sh, boxW, boxH, &cx, &cy, &cw, &ch);
        BlitScaled(src, sw, sh, cx, cy, cw, ch, dst, dw, dh, bx, by, boxW, boxH, 100);
        return;
    }

    // fit (and the sharp layer of blur): the scene keeps its shape inside
    // the box, centred; whatever is left stays as it was (black, or the
    // blurred backdrop in blur mode)
    ULONG rw = boxW, rh = boxH;
    if ((unsigned long long)rw * sh > (unsigned long long)rh * sw)
        rw = (ULONG)(((unsigned long long)rh * sw) / sh);
    else
        rh = (ULONG)(((unsigned long long)rw * sh) / sw);
    if (rw < 2) rw = 2;
    if (rh < 2) rh = 2;
    if (rw > dw) rw = dw;
    if (rh > dh) rh = dh;
    BlitScaled(src, sw, sh, 0, 0, sw, sh, dst, dw, dh,
               (dw - rw) / 2, (dh - rh) / 2, rw, rh, 100);
}

//---------------------------------------------------------------------
// v11: paint the layout marker straight into the frame.  Everything drawn
// is symmetric about the centre, so the optional vertical/horizontal flip
// cannot move it.  Colours are BGR and the buffer is bottom-up: image row
// y lives at buffer row (height-1-y).
//---------------------------------------------------------------------
static void GuideRow(BYTE *buf, ULONG dw, ULONG dh, int imgY, int x0, int x1,
                     BYTE r, BYTE g, BYTE b, int thick)
{
    if (imgY < 0 || imgY >= (int)dh)
        return;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)dw - 1) x1 = (int)dw - 1;
    if (x1 < x0)
        return;
    for (int t = 0; t < thick; ++t)
    {
        const int y = imgY + t;
        if (y >= (int)dh)
            break;
        BYTE *row = buf + (size_t)(dh - 1 - y) * dw * 3;
        for (int x = x0; x <= x1; ++x)
        {
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
    }
}

static void GuideBox(BYTE *buf, ULONG dw, ULONG dh, int x, int y, int w, int h,
                     BYTE r, BYTE g, BYTE b, int thick)
{
    if (w < 2 || h < 2)
        return;
    GuideRow(buf, dw, dh, y,             x, x + w - 1, r, g, b, thick);
    GuideRow(buf, dw, dh, y + h - thick, x, x + w - 1, r, g, b, thick);
    for (int i = 0; i < h; ++i)
    {
        GuideRow(buf, dw, dh, y + i, x,             x + thick - 1, r, g, b, 1);
        GuideRow(buf, dw, dh, y + i, x + w - thick, x + w - 1,     r, g, b, 1);
    }
}

static void DrawGuide(BYTE *buf, ULONG dw, ULONG dh, int boxW, int boxH, int level)
{
    if (level & 1)
    {
        // yellow: the whole frame, i.e. everything the camera handed over
        GuideBox(buf, dw, dh, 0, 0, (int)dw, (int)dh, 255, 255, 0, 2);

        if (boxW > 0 && boxH > 0)
        {
            // green: the layout target, the rectangle the host is expected
            // to cut out (always full height when boxH == 480)
            int tw = boxW, th = boxH;
            if (tw > (int)dw) tw = (int)dw;
            if (th > (int)dh) th = (int)dh;
            const int tx = ((int)dw - tw) / 2;
            const int ty = ((int)dh - th) / 2;
            GuideBox(buf, dw, dh, tx, ty, tw, th, 0, 255, 0, 2);

            // red: the part of the box the *picture* actually fills, which
            // is what tells a fit from a blur/fill frame in a screenshot
            int rw = boxW, rh = boxH;
            if (rw * (int)dh > rh * (int)dw) rw = (rh * (int)dw) / (int)dh;
            else                             rh = (rw * (int)dh) / (int)dw;
            const int rx = ((int)dw - rw) / 2;
            const int ry = ((int)dh - rh) / 2;
            GuideBox(buf, dw, dh, rx, ry, rw, rh, 255, 0, 0, 2);
        }

        // white cross dead centre, so a screenshot can be measured
        const int cx  = (int)dw / 2;
        const int cy  = (int)dh / 2;
        const int arm = 24;
        GuideRow(buf, dw, dh, cy, cx - arm, cx + arm, 255, 255, 255, 1);
        for (int i = -arm; i <= arm; ++i)
            GuideRow(buf, dw, dh, cy + i, cx, cx, 255, 255, 255, 1);
    }

    if (level & 2)
    {
        // v12 ruler: thin lines every 40 px, thick colour-coded ones at
        // 160 / 320 / 480 columns and 120 / 360 rows.  How many of them
        // survive in the host's window measures its crop to the pixel, and
        // the spacing of the thin ones shows whether it also scales the
        // picture unevenly.  Colours are unique per landmark so they can be
        // found automatically in a screenshot.
        // vertical landmarks, full height, one colour each:
        // 0 red, 160 magenta, 320 blue, 480 cyan
        for (int x = 0; x < (int)dw; x += 160)
        {
            BYTE r = 255, g = 0, b = 0;
            if (x == 160) { r = 255; g = 0; b = 255; }
            if (x == 320) { r = 0;   g = 0; b = 255; }
            if (x == 480) { r = 0;   g = 255; b = 255; }
            for (int t = 0; t < 3; ++t)
                GuideRow(buf, dw, dh, 0, x + t, x + t, r, g, b, (int)dh);
        }
        // horizontal landmarks every 120 px, full width, green
        for (int y = 0; y < (int)dh; y += 120)
            GuideRow(buf, dw, dh, y, 0, (int)dw - 1, 0, 255, 0, 3);
        // thin grey grid every 40 px in both directions
        for (int x = 40; x < (int)dw; x += 40)
            if (x % 160 != 0)
                GuideRow(buf, dw, dh, 0, x, x, 90, 90, 90, (int)dh);
        for (int y = 40; y < (int)dh; y += 40)
            if (y % 120 != 0)
                GuideRow(buf, dw, dh, y, 0, (int)dw - 1, 90, 90, 90, 1);
    }
}

//---------------------------------------------------------------------
// debug: throw one outgoing frame at %LOCALAPPDATA%\iSightCam-dump so it
// can be decoded offline.  The file holds the buffer in *memory order*,
// exactly as the host receives it, and the file name records which
// biHeight sign was declared -- enough to tell whether the host flipped
// it.  Only active when "dump=1" is in the ini.
//---------------------------------------------------------------------
static void DumpFrame(const BYTE *buf, const SubTypeInfo &si, ULONG width, ULONG height,
                      int orient, ULONG index)
{
    char dir[MAX_PATH] = "";
    if (GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH) == 0)
        GetTempPathA(MAX_PATH, dir);

    char path[MAX_PATH] = "";
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\iSightCam-dump", dir);
    CreateDirectoryA(path, NULL);

    char file[MAX_PATH] = "";
    _snprintf_s(file, sizeof(file), _TRUNCATE, "%s\\frame-%s-%s-pid%lu-%lu.raw",
                path, SubTypeName(si.subtype),
                OrientDataTopDown(orient) ? "topdown" : "bottomup",
                (unsigned long)GetCurrentProcessId(), (unsigned long)index);

    ULONG bytes = width * height * (ULONG)si.bpp / 8;
    FILE *f = fopen(file, "wb");
    if (f == NULL)
        return;
    fwrite(buf, 1, bytes, f);
    fclose(f);
    FLog("DumpFrame: %s (%s %lux%lu orient=%s, %lu bytes)",
         file, SubTypeName(si.subtype), width, height, OrientName(orient),
         (unsigned long)bytes);
}

//---------------------------------------------------------------------
// CiSightStream : one output pin, delivering RGB24 / YUY2 / RGB32
//---------------------------------------------------------------------
class CiSightStream : public CSourceStream, public IAMStreamConfig, public IKsPropertySet
{
public:
    DECLARE_IUNKNOWN
    CiSightStream(HRESULT *phr, CSource *pFilter, LPCWSTR pName);
    ~CiSightStream();

    // CSourceStream overrides
    HRESULT FillBuffer(IMediaSample *pSample);
    HRESULT DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pprop);
    HRESULT GetMediaType(int iPosition, CMediaType *pMediaType);
    HRESULT CheckMediaType(const CMediaType *pMediaType);
    HRESULT Active();
    HRESULT Inactive();

    // v14: the stock buffer loop gives up for good the moment Deliver()
    // reports anything other than S_OK -- it returns from
    // DoBufferProcessingLoop() and the worker thread parks on its request
    // queue, so FillBuffer() is never called again and the graph stays
    // "connected" with a frozen picture until the host tears it down.  A
    // host that starts the graph while its renderer is still transitioning
    // gets exactly that.  We take the loop over so a rejected sample is
    // retried instead of being fatal.
    HRESULT DoBufferProcessingLoop();

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE *pmt);
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE **ppmt);
    STDMETHODIMP GetNumberOfCapabilities(int *piCount, int *piSize);
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC);

    // IKsPropertySet -- this is how ICaptureGraphBuilder2 finds the
    // PIN_CATEGORY_CAPTURE pin of a source filter.
    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwID,
                     void *pInstanceData, DWORD cbInstanceData,
                     void *pPropData, DWORD cbPropData);
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwID,
                     void *pInstanceData, DWORD cbInstanceData,
                     void *pPropData, DWORD cbPropData, DWORD *pcbReturned);
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD *pTypeSupport);

    // expose IAMStreamConfig / IKsPropertySet
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv);

private:
    bool  TryStart();          // background bring-up worker
    void  KickBringUp();       // start TryStart() off-thread (never blocks)
    static DWORD WINAPI BringUpThunk(LPVOID p);
    void  ReleaseCamera();     // stop acquisition + drop the CMU handle
    void  DropStreamForRecovery(const char *why);   // safe teardown from FillBuffer
    void  KickBusMonitor();    // start watching the 1394 bus generation
    void  StopBusMonitor();
    static DWORD WINAPI BusMonThunk(LPVOID p);
    void  ConfigureVideo();
    // v16 -- microphone: start/stop the camera's audio unit, and pick the
    // audio packets out of the capture buffer the video is already in.
    void  AudioStart();
    void  AudioStop();
    void  AudioScanFrame();
    void  BuildMediaType(const GUID *subtype, REFERENCE_TIME interval, CMediaType *pmt) const;
    bool  MediaTypeCompatible(const CMediaType *pmt, CMediaType *pNormalized) const;

    CCritSec         m_csCamera;      // serialize camera access
    C1394Camera     *m_pCam;          // CMU camera object (owned)
    bool             m_bInit;         // camera selected+initialized
    bool             m_bAcquiring;    // isoch acquisition running
    unsigned long    m_width, m_height;
    int              m_rateIndex;
    REFERENCE_TIME   m_rtNext;        // timestamp of next frame
    bool             m_bFirstFrame;
    int              m_consecFail;    // consecutive capture failures
    unsigned long    m_frameCount;    // frames delivered since Active
    PBYTE            m_pScratch;      // RGB24 DIB scratch buffer
    ULONG            m_scratchBytes;
    PBYTE            m_pFull;         // 640x480 BGR scratch (letterbox path)
    PBYTE            m_pFit;          // v10: [layout] target= box composite
    PBYTE            m_pBlur;         // v12: tiny frame copy behind the picture
    ULONG            m_dumpCount;     // frames written to the debug dump (max 2)
    volatile PVOID   m_hBringUp;      // background bring-up thread
    volatile LONG    m_bringUpState;  // 0 idle, 1 running, 2 ok, 3 failed
    int              m_bringUpTries;  // bring-up attempts since the last good frame
    DWORD            m_nextTryMs;     // earliest tick for the next bring-up attempt
    DWORD            m_retryNotBefore;// after a bus reset: let the camera boot first

    // bus monitor: notices a power cycle (bus reset / device disappears)
    // long before any acquire can time out
    volatile PVOID   m_hMon;          // monitor thread
    HANDLE           m_evMonStop;     // signalled to stop the monitor
    volatile LONG    m_busResetSeen;  // generation changed while streaming
    volatile LONG    m_camGoneSeen;   // camera left the bus (switched off)
    volatile LONG    m_needReset;     // teardown pending (done in TryStart)
    char             m_monPath[512];  // cached 1394 device path

    // v12 probes.  The camera streams perfectly from the standalone CMU
    // diagnostic while the filter gets no frame at all, so the one thing that
    // has to be measured is *where the streaming thread stops*: FillBuffer
    // entry count tells us whether DirectShow still calls us, and the timing
    // around AcquireImageEx tells us whether we are inside the driver.
    volatile LONG    m_fbCalls;        // FillBuffer entries since Active
    volatile LONG    m_acqCalls;       // AcquireImageEx calls since Active
    volatile LONG    m_lastAcqEnterMs; // tick when the current acquire began
    volatile LONG    m_acqBlockedMs;   // duration of the last AcquireImageEx
    LONG             m_rejectStreak;   // v14: consecutive rejected deliveries
    LONG             m_loopRuns;       // v14: buffer-loop entries (thread alive)
    volatile LONG    m_acqStartMs;     // tick when acquisition was started

    // v16 microphone state
    bool             m_audioOn;        // the audio unit was told to transmit
    bool             m_audioSeen;      // at least one audio packet decoded
    FILE            *m_audioFile;      // PCM capture (optional)
    char             m_audioPath[MAX_PATH];
    LONG             m_audioPackets;   // packets decoded
    LONG             m_audioFrames;    // audio frames decoded (48000/s)
    LONG             m_audioLost;      // frames lost (sample_total gaps)
    LONG             m_audioBad;       // 'sght' hits that failed validation
    LONG             m_audioFramesScanned;  // video frames scanned for audio
    ULONG            m_audioExpect;    // next expected sample_total
    bool             m_audioExpectValid;
    DWORD            m_audioStartMs;   // when the audio unit was started
    LONG             m_audioPeak;      // loudest sample seen
    LONG             m_audioBytes;     // bytes written to the wav file

    // v17: audio packets are spliced into the picture by the 1394 driver
    // (measured: ~14 packets of 816 bytes per frame -- ten horizontal
    // seams and an 11 KB rainbow band at the bottom, see the PNG dumps).
    // StripFrame() cuts them out so the front of the buffer is a clean
    // 640x480 UYVY picture again.
    LONG             m_audioStripPackets;  // packets cut this session
    LONG             m_audioStripFrames;   // frames compacted
    LONG             m_audioStripBytes;    // bytes cut this session
    LONG             m_audioTailBytes;     // bytes left in the last compacted frame
};

//---------------------------------------------------------------------
// CiSightSource : the filter
//---------------------------------------------------------------------
class CiSightSource : public CSource, public IAMFilterMiscFlags
{
public:
    DECLARE_IUNKNOWN
    static CUnknown *WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT *phr);

    // Live source. Hosts (WeChat/QQ and ICaptureGraphBuilder2 alike) query
    // this to decide whether the device is a real-time source; a capture
    // filter that does not answer IAMFilterMiscFlags gets treated like a
    // file reader and the preview path bails out.
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv);

private:
    CiSightSource(LPUNKNOWN lpunk, HRESULT *phr);
    ~CiSightSource() { FLog("~CiSightSource: filter destroyed"); }
};

//---------------------------------------------------------------------
CiSightSource::CiSightSource(LPUNKNOWN lpunk, HRESULT *phr)
    : CSource(NAME("Apple iSight FireWire Capture"), lpunk, CLSID_ISightFireWireCam)
{
    FLog("CiSightSource: filter created");
    // one capture output pin
    CiSightStream *pPin = new CiSightStream(phr, this, L"Capture");
    if (pPin == NULL)
        *phr = E_OUTOFMEMORY;
}

CUnknown *WINAPI CiSightSource::CreateInstance(LPUNKNOWN lpunk, HRESULT *phr)
{
    ASSERT(phr != NULL);
    CUnknown *punk = new CiSightSource(lpunk, phr);
    if (punk == NULL)
        *phr = E_OUTOFMEMORY;
    return punk;
}

// every QI a host makes on the filter is logged: when an application
// "sees the camera but cannot open it", this is the list that shows
// which interface it was looking for.
STDMETHODIMP CiSightSource::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    HRESULT hr;
    if (riid == __uuidof(IAMFilterMiscFlags))
        hr = GetInterface((IAMFilterMiscFlags *)this, ppv);
    else
        hr = CSource::NonDelegatingQueryInterface(riid, ppv);

    // throttled per interface: the first few probes of each interface are
    // logged (that is the list that matters when a host refuses to open the
    // device), the 11 000th repeat of the same probe is not.
    char key[64];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "filter-qi-%s", GuidName(riid));
    FLogT(key, "filter QI %s -> %s", GuidName(riid), SUCCEEDED(hr) ? "OK" : "E_NOINTERFACE");
    return hr;
}

//---------------------------------------------------------------------
// CiSightStream implementation
//---------------------------------------------------------------------
CiSightStream::CiSightStream(HRESULT *phr, CSource *pFilter, LPCWSTR pName)
    : CSourceStream(NAME("iSight Capture Pin"), phr, pFilter, pName)
    , m_pCam(NULL)
    , m_bInit(false)
    , m_bAcquiring(false)
    , m_width(ISIGHT_WIDTH)
    , m_height(ISIGHT_HEIGHT)
    , m_rateIndex(kMaxRateIndex)
    , m_rtNext(0)
    , m_bFirstFrame(true)
    , m_consecFail(0)
    , m_frameCount(0)
    , m_pScratch(NULL)
    , m_scratchBytes(0)
    , m_pFull(NULL)
    , m_pFit(NULL)
    , m_pBlur(NULL)
    , m_dumpCount(0)
    , m_hBringUp(NULL)
    , m_bringUpState(0)
    , m_bringUpTries(0)
    , m_nextTryMs(0)
    , m_retryNotBefore(0)
    , m_hMon(NULL)
    , m_evMonStop(NULL)
    , m_busResetSeen(0)
    , m_camGoneSeen(0)
    , m_needReset(0)
    , m_fbCalls(0)
    , m_acqCalls(0)
    , m_lastAcqEnterMs(0)
    , m_acqBlockedMs(0)
    , m_rejectStreak(0)
    , m_loopRuns(0)
    , m_acqStartMs(0)
    , m_audioOn(false)
    , m_audioSeen(false)
    , m_audioFile(NULL)
    , m_audioPackets(0)
    , m_audioFrames(0)
    , m_audioLost(0)
    , m_audioBad(0)
    , m_audioFramesScanned(0)
    , m_audioExpect(0)
    , m_audioExpectValid(false)
    , m_audioStartMs(0)
    , m_audioPeak(0)
    , m_audioBytes(0)
    , m_audioStripPackets(0)
    , m_audioStripFrames(0)
    , m_audioStripBytes(0)
    , m_audioTailBytes(0)
{
    m_monPath[0] = 0;
    m_audioPath[0] = 0;
}

CiSightStream::~CiSightStream()
{
    FLog("~CiSightStream");
    ReleaseCamera();
    if (m_pScratch)
    {
        delete[] m_pScratch;
        m_pScratch = NULL;
    }
    if (m_pFull)
    {
        delete[] m_pFull;
        m_pFull = NULL;
    }
    if (m_pBlur)
    {
        delete[] m_pBlur;
        m_pBlur = NULL;
    }
    if (m_pFit)
    {
        delete[] m_pFit;
        m_pFit = NULL;
    }
}

// Camera bring-up takes ~15 s (the CMU library reads the whole capability
// table register by register). It therefore must not run on a thread the
// host is waiting on: Active()/Pause()/Run() and FillBuffer() all have to
// return promptly, so the work is handed to this one-shot thread and the
// pin keeps emitting black frames until the camera is live.
void CiSightStream::KickBringUp()
{
    if (InterlockedCompareExchange(&m_bringUpState, 0, 0) == 1)
        return;                                   // already running

    // drop the handle of the previous (finished) attempt
    PVOID old = InterlockedExchangePointer(&m_hBringUp, NULL);
    if (old) CloseHandle((HANDLE)old);

    InterlockedExchange(&m_bringUpState, 1);
    HANDLE h = CreateThread(NULL, 0, &CiSightStream::BringUpThunk, this, 0, NULL);
    if (h == NULL)
    {
        // no thread available: do it synchronously (old behaviour) rather
        // than never bringing the camera up at all
        InterlockedExchange(&m_bringUpState, TryStart() ? 2 : 3);
        return;
    }
    InterlockedExchangePointer(&m_hBringUp, (PVOID)h);   // closed in ReleaseCamera
    FLogT("kick-bringup", "KickBringUp: camera bring-up started on a worker thread");
}

DWORD WINAPI CiSightStream::BringUpThunk(LPVOID p)
{
    CiSightStream *self = (CiSightStream *)p;
    bool ok = self->TryStart();
    InterlockedExchange(&self->m_bringUpState, ok ? 2 : 3);
    return 0;
}

//---------------------------------------------------------------------
// bus monitor
//
// The front ring of the iSight is its power switch.  Switching it off and
// on again resets the 1394 bus: the camera comes back at a different node
// address and the isochronous stream we were running is gone for good --
// the handle we still hold never produces another frame.  Every bus reset
// bumps IOCTL_GET_GENERATION_COUNT, and while the camera is off its device
// object leaves the bus, so polling that single value every 200 ms reports
// the event almost immediately.  (Waiting for it through acquire timeouts
// took a minute: three 2 s timeouts is already 6 s, and after the camera
// disappears the driver's waits get longer still.)
//---------------------------------------------------------------------
static const DWORD kIoctlGetGenerationCount =
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x0800 + 26, METHOD_BUFFERED, FILE_ANY_ACCESS);

DWORD WINAPI CiSightStream::BusMonThunk(LPVOID p)
{
    CiSightStream *self = (CiSightStream *)p;
    HANDLE dev = INVALID_HANDLE_VALUE;
    DWORD  lastGen = 0;
    bool   haveGen = false;
    bool   wasPresent = false;
    int    openFails = 0;
    int    hb = 0;                                  // v12 probe heartbeat counter
    LONG   lastFb = -1;                             // v14: FillBuffer entry count
    int    stuck  = 0;                              // v14: consecutive frozen ticks

    for (;;)
    {
        if (WaitForSingleObject(self->m_evMonStop, 200) != WAIT_TIMEOUT)
            break;                                  // asked to stop

        // v12 probe: a heartbeat that keeps ticking even when the streaming
        // thread is stuck inside the driver.  If "fbCalls" stops growing here
        // while the graph is running, DirectShow no longer calls us; if it
        // grows but "frames" stays at 0 and "acqBlockedMs" is large, the
        // driver never completes the overlapped frame.
        if (++hb >= 10)                             // 10 x 200 ms = 2 s
        {
            hb = 0;
            FLog("probe: fbCalls=%ld frames=%lu acquiring=%d cam=%p init=%d acqCalls=%ld acqBlockedMs=%ld loopRuns=%ld",
                 (long)self->m_fbCalls, self->m_frameCount,
                 self->m_bAcquiring ? 1 : 0, (void *)self->m_pCam,
                 self->m_bInit ? 1 : 0, (long)self->m_acqCalls,
                 (long)self->m_acqBlockedMs, (long)self->m_loopRuns);

            // v14: the killer signature.  The streaming thread only stops
            // calling FillBuffer when the buffer loop has returned -- which
            // is what the stock loop does as soon as Deliver() fails.  Say
            // so out loud, because from the host side this looks like a
            // connected but dead camera, with no error anywhere.
            const LONG fbNow = (LONG)self->m_fbCalls;
            if (fbNow != lastFb)
            {
                lastFb = fbNow;
                stuck  = 0;
            }
            else if (++stuck == 4 || (stuck % 15) == 0)
            {
                FLog("probe: STREAM PARKED -- FillBuffer frozen at #%ld for %ds "
                     "(loopRuns=%ld frames=%lu acquiring=%d)",
                     (long)fbNow, stuck * 2, (long)self->m_loopRuns,
                     self->m_frameCount, self->m_bAcquiring ? 1 : 0);
            }
        }

        if (dev == INVALID_HANDLE_VALUE)
        {
            if (self->m_monPath[0] == 0)
            {
                // Resolve the camera's device path.  It contains the camera's
                // EUI-64, so it survives bus resets and only has to be looked
                // up again while the camera is off the bus.
                //
                // t1394CmdrGetDeviceList() builds a fresh SetupAPI device list
                // on every call and never frees it (the CMU library leaks one
                // per RefreshCameraList as well), so we destroy it ourselves --
                // a camera left switched off would otherwise leak a handle
                // every 200 ms.  GetDevicePath keeps no state, so calling it
                // here needs no lock.
                HDEVINFO list = t1394CmdrGetDeviceList();
                if (list != INVALID_HANDLE_VALUE)
                {
                    char path[512] = "";
                    ULONG sz = sizeof(path);
                    if (t1394CmdrGetDevicePath(list, 0, path, &sz) > 0)
                        strncpy_s(self->m_monPath, path, _TRUNCATE);
                    SetupDiDestroyDeviceInfoList(list);
                }
                if (self->m_monPath[0] == 0)
                {
                    if (wasPresent)
                    {
                        wasPresent = false;
                        haveGen = false;
                        InterlockedIncrement(&self->m_camGoneSeen);
                        FLog("busmon: camera left the bus (switched off?)");
                    }
                    continue;                       // look again in 200 ms
                }
            }

            dev = CreateFileA(self->m_monPath, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, 0, NULL);
            if (dev == INVALID_HANDLE_VALUE)
            {
                DWORD err = GetLastError();
                if (wasPresent)
                {
                    wasPresent = false;
                    haveGen = false;
                    InterlockedIncrement(&self->m_camGoneSeen);
                    FLog("busmon: camera device gone (open err=%lu)", err);
                }
                else if (++openFails > 30)
                {
                    // the camera is on the bus (the CMU library sees it) but
                    // this process may not open it: stop watching rather than
                    // keep poking.  The acquire-failure path still recovers.
                    FLog("busmon: cannot open the device (err=%lu) - monitoring disabled", err);
                    break;
                }
                self->m_monPath[0] = 0;             // node is not there any more
                continue;
            }
            openFails = 0;
            if (!wasPresent)
            {
                wasPresent = true;
                FLog("busmon: camera present, watching the bus generation");
            }
            haveGen = false;                        // the counter belonged to the old handle
        }

        DWORD gen = 0, ret = 0;
        if (DeviceIoControl(dev, kIoctlGetGenerationCount, NULL, 0,
                            &gen, sizeof(gen), &ret, NULL))
        {
            if (haveGen && gen != lastGen)
            {
                InterlockedIncrement(&self->m_busResetSeen);
                FLog("busmon: BUS RESET detected (generation %lu -> %lu)", lastGen, gen);
            }
            lastGen = gen;
            haveGen = true;
        }
        else
        {
            CloseHandle(dev);                       // handle died with the bus
            dev = INVALID_HANDLE_VALUE;
        }
    }

    if (dev != INVALID_HANDLE_VALUE)
        CloseHandle(dev);
    return 0;
}

void CiSightStream::KickBusMonitor()
{
    if (!Opts().busMon)
        return;
    if (InterlockedCompareExchangePointer(&m_hMon, NULL, NULL) != NULL)
        return;                                     // already running

    if (m_evMonStop == NULL)
    {
        m_evMonStop = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (m_evMonStop == NULL)
            return;
    }
    ResetEvent(m_evMonStop);
    InterlockedExchange(&m_busResetSeen, 0);
    InterlockedExchange(&m_camGoneSeen, 0);

    HANDLE h = CreateThread(NULL, 0, &CiSightStream::BusMonThunk, this, 0, NULL);
    if (h == NULL)
    {
        FLog("busmon: could not start the monitor thread");
        return;
    }
    InterlockedExchangePointer(&m_hMon, (PVOID)h);
    FLog("busmon: watching for bus resets / camera power cycles");
}

void CiSightStream::StopBusMonitor()
{
    if (m_evMonStop)
        SetEvent(m_evMonStop);
    PVOID h = InterlockedExchangePointer(&m_hMon, NULL);
    if (h)
    {
        WaitForSingleObject((HANDLE)h, 3000);
        CloseHandle((HANDLE)h);
    }
    if (m_evMonStop)
    {
        CloseHandle(m_evMonStop);
        m_evMonStop = NULL;
    }
}

// Called from the streaming thread when the bus monitor reports a reset.
//
// It deliberately does NOT touch m_pCam: the bring-up thread may be holding
// the camera lock for several seconds (InitCamera + ConfigureVideo), and
// blocking the streaming thread on that lock would stall the whole graph --
// hosts drop a source that stops delivering samples.  So this only
// invalidates the stream; the teardown itself happens in TryStart(), which
// already runs under that lock.
void CiSightStream::DropStreamForRecovery(const char *why)
{
    InterlockedExchange(&m_needReset, 1);
    m_bAcquiring = false;      // only the streaming thread writes this
    m_bInit = false;
    m_consecFail = 0;
    m_bringUpTries = 0;
    InterlockedExchange(&m_acqStartMs, 0);
    // give the camera time to boot before we talk to it again, otherwise the
    // first attempt is wasted on a node that is not listening yet
    m_retryNotBefore = TickMs() + (DWORD)Opts().bootDelayMs;
    m_nextTryMs = m_retryNotBefore;
    InterlockedExchange(&m_bringUpState, 0);   // re-arm the bring-up worker
    FLog("recovery: stream invalidated (%s) after %lu frames, new attempt in %d ms",
         why, m_frameCount, Opts().bootDelayMs);
}

// bring the camera up and start isochronous acquisition. never throws,
// returns false quietly when hardware is absent/unready.
bool CiSightStream::TryStart()
{
    CAutoLock lock(&m_csCamera);

    // A bus reset (camera switched off / on) invalidated everything we had:
    // the node address changed, so the handle we hold is dead and must go
    // before we enumerate again.  Doing it here keeps the (slow) teardown
    // off the streaming thread -- see DropStreamForRecovery().
    if (InterlockedExchange(&m_needReset, 0))
    {
        if (m_pCam)
        {
            if (m_bAcquiring)
            {
                m_pCam->StopImageAcquisition();
                m_bAcquiring = false;
            }
            delete m_pCam;
            m_pCam = NULL;
            FLog("recovery: stale camera handle released, re-enumerating");
        }
        m_bInit = false;
    }

    if (m_bAcquiring)
        return true;

    if (!m_bInit)
    {
        if (m_pCam == NULL)
        {
            try { m_pCam = new C1394Camera(); }
            catch (...) { m_pCam = NULL; }
            if (m_pCam == NULL)
            {
                FLog("TryStart: new C1394Camera failed");
                return false;
            }
        }

        DWORD t0 = TickMs();
        // Skip the CMU library's per-feature inquiry walk: it costs ~14 s on
        // this driver (one synchronous IOCTL per register) and a capture
        // filter exposes none of those controls anyway.  This is what
        // brings the camera up in well under a second.
        g_bISightFastBringUp = TRUE;

        int cameras = m_pCam->RefreshCameraList();
        if (cameras <= 0)
        {
            FLogT("no-camera", "TryStart: RefreshCameraList -> %d (camera off the bus?)", cameras);
            delete m_pCam; m_pCam = NULL;
            return false;
        }
        if (m_pCam->SelectCamera(0) != CAM_SUCCESS)
        {
            FLogT("select-fail", "TryStart: SelectCamera(0) failed");
            delete m_pCam; m_pCam = NULL;
            return false;
        }
        DWORD t1 = TickMs();

        int rc = m_pCam->InitCamera(FALSE);
        if (rc != CAM_SUCCESS)
        {
            FLogT("init-fail", "TryStart: InitCamera -> %d (CAM_SUCCESS=%d)", rc, CAM_SUCCESS);
            delete m_pCam; m_pCam = NULL;
            return false;
        }
        DWORD t2 = TickMs();
        // Still run on the bring-up thread rather than inside Active(): even
        // with the feature walk skipped this is a hardware round trip (and
        // the camera needs ~1 s after the stream starts before frame 0), so
        // the host's Pause()/Run() must not be made to wait for it.
        FLog("TryStart: InitCamera OK, MaxSpeed=%d (enum=%lums init=%lums)",
             m_pCam->GetMaxSpeed(), (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
        ConfigureVideo();
        DWORD t3 = TickMs();
        FLog("TryStart: ConfigureVideo done (%lums) -> %ux%u @ rate %d",
             (unsigned long)(t3 - t2), m_width, m_height, m_rateIndex);
        m_bInit = true;
    }

    // 8 buffers and a 2 s frame timeout -- the exact configuration the
    // standalone CMU diagnostic (isight-diag.exe) uses to pull 90 frames off
    // this camera without a single timeout.  StartImageAcquisition() would
    // use the library defaults (6 / 1000 ms) and the iSight needs a good
    // second after the stream starts before frame 0 appears.
    int rc = m_pCam->StartImageAcquisitionEx(8, 2000, ACQ_START_VIDEO_STREAM);
    if (rc != CAM_SUCCESS)
    {
        // -1 is what the driver returns when another process already holds
        // the camera.  Either way the handle is suspect now, so drop it: the
        // next attempt (1 s later, see FillBuffer) starts from a clean one
        // and re-enumerates the bus.
        FLogT("start-acq-fail", "TryStart: StartImageAcquisition -> %d%s, dropping the handle",
              rc, (rc == -1) ? " (camera held by another process?)" : "");
        delete m_pCam;
        m_pCam = NULL;
        m_bInit = false;
        return false;
    }

    FLog("TryStart: acquisition started (%ux%u @ %d, attempt %d, IsAcquiring=%d, maxSpeed=%d)",
         m_width, m_height, m_rateIndex, m_bringUpTries + 1,
         m_pCam->IsAcquiring() ? 1 : 0, m_pCam->GetMaxSpeed());
    m_bAcquiring = true;
    // v16: the microphone rides this same isochronous stream -- tell the
    // camera's audio unit to start now that the video engine is running.
    AudioStart();
    m_consecFail = 0;
    m_bringUpTries = 0;
    InterlockedExchange(&m_acqStartMs, (LONG)TickMs());
    return true;
}

void CiSightStream::ReleaseCamera()
{
    // the bus monitor must be gone before the camera handle is dropped
    StopBusMonitor();

    // never pull the camera object out from under the bring-up thread
    PVOID h = InterlockedExchangePointer(&m_hBringUp, NULL);
    if (h)
    {
        WaitForSingleObject((HANDLE)h, 30000);
        CloseHandle((HANDLE)h);
    }

    CAutoLock lock(&m_csCamera);
    if (m_pCam)
    {
        if (m_bAcquiring)
        {
            AudioStop();                    // v16: silence the mic first
            m_pCam->StopImageAcquisition();
            m_bAcquiring = false;
        }
        delete m_pCam;      // closes the 1394Camera.sys handle
        m_pCam = NULL;
        FLog("ReleaseCamera: handle closed after %lu frames", m_frameCount);
    }
    m_bInit = false;
    m_consecFail = 0;
    m_bringUpTries = 0;
    m_nextTryMs = 0;
    m_retryNotBefore = 0;
    InterlockedExchange(&m_bringUpState, 0);   // allow a fresh bring-up later
    InterlockedExchange(&m_busResetSeen, 0);
    InterlockedExchange(&m_camGoneSeen, 0);
    InterlockedExchange(&m_needReset, 0);
    InterlockedExchange(&m_acqStartMs, 0);
}

//---------------------------------------------------------------------
// v16 -- the microphone.
//
// The iSight's mic is not an AV/C subunit and not 61883-6 audio: it is a
// second unit directory on the same 1394 node (spec 0x000A27 / ver
// 0x000010) whose eleven registers sit at 0xFFFFF0020000, and it streams a
// private packet format -- a 16-byte header whose second quadlet is the
// ASCII "sght", then sample_count * 2 channels of 48 kHz / 16-bit big-
// endian PCM.  Linux calls it SW_ISIGHT_AUDIO (sound/firewire/isight.c).
//
// The important measurement (diags/audio.cpp, v9-v12) is that the audio
// unit does NOT get its own isochronous channel: it transmits on the video
// engine's channel, interleaved with the video packets.  That is why the
// microphone needs no second stream -- and it also means the CMU driver
// allows it, because we are not opening one.  The filter already holds the
// only isoch stream the device will give us, so the audio packets are
// simply picked out of the capture buffer we are already reading.
//
// Order is Linux's: SAMPLE_RATE -> ISO_TX_CONFIG -> AUDIO_ENABLE.
//---------------------------------------------------------------------
#define AUDIO_BASE        0xF0020000UL   // absolute driver offset
#define AUDIO_ENABLE      0x000
#define AUDIO_TX_CONFIG   0x300          // low 16 = channel, high 16 = speed
#define AUDIO_SAMPLE_RATE 0x400
#define AUDIO_GAIN        0x500
#define AUDIO_RATE_48000  0x80000000u
#define AUDIO_MAX_FRAMES  475            // the camera's per-packet limit
#define AUDIO_HEADER      16

// DCAM 0x60C packs the channel differently in 1394a and 1394b mode
static ULONG AudioChannelOf60C(ULONG v)
{
    if (v & 0x00008000u) return (v >> 8) & 0x3F;
    return (v >> 28) & 0x0F;
}

static int AudioSpeedIndex(ULONG flag)
{
    if (flag & SPEED_FLAGS_400) return 2;
    if (flag & SPEED_FLAGS_200) return 1;
    return 0;
}

static inline ULONG RdBE32(const BYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static void WavHeader(BYTE *h, ULONG dataBytes)
{
    const ULONG rate = 48000, ch = 2, bits = 16;
    ULONG frame = ch * bits / 8;                 // 4
    ULONG bytesPerSec = rate * frame;
    memcpy(h,      "RIFF", 4);
    h[4]  = (BYTE)((36 + dataBytes) & 0xFF);
    h[5]  = (BYTE)(((36 + dataBytes) >> 8) & 0xFF);
    h[6]  = (BYTE)(((36 + dataBytes) >> 16) & 0xFF);
    h[7]  = (BYTE)(((36 + dataBytes) >> 24) & 0xFF);
    memcpy(h + 8,  "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;       // fmt chunk size
    h[20] = 1;  h[21] = 0;                       // PCM
    h[22] = (BYTE)ch; h[23] = 0;
    h[24] = (BYTE)(rate & 0xFF);
    h[25] = (BYTE)((rate >> 8) & 0xFF);
    h[26] = (BYTE)((rate >> 16) & 0xFF);
    h[27] = (BYTE)((rate >> 24) & 0xFF);
    h[28] = (BYTE)(bytesPerSec & 0xFF);
    h[29] = (BYTE)((bytesPerSec >> 8) & 0xFF);
    h[30] = (BYTE)((bytesPerSec >> 16) & 0xFF);
    h[31] = (BYTE)((bytesPerSec >> 24) & 0xFF);
    h[32] = (BYTE)(frame & 0xFF); h[33] = (BYTE)((frame >> 8) & 0xFF);
    h[34] = (BYTE)bits; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (BYTE)(dataBytes & 0xFF);
    h[41] = (BYTE)((dataBytes >> 8) & 0xFF);
    h[42] = (BYTE)((dataBytes >> 16) & 0xFF);
    h[43] = (BYTE)((dataBytes >> 24) & 0xFF);
}

void CiSightStream::AudioStart()
{
    m_audioOn = m_audioSeen = false;
    m_audioFile = NULL;
    m_audioPackets = m_audioFrames = m_audioLost = m_audioBad = 0;
    m_audioFramesScanned = 0;
    m_audioExpectValid = false;
    m_audioExpect = 0;
    m_audioPeak = 0;
    m_audioBytes = 0;
    m_audioPath[0] = '\0';
    m_audioStripPackets = m_audioStripFrames = m_audioStripBytes = 0;
    m_audioTailBytes = 0;

    const ISightOptions &o = Opts();
    if (!o.audioOn || m_pCam == NULL)
        return;

    // which channel the video engine is on: that is where the audio goes too
    ULONG iso = 0, ch = 0;
    if (m_pCam->ReadQuadlet(0x60C, &iso) == CAM_SUCCESS)
        ch = AudioChannelOf60C(iso);
    int  spd = AudioSpeedIndex((ULONG)m_pCam->GetMaxSpeed());
    ULONG txv = (ch & 0x3F) | ((ULONG)spd << 16);

    int r1 = m_pCam->WriteQuadlet(AUDIO_BASE + AUDIO_SAMPLE_RATE, AUDIO_RATE_48000);
    int r2 = m_pCam->WriteQuadlet(AUDIO_BASE + AUDIO_TX_CONFIG,   txv);
    int r3 = 0;
    if (o.audioGain > 0)
        r3 = m_pCam->WriteQuadlet(AUDIO_BASE + AUDIO_GAIN, (ULONG)o.audioGain);
    int r4 = m_pCam->WriteQuadlet(AUDIO_BASE + AUDIO_ENABLE, 0x80000000u);

    FLog("audio: unit start -- channel=%lu speed=%d (0x60C=0x%08lX) "
         "rate=%d tx=%d gain=%d enable=%d",
         (unsigned long)ch, spd, (unsigned long)iso, r1, r2, r3, r4);
    if (r4 != CAM_SUCCESS)
    {
        FLog("audio: the unit did not start (enable -> %d) -- video only", r4);
        return;
    }
    m_audioOn = true;
    m_audioStartMs = TickMs();

    if (!o.audioWav)
        return;
    char dir[MAX_PATH] = "";
    if (GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH) == 0)
        GetTempPathA(MAX_PATH, dir);
    _snprintf_s(m_audioPath, sizeof(m_audioPath), _TRUNCATE,
                "%s\\iSightAudio.wav", dir[0] ? dir : ".");
    m_audioFile = fopen(m_audioPath, "wb");
    if (m_audioFile == NULL)
    {
        FLog("audio: cannot open %s -- PCM capture off", m_audioPath);
        return;
    }
    BYTE hdr[44];
    WavHeader(hdr, 0);
    fwrite(hdr, 1, 44, m_audioFile);
    fflush(m_audioFile);
    FLog("audio: writing 48 kHz / 16 bit stereo PCM to %s", m_audioPath);
}

void CiSightStream::AudioStop()
{
    const bool wasOn = m_audioOn;
    m_audioOn = false;
    if (m_pCam != NULL && wasOn)
    {
        m_pCam->WriteQuadlet(AUDIO_BASE + AUDIO_ENABLE, 0);
        FLog("audio: unit stopped");
    }
    if (m_audioFile != NULL)
    {
        BYTE hdr[44];
        WavHeader(hdr, (ULONG)m_audioBytes);
        fseek(m_audioFile, 0, SEEK_SET);
        fwrite(hdr, 1, 44, m_audioFile);
        fclose(m_audioFile);
        m_audioFile = NULL;
        FLog("audio: %s closed (%ld bytes of PCM)", m_audioPath, (long)m_audioBytes);
    }
}

// Called right after a successful AcquireImageEx(): the frame the video will
// be built from is also the buffer the camera put its audio packets in.
// One pass over the buffer does two things:
//   1. every valid "sght" packet is decoded into PCM (stats + optional wav);
//   2. with [audio] strip=1 the packet is cut out again.  The 1394 driver
//      splices audio into the picture as if it were video bytes -- measured
//      in v16 as ~14 packets of 816 bytes per frame: ten horizontal seams
//      and an 11 KB rainbow band at the bottom of the picture.  After
//      compaction the front of the buffer is a clean UYVY frame and the
//      video converters see video bytes only.
void CiSightStream::AudioScanFrame()
{
    if (!m_audioOn || m_pCam == NULL)
        return;
    const ISightOptions &o = Opts();
    BYTE *p = NULL;
    ULONG cb = 0;
    if (m_pCam->GetRawFrameBuffer(&p, &cb) != CAM_SUCCESS || p == NULL || cb < 32)
        return;

    const bool  strip = o.audioStrip != 0;
    const ULONG want  = m_width * m_height * 2;    // UYVY bytes of a full frame
    ULONG keep = 0, w = 0;   // [keep,..) unexamined; [0,w) already compacted

    InterlockedIncrement(&m_audioFramesScanned);
    const ULONG limit = (cb > AUDIO_HEADER + 8) ? (cb - 8) : 0;
    for (ULONG i = 0; i + 8 <= limit; ++i)
    {
        if (p[i] != 0x73 || p[i+1] != 0x67 || p[i+2] != 0x68 || p[i+3] != 0x74)
            continue;
        if (i < 4) continue;
        ULONG start = i - 4;                       // sample_count
        ULONG count = RdBE32(p + start);
        ULONG total = RdBE32(p + start + 8);       // running frame counter
        if (count == 0 || count > AUDIO_MAX_FRAMES)
        {
            ++m_audioBad;                          // a stray signature: video
            continue;                              // bytes, leave them alone
        }
        ULONG body = start + AUDIO_HEADER;
        ULONG need = count * 4;                    // 2 channels * 16 bit
        bool  cut  = false;
        if (body + need > cb)
        {
            // the packet straddles the end of this capture buffer; take
            // what is here (the rest is past the frame, dropped)
            need = (cb > body) ? ((cb - body) & ~3UL) : 0;
            cut = true;
        }
        if (need < 4) { ++m_audioBad; continue; }
        ULONG end = body + need;
        if (end > cb) end = cb;

        if (m_audioExpectValid && total != m_audioExpect)
        {
            LONG gap = (LONG)((long long)total - (long long)m_audioExpect);
            if (gap > 0) m_audioLost += gap;
        }
        m_audioExpect = total + count;
        m_audioExpectValid = true;

        ++m_audioPackets;
        m_audioFrames += (LONG)count;
        m_audioSeen = true;

        // S16_BE -> S16_LE, and the loudest sample along the way
        BYTE  conv[2048];
        ULONG todo = need;
        const BYTE *src = p + body;
        while (todo > 0)
        {
            ULONG n = (todo > sizeof(conv)) ? sizeof(conv) : todo;
            for (ULONG k = 0; k + 1 < n; k += 2)
            {
                conv[k]     = src[k + 1];
                conv[k + 1] = src[k];
                SHORT v = (SHORT)((src[k] << 8) | src[k + 1]);
                LONG  a = v < 0 ? -v : v;
                if (a > m_audioPeak) m_audioPeak = a;
            }
            if (m_audioFile != NULL && (o.audioMaxMB == 0 ||
                m_audioBytes < (LONG)o.audioMaxMB * 1024L * 1024L))
            {
                fwrite(conv, 1, n, m_audioFile);
                m_audioBytes += (LONG)n;
            }
            src  += n;
            todo -= n;
        }

        if (o.audioLog >= 2)
            FLog("audio: packet #%ld at +%lu of %lu: %lu frames, total=%lu%s",
                 (long)m_audioPackets, (unsigned long)start, (unsigned long)cb,
                 (unsigned long)count, (unsigned long)total, cut ? " (cut)" : "");

        if (strip && !cut && start >= keep)
        {
            // cut [start, end) out: move the video bytes seen since the
            // last cut down over it.  w <= keep <= start always holds, so
            // source and destination never overlap the packet itself.
            if (start > keep) { memmove(p + w, p + keep, start - keep); w += start - keep; }
            m_audioStripBytes += (LONG)(end - start);
            ++m_audioStripPackets;
            keep = end;
            i = end - 1;                           // resume after this packet
        }
        else
        {
            i = body + need - 1;                   // skip this packet's body
        }
    }

    if (strip)
    {
        // move the video tail down over the (already cut) holes
        if (cb > keep) { memmove(p + w, p + keep, cb - keep); w += cb - keep; }
        ++m_audioStripFrames;
        m_audioTailBytes = (LONG)w;
        if (m_audioStripFrames <= 3 || (m_audioStripFrames % 300) == 0)
            FLog("audio: strip frame #%ld: %ld packets / %ld bytes cut, "
                 "%ld bytes of frame data left (a full frame is %lu)",
                 (long)m_audioStripFrames, (long)m_audioStripPackets,
                 (long)m_audioStripBytes, (long)m_audioTailBytes,
                 (unsigned long)want);
    }

    if (o.audioLog >= 1 && m_audioPackets > 0 &&
        (m_audioPackets % 100) == 0 && (m_audioPackets / 100) <= 100)
    {
        DWORD ms = TickMs() - m_audioStartMs;
        double secs = ms / 1000.0;
        double got  = m_audioFrames / 48000.0;
        FLog("audio: %ld packets, %ld frames (%.1f s of audio in %.1f s of video), "
             "lost %ld frames (%.1f%%), peak %ld, %ld video frames scanned, %ld bad",
             (long)m_audioPackets, (long)m_audioFrames, got, secs,
             (long)m_audioLost,
             (m_audioFrames + m_audioLost) > 0
                 ? (100.0 * m_audioLost / (double)(m_audioFrames + m_audioLost)) : 0.0,
             (long)m_audioPeak, (long)m_audioFramesScanned, (long)m_audioBad);
    }
}

// Must end up with Format 0 / Mode 2 (640x480 YUV422). The iSight also
// advertises smaller modes, and the naive "first supported mode wins"
// scan picks Mode 1 (320x240) -- the driver then streams 320x240 while
// the media type we advertise (and the direct-show buffer we hand out)
// is 640x480, so every AcquireImageEx fails. So: verify the size after
// every candidate and only accept an exact 640x480.
// Rate is capped at 15 fps: the iSight is an S100 (100 Mbit/s) device
// and 640x480 YUV422 @ 30 fps (~150 Mbit/s) does not fit on the bus.
void CiSightStream::ConfigureVideo()
{
    static const struct { unsigned long fmt, mode; } kCandidates[] =
    {
        { 0, 2 },      // Format 0 / Mode 2 -> 640x480  (what we want)
        { 1, 0 },      // Format 1 / Mode 0 -> 800x600  (never on an iSight)
        { 0, 1 }       // last resort: 320x240 (will be letterboxed)
    };

    bool ok = false;
    unsigned long w = 0, h = 0;
    int bestRate = kMaxRateIndex;

    for (int ci = 0; ci < (int)_countof(kCandidates) && !ok; ++ci)
    {
        unsigned long f = kCandidates[ci].fmt;
        unsigned long m = kCandidates[ci].mode;

        if (!m_pCam->HasVideoFormat(f))
            continue;
        if (!m_pCam->HasVideoMode(f, m))
            continue;

        for (int r = kMaxRateIndex; r >= 0 && !ok; --r)
        {
            if (!m_pCam->HasVideoFrameRate(f, m, (unsigned long)r))
                continue;

            if (m_pCam->SetVideoFormat(f) != CAM_SUCCESS) continue;
            if (m_pCam->SetVideoMode(m) != CAM_SUCCESS)   continue;
            if (m_pCam->SetVideoFrameRate((unsigned long)r) != CAM_SUCCESS) continue;

            m_pCam->UpdateParameters(TRUE);
            w = h = 0;
            m_pCam->GetVideoFrameDimensions(&w, &h);

            if (w == ISIGHT_WIDTH && h == ISIGHT_HEIGHT)
            {
                m_rateIndex = r;
                m_width = w;
                m_height = h;
                ok = true;
                FLog("ConfigureVideo: format=%lu mode=%lu rate=%d -> %lux%lu", f, m, r, w, h);
            }
            else
            {
                FLog("ConfigureVideo: format=%lu mode=%lu rate=%d gave %lux%lu, next",
                     f, m, r, w, h);
                if (bestRate > r)
                    bestRate = r;
            }
        }
    }

    if (!ok)
    {
        // nothing gave us 640x480: fall back to mode 2 at the slowest
        // usable rate and let FillBuffer letterbox whatever arrives.
        m_rateIndex = bestRate;
        if (m_pCam->HasVideoFormat(0) && m_pCam->HasVideoMode(0, 2))
        {
            m_pCam->SetVideoFormat(0);
            m_pCam->SetVideoMode(2);
            m_pCam->SetVideoFrameRate((unsigned long)m_rateIndex);
            m_pCam->UpdateParameters(TRUE);
            w = h = 0;
            m_pCam->GetVideoFrameDimensions(&w, &h);
        }
        FLog("ConfigureVideo: NO 640x480 mode found, falling back to %lux%lu @ rate %d",
             w, h, m_rateIndex);
        if (w && h)
        {
            m_width = w;
            m_height = h;
        }
    }
    m_pCam->UpdateParameters(TRUE);
}

HRESULT CiSightStream::Active()
{
    CAutoLock lock(m_pFilter->pStateLock());
    m_bFirstFrame = true;
    m_frameCount = 0;
    m_rtNext = 0;
    InterlockedExchange(&m_fbCalls, 0);
    InterlockedExchange(&m_acqCalls, 0);
    InterlockedExchange(&m_acqBlockedMs, 0);
    m_rejectStreak = 0;
    InterlockedExchange(&m_acqStartMs, 0);
    HRESULT hr = CSourceStream::Active();
    if (FAILED(hr))
    {
        FLog("Active: CSourceStream::Active -> 0x%08X", (unsigned)hr);
        return hr;
    }
    FLog("Active: pin active");
    FLog("Active: connected=%d subtype=%s orient=%s",
         IsConnected() ? 1 : 0,
         SubTypeName(&m_mt.subtype),
         OrientName(OrientForSubType(&m_mt.subtype)));
    // DO NOT touch the camera here. Camera bring-up costs ~15 s (the CMU
    // library reads the whole capability table) and Active() runs inside
    // the host's Pause()/Run() call: blocking here makes every application
    // -- WeChat, QQ, OBS -- give up with "cannot open camera". We just
    // start it on its own thread and hand out black frames until it is up.
    if (IsConnected())
    {
        m_bringUpTries   = 0;
        m_nextTryMs      = 0;      // first attempt right away
        m_retryNotBefore = 0;
        KickBusMonitor();          // notice a power cycle within ~200 ms
        KickBringUp();
    }
    return S_OK;
}

HRESULT CiSightStream::Inactive()
{
    CAutoLock lock(m_pFilter->pStateLock());
    HRESULT hr = CSourceStream::Inactive();  // stops the worker thread first
    ReleaseCamera();                          // host may reopen us later
    FLog("Inactive: pin stopped");
    return hr;
}

// how often the streaming thread may start another bring-up attempt.  The
// attempt itself runs on its own thread, so this is only a cadence limit:
// fast enough to pick the camera up as soon as it has booted, slow enough
// not to hammer a camera that is switched off.
static const DWORD kBringUpRetryMs = 1000;

//---------------------------------------------------------------------
// v14: our own copy of the base class buffer loop.
//
// The stock CSourceStream::DoBufferProcessingLoop() (Win7Samples base
// classes) stops for good in two situations that must not be fatal:
//
//   * Deliver() returned anything but S_OK -- a renderer that is still
//     transitioning, or one that is momentarily rejecting samples, ends
//     the loop.  The worker thread then parks on its request queue and
//     FillBuffer() is never called again: the graph looks connected, the
//     host shows a frozen (or black) picture, and only the host can
//     recover by tearing the graph down and building it again.
//   * FillBuffer() returned S_FALSE -- same effect, plus EndOfStream.
//
// Measured on this machine: a graph built against the stock NullRenderer
// stops after exactly one sample (FillBuffer entry count freezes at 1,
// zero frames, the renderer reports EC_COMPLETE while the graph is still
// Running) -- the same "connected but 0 frames" signature that QQ.exe
// left in the log on the previous build.
//
// So: log everything, and treat a rejected delivery as transient.  After
// kMaxRejects consecutive rejections we let the stock behaviour through,
// because at that point the downstream really is gone.
//---------------------------------------------------------------------
#define ISIGHT_MAX_REJECTS 120          // ~1.2 s of retries, 10 ms apart

HRESULT CiSightStream::DoBufferProcessingLoop(void)
{
    Command com;

    InterlockedIncrement(&m_loopRuns);
    FLog("stream: buffer loop #%ld enter (fbCalls=%ld frames=%lu)",
         (long)m_loopRuns, (long)m_fbCalls, m_frameCount);

    OnThreadStartPlay();

    do {
        while (!CheckRequest(&com))
        {
            IMediaSample *pSample = NULL;

            HRESULT hr = GetDeliveryBuffer(&pSample, NULL, NULL, 0);
            if (FAILED(hr))
            {
                // the stock loop spins here as well: the allocator may be
                // decommitted or still busy, either way it is not fatal
                if (m_rejectStreak <= 3)
                    FLog("stream: GetDeliveryBuffer -> 0x%08X (waiting)", (unsigned)hr);
                Sleep(1);
                continue;
            }

            hr = FillBuffer(pSample);

            if (hr == S_OK)
            {
                hr = Deliver(pSample);
                pSample->Release();

                if (hr != S_OK)
                {
                    ++m_rejectStreak;
                    if (m_rejectStreak <= 3 || (m_rejectStreak % 25) == 0)
                        FLog("stream: downstream rejected sample -> 0x%08X (streak %ld) -- retrying",
                             (unsigned)hr, m_rejectStreak);

                    if (m_rejectStreak > ISIGHT_MAX_REJECTS)
                    {
                        FLog("stream: %ld rejected samples in a row -> ending the stream (0x%08X)",
                             m_rejectStreak, (unsigned)hr);
                        return S_OK;            // same as the stock loop
                    }
                    Sleep(10);                  // do not spin on a busy renderer
                    continue;
                }
                m_rejectStreak = 0;
            }
            else if (hr == S_FALSE)
            {
                pSample->Release();
                FLog("stream: FillBuffer asked to stop (S_FALSE) -> EndOfStream");
                DeliverEndOfStream();
                return S_OK;
            }
            else
            {
                pSample->Release();
                FLog("stream: FillBuffer failed 0x%08X -> EndOfStream + EC_ERRORABORT", (unsigned)hr);
                DeliverEndOfStream();
                m_pFilter->NotifyEvent(EC_ERRORABORT, hr, 0);
                return hr;
            }
        }

        if (com == CMD_RUN || com == CMD_PAUSE)
            Reply(NOERROR);
        else if (com != CMD_STOP)
        {
            Reply((DWORD) E_UNEXPECTED);
            FLog("stream: unexpected command %d", (int)com);
        }
    } while (com != CMD_STOP);

    FLog("stream: buffer loop #%ld exit (CMD_STOP, fbCalls=%ld frames=%lu)",
         (long)m_loopRuns, (long)m_fbCalls, m_frameCount);
    return S_FALSE;
}

HRESULT CiSightStream::FillBuffer(IMediaSample *pSample)
{
    CheckPointer(pSample, E_POINTER);

    // v12 probe: prove that DirectShow still calls us, and with which state.
    {
        LONG fb = InterlockedIncrement(&m_fbCalls);
        if (fb <= 6 || (fb % 25) == 0)
            FLog("probe: FillBuffer entry #%ld (acquiring=%d cam=%p init=%d frames=%lu state=%ld)",
                 fb, m_bAcquiring ? 1 : 0, (void *)m_pCam, m_bInit ? 1 : 0,
                 m_frameCount, (long)InterlockedCompareExchange(&m_bringUpState, 0, 0));
    }

    // v11: pick up edits to iSightCam.ini (orientation, layout, guide) so the
    // picture can be reshaped while the call is up.  Cheap: the ini is only
    // stat()ed twice a second.
    MaybeReloadTunables();

    // the connected type decides the layout we must produce
    SubTypeInfo si = SubTypeFor(&m_mt.subtype);
    ULONG need = FrameBytesFor(si);
    PBYTE pBuf = NULL;
    if (FAILED(pSample->GetPointer(&pBuf)) || pBuf == NULL)
        return E_FAIL;

    // A bus reset (the camera switched off and on again, or anything else
    // joining / leaving the bus) invalidates the stream instantly: the node
    // address changed and the handle we hold is stale.  The bus monitor
    // reports that within ~200 ms, far sooner than any acquire can time out,
    // so the teardown happens here -- on the streaming thread and never
    // while the camera lock below is held.
    if (InterlockedExchange(&m_busResetSeen, 0) || InterlockedExchange(&m_camGoneSeen, 0))
        DropStreamForRecovery("bus reset / camera power cycle");

    bool got = false;

    // v13 self-heal.  The stream is marked running but no frame has arrived
    // for four seconds: whatever the driver is doing, that stream is not
    // coming back on its own (measured: the camera streams fine from the
    // standalone CMU tool at the same moment the filter gets nothing).
    // Invalidate the stream instead of waiting -- TryStart() then drops the
    // stale handle under the camera lock and brings the camera up again.
    if (m_bAcquiring && m_frameCount == 0)
    {
        LONG started = InterlockedCompareExchange(&m_acqStartMs, 0, 0);
        if (started != 0 && (LONG)(TickMs() - (DWORD)started) > 4000)
        {
            FLog("recovery: no frame in 4 s while acquiring -> dropping the stream");
            DropStreamForRecovery("no frame after a successful start");
        }
    }

    if (!m_bAcquiring)
    {
        // Camera not live yet: keep the graph fed with black frames and keep
        // one bring-up attempt in flight.  Never block this thread on the
        // hardware init, and never hammer a camera that is switched off --
        // one attempt per second is plenty.
        DWORD now = TickMs();
        LONG  st  = InterlockedCompareExchange(&m_bringUpState, 0, 0);
        bool  armed = ((LONG)(now - m_nextTryMs) >= 0) &&
                      ((LONG)(now - m_retryNotBefore) >= 0);
        if (st != 1 && armed)
        {
            m_nextTryMs = now + kBringUpRetryMs;
            if (st == 2)                    // finished, but the stream went away
                InterlockedExchange(&m_bringUpState, 0);
            ++m_bringUpTries;
            if (m_bringUpTries <= 3 || (m_bringUpTries % 20) == 0)
                FLog("FillBuffer: camera bring-up attempt #%d (state=%ld)",
                     m_bringUpTries, st);
            KickBringUp();
        }
    }
    if (m_bAcquiring && m_pCam != NULL)
    {
        CAutoLock lock(&m_csCamera);
        int dropped = 0;
        DWORD ta = TickMs();
        InterlockedExchange(&m_lastAcqEnterMs, (LONG)ta);
        int rc = m_pCam->AcquireImageEx(TRUE, &dropped);
        DWORD dt = TickMs() - ta;
        InterlockedExchange(&m_acqBlockedMs, (LONG)dt);
        LONG nc = InterlockedIncrement(&m_acqCalls);
        if (nc <= 25 || (dt > 1500 && (nc % 20) == 0))
            FLog("probe: AcquireImageEx #%ld -> %d in %lums (w=%lu h=%lu acquiring=%d frames=%lu)",
                 nc, rc, (unsigned long)dt, m_width, m_height,
                 m_bAcquiring ? 1 : 0, m_frameCount);

        if (rc == CAM_SUCCESS)
        {
            // v16: the camera interleaves its microphone packets with the
            // video in this very buffer, so pick them out before the frame
            // is converted.
            AudioScanFrame();

            // size the scratch buffer from what the camera actually streams
            ULONG frameBytes = m_width * m_height * 3;
            if (m_pScratch == NULL || m_scratchBytes < frameBytes)
            {
                if (m_pScratch) { delete[] m_pScratch; m_pScratch = NULL; }
                m_pScratch = new BYTE[frameBytes];
                m_scratchBytes = m_pScratch ? frameBytes : 0;
            }
            if (m_pScratch &&
                m_pCam->getDIB(m_pScratch, m_scratchBytes) == CAM_SUCCESS)
            {
                const BYTE *src = m_pScratch;
                if (m_width != ISIGHT_WIDTH || m_height != ISIGHT_HEIGHT)
                {
                    // the driver gave us a smaller mode than the 640x480 we
                    // advertise: letterbox into a black full-size frame so
                    // the downstream buffer is always the agreed size
                    if (m_pFull == NULL)
                    {
                        m_pFull = new BYTE[ISIGHT_DIB_BYTES];
                        if (m_pFull) ZeroMemory(m_pFull, ISIGHT_DIB_BYTES);
                    }
                    if (m_pFull)
                    {
                        ULONG cw = (m_width  < ISIGHT_WIDTH)  ? m_width  : ISIGHT_WIDTH;
                        ULONG ch = (m_height < ISIGHT_HEIGHT) ? m_height : ISIGHT_HEIGHT;
                        for (ULONG y = 0; y < ch; ++y)
                            memcpy(m_pFull + (size_t)y * ISIGHT_WIDTH * 3,
                                   m_pScratch + (size_t)y * m_width * 3,
                                   (size_t)cw * 3);
                        src = m_pFull;
                    }
                }

                // v10-v12: if this host cuts the picture down to its own
                // window shape, hand it a frame whose content already sits
                // inside that shape (see the [layout] notes at the top of
                // this file).  The box applies per host, and the composite
                // buffer is cleared every frame so live edits leave no
                // leftovers.
                int lw = 0, lh = 0;
                LayoutTarget(&lw, &lh);
                const ISightOptions &opt = Opts();
                if (lw > 0 && lh > 0)
                {
                    if (m_pFit == NULL)
                        m_pFit = new BYTE[ISIGHT_DIB_BYTES];
                    if (m_pBlur == NULL)
                        m_pBlur = new BYTE[kBlurScratchBytes];
                    if (m_pFit)
                    {
                        ZeroMemory(m_pFit, ISIGHT_DIB_BYTES);   // black surround
                        ComposeBox(src, ISIGHT_WIDTH, ISIGHT_HEIGHT,
                                   m_pFit, ISIGHT_WIDTH, ISIGHT_HEIGHT,
                                   (ULONG)lw, (ULONG)lh, opt.layoutMode,
                                   m_pBlur, kBlurScratchBytes,
                                   opt.blurCells, opt.blurShade);
                        src = m_pFit;
                    }
                }

                // v11/v12: the marker goes on last, so it is visible both in
                // the host's window and in the dumped frame.
                if (opt.guide)
                    DrawGuide((PBYTE)src, ISIGHT_WIDTH, ISIGHT_HEIGHT, lw, lh, opt.guide);

                int orient = OrientForSubType(si.subtype);
                EmitFrame(src, pBuf, ISIGHT_WIDTH, ISIGHT_HEIGHT, orient, si);

                if (Opts().dump && m_dumpCount < 2)
                    DumpFrame(pBuf, si, ISIGHT_WIDTH, ISIGHT_HEIGHT, orient, ++m_dumpCount);

                got = true;
                m_consecFail = 0;
                m_frameCount++;
                if (m_frameCount <= 3 || (m_frameCount % 150) == 0)
                {
                    const BYTE *s = (const BYTE *)((*si.subtype == MEDIASUBTYPE_YUY2) ? pBuf + 0 : pBuf);
                    FLog("FillBuffer #%lu ok (%s, %lu bytes, first=%02X %02X %02X %02X, dropped=%d)",
                         m_frameCount, SubTypeName(si.subtype), (unsigned long)need,
                         s[0], s[1], s[2], s[3], dropped);
                }
            }
            else
            {
                FLog("FillBuffer: getDIB failed (scratch=%p)", m_pScratch);
            }
        }
        else
        {
            m_consecFail++;
            if (m_consecFail <= 3 || (m_consecFail % 60) == 0)
                FLog("FillBuffer: AcquireImageEx -> %d (fail streak %d, %lux%lu @ rate %d)",
                     rc, m_consecFail, m_width, m_height, m_rateIndex);
            if (m_consecFail >= 3)
            {
                // Three failed grabs in a row (2 s timeout each) means the
                // stream is really gone.  This is also the fallback path when
                // the bus monitor could not open its own device handle.  We
                // already hold the camera lock here, so tear everything down
                // inline and let the bring-up worker start a clean stream.
                FLog("FillBuffer: %d consecutive grab failures -> dropping the camera handle",
                     m_consecFail);
                if (m_bAcquiring)
                {
                    m_pCam->StopImageAcquisition();
                    m_bAcquiring = false;
                }
                delete m_pCam;          // stale after a reset, see the v9 notes
                m_pCam = NULL;
                m_bInit = false;
                m_consecFail = 0;
                m_bringUpTries = 0;
                m_retryNotBefore = TickMs() + (DWORD)Opts().bootDelayMs;
                m_nextTryMs = m_retryNotBefore;
                InterlockedExchange(&m_bringUpState, 0);
            }
        }
    }

    if (!got)
    {
        // never stall the graph: hand out a black frame instead
        ZeroMemory(pBuf, pSample->GetSize());
        if (m_bFirstFrame)
            FLog("FillBuffer: delivering black frame (camera not ready)");
    }

    // timestamp: live source -> use stream time so renderers do not try
    // to catch up when the first frame takes a second to arrive.
    REFERENCE_TIME rtStart = m_rtNext;
    CRefTime rtStream;
    if (SUCCEEDED(m_pFilter->StreamTime(rtStream)))
        rtStart = (REFERENCE_TIME)rtStream;
    REFERENCE_TIME rtEnd = rtStart + kFrameDurations[m_rateIndex];
    m_rtNext = rtEnd;

    pSample->SetTime(&rtStart, &rtEnd);
    pSample->SetSyncPoint(TRUE);
    pSample->SetDiscontinuity(m_bFirstFrame);
    m_bFirstFrame = false;

    return S_OK;
}

HRESULT CiSightStream::DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pprop)
{
    CheckPointer(pAlloc, E_POINTER);
    CheckPointer(pprop, E_POINTER);

    CAutoLock lock(m_pFilter->pStateLock());

    SubTypeInfo si = SubTypeFor(&m_mt.subtype);
    pprop->cBuffers = 2;
    pprop->cbBuffer = FrameBytesFor(si);
    pprop->cbAlign  = 1;
    pprop->cbPrefix = 0;

    FLog("DecideBufferSize: %s -> %lu bytes", SubTypeName(si.subtype),
         (unsigned long)pprop->cbBuffer);

    ALLOCATOR_PROPERTIES actual;
    HRESULT hr = pAlloc->SetProperties(pprop, &actual);
    if (FAILED(hr))
    {
        FLog("DecideBufferSize: SetProperties -> 0x%08X", (unsigned)hr);
        return hr;
    }
    FLog("DecideBufferSize: negotiated %ld buffers x %ld bytes (asked %lu x %lu)",
         (long)actual.cBuffers, (long)actual.cbBuffer,
         (unsigned long)pprop->cBuffers, (unsigned long)pprop->cbBuffer);
    if (actual.cbBuffer < pprop->cbBuffer)
        return E_OUTOFMEMORY;
    return S_OK;
}

void CiSightStream::BuildMediaType(const GUID *subtype, REFERENCE_TIME interval, CMediaType *pmt) const
{
    SubTypeInfo si = SubTypeFor(subtype);

    pmt->InitMediaType();
    pmt->SetType(&MEDIATYPE_Video);
    pmt->SetSubtype(si.subtype);
    pmt->SetFormatType(&FORMAT_VideoInfo);
    pmt->SetTemporalCompression(FALSE);

    VIDEOINFOHEADER vih;
    ZeroMemory(&vih, sizeof(vih));
    vih.AvgTimePerFrame = interval;
    vih.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    vih.bmiHeader.biWidth       = ISIGHT_WIDTH;
    // The data we hand out is top-down when the orientation needs a
    // vertical flip (see Opts()), and getDIB's native bottom-up
    // otherwise.  Declaring the matching sign is what keeps a
    // conformant renderer and a sign-ignoring host both correct.
    vih.bmiHeader.biHeight      = OrientDeclareNegative(OrientForSubType(si.subtype))
                                    ? -ISIGHT_HEIGHT : ISIGHT_HEIGHT;
    vih.bmiHeader.biPlanes      = 1;
    vih.bmiHeader.biBitCount    = (WORD)si.bpp;
    vih.bmiHeader.biCompression = si.compression;
    vih.bmiHeader.biSizeImage   = FrameBytesFor(si);

    // v10: publish the real frame rectangle.  These were left zeroed before,
    // which is not what any sample source does and gives a host that derives
    // its own scaling from them an empty rect to work with.
    // (assigned field by field: SetRect() would pull in a user32 import)
    vih.rcSource.left   = 0;
    vih.rcSource.top    = 0;
    vih.rcSource.right  = ISIGHT_WIDTH;
    vih.rcSource.bottom = ISIGHT_HEIGHT;
    vih.rcTarget        = vih.rcSource;

    pmt->SetFormat((PBYTE)&vih, sizeof(vih));
    pmt->SetSampleSize(FrameBytesFor(si));
}

// accept anything that is 640x480 video in one of our three subtypes; the
// requested frame interval is honoured only as a hint (the camera runs at
// 15 fps on S100 no matter what the host asks for).
bool CiSightStream::MediaTypeCompatible(const CMediaType *pmt, CMediaType *pNormalized) const
{
    if (!pmt) return false;
    if (pmt->majortype != MEDIATYPE_Video) return false;
    if (pmt->formattype != FORMAT_VideoInfo && pmt->formattype != FORMAT_VideoInfo2) return false;
    if (pmt->cbFormat < sizeof(VIDEOINFOHEADER)) return false;
    // v10: only the subtypes [format] types= lists are accepted
    if (!SubTypeEnabled(&pmt->subtype)) return false;

    const VIDEOINFOHEADER *pvi = (const VIDEOINFOHEADER *)pmt->pbFormat;
    if (pvi->bmiHeader.biWidth != (LONG)ISIGHT_WIDTH) return false;
    if (labs(pvi->bmiHeader.biHeight) != (LONG)ISIGHT_HEIGHT) return false;

    SubTypeInfo si = SubTypeFor(&pmt->subtype);
    if (pvi->bmiHeader.biBitCount != (WORD)si.bpp) return false;
    if (pvi->bmiHeader.biCompression != si.compression &&
        pvi->bmiHeader.biCompression != BI_RGB &&
        pvi->bmiHeader.biCompression != 0) return false;

    if (pNormalized)
    {
        REFERENCE_TIME interval = pvi->AvgTimePerFrame;
        if (interval <= 0)
            interval = kFrameDurations[kMaxRateIndex];
        if (interval < kFrameDurations[kMaxRateIndex])       // faster than 15 fps: clamp
            interval = kFrameDurations[kMaxRateIndex];
        BuildMediaType(&pmt->subtype, interval, pNormalized);
    }
    return true;
}

HRESULT CiSightStream::GetMediaType(int iPosition, CMediaType *pmt)
{
    CAutoLock lock(m_pFilter->pStateLock());
    if (iPosition < 0)
        return E_INVALIDARG;

    // v10: the advertised list (and order) comes from [format] types=
    const ISightOptions &o = Opts();
    if (iPosition >= o.typeCount)
        return VFW_S_NO_MORE_ITEMS;

    const GUID *sub = kAllSubs[o.typeOrder[iPosition]];
    FLogT("enum-mt", "EnumMediaTypes: #%d -> %s", iPosition, SubTypeName(sub));
    BuildMediaType(sub, kFrameDurations[kMaxRateIndex], pmt);
    return S_OK;
}

HRESULT CiSightStream::CheckMediaType(const CMediaType *pmt)
{
    CAutoLock lock(m_pFilter->pStateLock());

    const VIDEOINFOHEADER *pvi = (pmt && pmt->cbFormat >= sizeof(VIDEOINFOHEADER))
                               ? (const VIDEOINFOHEADER *)pmt->pbFormat : NULL;

    if (!MediaTypeCompatible(pmt, NULL))
    {
        FLog("CheckMediaType: REJECT subtype=%s fmt=%s %ldx%ld bpp=%u comp=%08X",
             pmt ? SubTypeName(&pmt->subtype) : "null",
             pmt ? ((pmt->formattype == FORMAT_VideoInfo) ? "VideoInfo" :
                    (pmt->formattype == FORMAT_VideoInfo2) ? "VideoInfo2" : "other") : "-",
             pvi ? (long)pvi->bmiHeader.biWidth : -1,
             pvi ? (long)pvi->bmiHeader.biHeight : -1,
             pvi ? (unsigned)pvi->bmiHeader.biBitCount : 0,
             pvi ? (unsigned)pvi->bmiHeader.biCompression : 0);
        return E_FAIL;
    }

    FLog("CheckMediaType: accept %s %ldx%ld bpp=%u interval=%lld",
         SubTypeName(&pmt->subtype),
         pvi ? (long)pvi->bmiHeader.biWidth : -1,
         pvi ? (long)pvi->bmiHeader.biHeight : -1,
         pvi ? (unsigned)pvi->bmiHeader.biBitCount : 0,
         pvi ? (long long)pvi->AvgTimePerFrame : 0);
    return S_OK;
}

//---------------------------------------------------------------------
// IAMStreamConfig
//---------------------------------------------------------------------
STDMETHODIMP CiSightStream::GetFormat(AM_MEDIA_TYPE **ppmt)
{
    CheckPointer(ppmt, E_POINTER);
    CAutoLock lock(m_pFilter->pStateLock());

    const ISightOptions &o = Opts();
    CMediaType mt;
    if (MediaTypeCompatible(&m_mt, &mt))
    {
        FLog("GetFormat: -> current %s", SubTypeName(&mt.subtype));
        *ppmt = CreateMediaType(&mt);
    }
    else
    {
        const GUID *sub = kAllSubs[o.typeOrder[0]];
        FLog("GetFormat: no current format, -> default %s", SubTypeName(sub));
        BuildMediaType(sub, kFrameDurations[kMaxRateIndex], &mt);
        *ppmt = CreateMediaType(&mt);
    }
    return (*ppmt != NULL) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CiSightStream::SetFormat(AM_MEDIA_TYPE *pmt)
{
    CAutoLock lock(m_pFilter->pStateLock());

    // apps commonly pass NULL to reset the pin to its default format
    if (pmt == NULL)
    {
        FLog("SetFormat(NULL): reset to default -> S_OK");
        return S_OK;
    }

    CMediaType normalized;
    if (!MediaTypeCompatible((CMediaType *)pmt, &normalized))
    {
        const VIDEOINFOHEADER *pvi = (const VIDEOINFOHEADER *)pmt->pbFormat;
        FLog("SetFormat: REJECT subtype=%s fmt=%s %ldx%ld bpp=%u comp=%08X",
             SubTypeName(&pmt->subtype),
             (pmt->formattype == FORMAT_VideoInfo) ? "VideoInfo" :
             (pmt->formattype == FORMAT_VideoInfo2) ? "VideoInfo2" : "other",
             pvi ? (long)pvi->bmiHeader.biWidth : -1,
             pvi ? (long)pvi->bmiHeader.biHeight : -1,
             pvi ? (unsigned)pvi->bmiHeader.biBitCount : 0,
             pvi ? (unsigned)pvi->bmiHeader.biCompression : 0);
        return VFW_E_INVALIDMEDIATYPE;
    }

    const VIDEOINFOHEADER *pvi = (const VIDEOINFOHEADER *)pmt->pbFormat;
    FLog("SetFormat: %s %ldx%ld bpp=%u interval=%lld -> S_OK",
         SubTypeName(&pmt->subtype),
         (long)pvi->bmiHeader.biWidth, (long)pvi->bmiHeader.biHeight,
         (unsigned)pvi->bmiHeader.biBitCount, (long long)pvi->AvgTimePerFrame);
    return S_OK;
}

STDMETHODIMP CiSightStream::GetNumberOfCapabilities(int *piCount, int *piSize)
{
    CheckPointer(piCount, E_POINTER);
    CheckPointer(piSize, E_POINTER);
    *piCount = Opts().typeCount;
    *piSize  = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    FLogT("caps-count", "GetNumberOfCapabilities -> count=%d (picked by hosts that walk the caps)",
          *piCount);
    return S_OK;
}

STDMETHODIMP CiSightStream::GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC)
{
    CheckPointer(ppmt, E_POINTER);
    CheckPointer(pSCC, E_POINTER);

    const ISightOptions &o = Opts();
    if (iIndex < 0 || iIndex >= o.typeCount)
        return S_FALSE;

    CAutoLock lock(m_pFilter->pStateLock());

    const GUID *sub = kAllSubs[o.typeOrder[iIndex]];
    FLog("GetStreamCaps: #%d -> %s (the capability a host picks is the subtype it connects with)",
         iIndex, SubTypeName(sub));

    CMediaType mt;
    BuildMediaType(sub, kFrameDurations[kMaxRateIndex], &mt);
    *ppmt = CreateMediaType(&mt);
    if (*ppmt == NULL)
        return E_OUTOFMEMORY;

    ULONG bytes = FrameBytesFor(SubTypeFor(sub));

    VIDEO_STREAM_CONFIG_CAPS *caps = (VIDEO_STREAM_CONFIG_CAPS *)pSCC;
    ZeroMemory(caps, sizeof(*caps));
    caps->guid            = FORMAT_VideoInfo;
    caps->VideoStandard   = AnalogVideo_None;
    caps->InputSize.cx          = ISIGHT_WIDTH;
    caps->InputSize.cy          = ISIGHT_HEIGHT;
    caps->MinCroppingSize       = caps->InputSize;
    caps->MaxCroppingSize       = caps->InputSize;
    caps->MinOutputSize         = caps->InputSize;
    caps->MaxOutputSize         = caps->InputSize;
    caps->MinBitsPerSecond      = bytes * 8 * 2;
    caps->MaxBitsPerSecond      = bytes * 8 * 15;
    caps->MinFrameInterval      = kFrameDurations[kMaxRateIndex];   // fastest = 15 fps
    caps->MaxFrameInterval      = kFrameDurations[0];               // slowest = 1.875 fps
    return S_OK;
}

STDMETHODIMP CiSightStream::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    CheckPointer(ppv, E_POINTER);
    HRESULT hr;
    if (riid == __uuidof(IAMStreamConfig))
        hr = GetInterface((IAMStreamConfig *)this, ppv);
    else if (riid == __uuidof(IKsPropertySet))
        hr = GetInterface((IKsPropertySet *)this, ppv);
    else
        hr = CSourceStream::NonDelegatingQueryInterface(riid, ppv);
    char key[64];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "pin-qi-%s", GuidName(riid));
    FLogT(key, "pin QI %s -> %s", GuidName(riid), SUCCEEDED(hr) ? "OK" : "E_NOINTERFACE");
    return hr;
}

//---------------------------------------------------------------------
// IKsPropertySet : report the pin category (PIN_CATEGORY_CAPTURE).
//
// Without this, ICaptureGraphBuilder2::RenderStream() -- the call every
// real capture application makes -- cannot locate our output pin and
// fails with E_INVALIDARG before a single media type is negotiated.
//---------------------------------------------------------------------
STDMETHODIMP CiSightStream::Set(REFGUID guidPropSet, DWORD /*dwID*/,
                                void * /*pInstanceData*/, DWORD /*cbInstanceData*/,
                                void * /*pPropData*/, DWORD /*cbPropData*/)
{
    if (guidPropSet == kAMPROPSETID_Pin)
        return E_PROP_SET_UNSUPPORTED;      // all our pin properties are read-only
    return E_PROP_SET_UNSUPPORTED;
}

STDMETHODIMP CiSightStream::Get(REFGUID guidPropSet, DWORD dwID,
                                void * /*pInstanceData*/, DWORD /*cbInstanceData*/,
                                void *pPropData, DWORD cbPropData, DWORD *pcbReturned)
{
    if (!(guidPropSet == kAMPROPSETID_Pin))
        return E_PROP_SET_UNSUPPORTED;

    if (dwID == AMPROPERTY_PIN_CATEGORY_LOCAL)
    {
        if (pcbReturned) *pcbReturned = sizeof(GUID);
        if (pPropData == NULL)                       // size query
            return S_OK;
        if (cbPropData < sizeof(GUID))
            return E_UNEXPECTED;
        *(GUID *)pPropData = PIN_CATEGORY_CAPTURE;
        FLogT("pin-cat", "pin IKsPropertySet Get(PIN_CATEGORY) -> CAPTURE");
        return S_OK;
    }

    if (dwID == AMPROPERTY_PIN_MEDIUM_LOCAL)
    {
        // The iSight lives on a 1394 bus.  Answering this is optional; some
        // graph builders use it to avoid mixing transport media.  We report
        // "no medium" rather than inventing a GUID the camera does not have.
        if (pcbReturned) *pcbReturned = 0;
        return pPropData ? E_PROP_ID_UNSUPPORTED : S_OK;
    }

    return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP CiSightStream::QuerySupported(REFGUID guidPropSet, DWORD dwPropID,
                                           DWORD *pTypeSupport)
{
    if (!(guidPropSet == kAMPROPSETID_Pin))
        return E_PROP_SET_UNSUPPORTED;

    if (pTypeSupport == NULL)
        return E_POINTER;

    if (dwPropID == AMPROPERTY_PIN_CATEGORY_LOCAL)
    {
        *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    if (dwPropID == AMPROPERTY_PIN_MEDIUM_LOCAL)
    {
        *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    return E_PROP_ID_UNSUPPORTED;
}

//---------------------------------------------------------------------
// COM plumbing
//---------------------------------------------------------------------
CFactoryTemplate g_Templates[] =
{
    {
        g_wszFilterName,
        &CLSID_ISightFireWireCam,
        CiSightSource::CreateInstance,
        NULL,
        NULL
    }
};
int g_cTemplates = _countof(g_Templates);

//---------------------------------------------------------------------
// minimal IClassFactory over the template array (the modern base
// classes keep their CClassFactory inside dllentry.cpp, which we do
// not link, so we provide our own equivalent here)
//---------------------------------------------------------------------
class CMiniClassFactory : public IClassFactory
{
    const CFactoryTemplate *const m_pTemplate;
    ULONG m_cRef;

public:
    CMiniClassFactory(const CFactoryTemplate *pTemplate)
        : m_pTemplate(pTemplate), m_cRef(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        CheckPointer(ppv, E_POINTER);
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = (LPVOID)this;
            ((LPUNKNOWN)*ppv)->AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return ++m_cRef;
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG cRef = --m_cRef;
        if (cRef == 0)
            delete this;
        return cRef;
    }

    STDMETHODIMP CreateInstance(LPUNKNOWN pUnkOuter, REFIID riid, void **ppv) override
    {
        CheckPointer(ppv, E_POINTER);
        *ppv = NULL;

        // v15: a host that crashes on this filter never gets one.  Checked
        // before anything else, so a denied host runs none of the camera
        // code -- no CMU library, no 1394 handle, no worker threads.  It is
        // what lets QQ open its audio settings again (measured: it died on
        // every attempt while the filter was registered) without giving up
        // the camera in the hosts that do work.
        if (!HostAllowed()) {
            FLog("host gate: DENIED %s -- CreateInstance refused, no camera code runs",
                 HostExeName());
            return E_FAIL;
        }

        if (pUnkOuter != NULL && !IsEqualIID(riid, IID_IUnknown))
            return CLASS_E_NOAGGREGATION;

        FLog("=== iSightCam %s / %s ===", ISIGHT_RELEASE, ISIGHT_BUILD_TAG);
        FLog("CreateInstance: riid=%s requested by %s", GuidName(riid), HostExeName());

        HRESULT hr = S_OK;
        CUnknown *punk = m_pTemplate->CreateInstance(pUnkOuter, &hr);
        if (punk == NULL)
            return FAILED(hr) ? hr : E_OUTOFMEMORY;

        // IMPORTANT: modern baseclasses construct CUnknown with refcount 0;
        // NonDelegatingQueryInterface's AddRef is the reference the caller
        // holds. (dllentry.cpp's CClassFactory behaves the same: delete on
        // failure, no extra Release.) The old "construct = 1 ref + factory
        // Release" pattern turns every object into an immediate use-after-free:
        // the extra NonDelegatingRelease deletes the filter while the caller
        // still holds it, so any later call (EnumPins etc.) crashes with
        // "access violation writing 0x24" inside EnterCriticalSection.
        hr = punk->NonDelegatingQueryInterface(riid, ppv);
        if (FAILED(hr))
            delete punk;
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override
    {
        UNREFERENCED_PARAMETER(fLock);
        return S_OK;
    }
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    CheckPointer(ppv, E_POINTER);
    *ppv = NULL;
    if (!(riid == IID_IUnknown) && !(riid == IID_IClassFactory))
        return E_NOINTERFACE;
    for (int i = 0; i < g_cTemplates; i++)
    {
        if (g_Templates[i].IsClassID(rclsid))
        {
            CMiniClassFactory *pFactory = new CMiniClassFactory(&g_Templates[i]);
            if (pFactory == NULL)
                return E_OUTOFMEMORY;
            HRESULT hr = pFactory->QueryInterface(riid, ppv);
            pFactory->Release();
            return hr;
        }
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    return S_FALSE;   // keep it simple: never unload
}

STDAPI DllRegisterServer()
{
    // dllentry.cpp (not linked) normally sets g_hInst in DllMain. Recover this
    // DLL's own instance handle from a known code address so dllsetup stores
    // the correct path in InprocServer32 instead of the host executable's.
    if (!g_hInst)
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&DllRegisterServer), &g_hInst);
    }

    HRESULT hr = AMovieDllRegisterServer2(TRUE);
    if (FAILED(hr))
        return hr;

    // additionally register as a video capture device so DirectShow
    // apps enumerate us alongside USB webcams
    IFilterMapper2 *pMapper = NULL;
    hr = CoCreateInstance(CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER,
                          IID_IFilterMapper2, (void **)&pMapper);
    if (SUCCEEDED(hr))
    {
        REGPINTYPES rgTypes[3] = {
            { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 },
            { &MEDIATYPE_Video, &MEDIASUBTYPE_YUY2  },
            { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 }
        };
        REGFILTERPINS2 rgPin = { 0 };
        rgPin.dwFlags     = 0;                  // no special flags (not a rendered pin)
        rgPin.cInstances  = 1;
        rgPin.nMediaTypes = 3;
        rgPin.lpMediaType = rgTypes;
        rgPin.nMediums    = 0;
        rgPin.lpMedium    = NULL;
        rgPin.clsPinCategory = &PIN_CATEGORY_CAPTURE;

        REGFILTER2 rf;
        ZeroMemory(&rf, sizeof(rf));
        rf.dwVersion = 2;
        rf.dwMerit   = MERIT_PREFERRED;
        rf.cPins2    = 1;
        rf.rgPins2   = &rgPin;

        hr = pMapper->RegisterFilter(CLSID_ISightFireWireCam, g_wszFilterName,
                                     NULL, &CLSID_VideoInputDeviceCategory,
                                     NULL, &rf);
        pMapper->Release();
    }
    return hr;
}

STDAPI DllUnregisterServer()
{
    HRESULT hr = S_OK;
    IFilterMapper2 *pMapper = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER,
                                   IID_IFilterMapper2, (void **)&pMapper)))
    {
        pMapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, NULL,
                                  CLSID_ISightFireWireCam);
        pMapper->Release();
    }
    AMovieDllUnregisterServer();
    return hr;
}
