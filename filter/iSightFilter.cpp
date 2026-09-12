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
//=====================================================================

#include <windows.h>
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
#define ISIGHT_BUILD_TAG "ISIGHTFILTER-BUILD-V7-20260912-PINCAT"

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

void FLog(const char *fmt, ...)
{
    static CRITICAL_SECTION s_cs;
    static LONG s_once = 0;
    if (InterlockedCompareExchange(&s_once, 1, 0) == 0)
        InitializeCriticalSection(&s_cs);

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    EnterCriticalSection(&s_cs);
    FILE *f = fopen(LogPath(), "a");
    if (f)
    {
        fprintf(f, "[%02d:%02d:%02d.%03d pid=%lu %s] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentProcessId(), HostExeName(), msg);
        fclose(f);
    }
    LeaveCriticalSection(&s_cs);
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

static bool SubTypeKnown(const GUID *sub)
{
    if (!sub) return false;
    if (*sub == MEDIASUBTYPE_RGB24) return true;
    if (*sub == MEDIASUBTYPE_RGB32) return true;
    if (*sub == MEDIASUBTYPE_YUY2)  return true;
    return false;
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

// BGR bottom-up (from getDIB) -> RGB32 bottom-up
static void ConvToRGB32(const BYTE *src, BYTE *dst, ULONG pixels)
{
    for (ULONG i = 0; i < pixels; ++i)
    {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xFF;
        src += 3; dst += 4;
    }
}

// BGR bottom-up (from getDIB) -> YUY2 bottom-up (BT.601, chroma averaged)
static void ConvToYUY2(const BYTE *src, BYTE *dst, ULONG width, ULONG height)
{
    for (ULONG y = 0; y < height; ++y)
    {
        const BYTE *s = src + (size_t)y * width * 3;
        BYTE *d = dst + (size_t)y * width * 2;
        for (ULONG x = 0; x < width; x += 2)
        {
            int B0 = s[0], G0 = s[1], R0 = s[2];
            int B1 = s[3], G1 = s[4], R1 = s[5];
            int Rc = (R0 + R1) >> 1, Gc = (G0 + G1) >> 1, Bc = (B0 + B1) >> 1;

            int Y0 = ((66 * R0 + 129 * G0 + 25 * B0 + 128) >> 8) + 16;
            int Y1 = ((66 * R1 + 129 * G1 + 25 * B1 + 128) >> 8) + 16;
            int U  = ((-38 * Rc - 74 * Gc + 112 * Bc + 128) >> 8) + 128;
            int V  = ((112 * Rc - 94 * Gc - 18 * Bc + 128) >> 8) + 128;

            d[0] = (BYTE)(Y0 < 0 ? 0 : (Y0 > 255 ? 255 : Y0));
            d[1] = (BYTE)(U  < 0 ? 0 : (U  > 255 ? 255 : U));
            d[2] = (BYTE)(Y1 < 0 ? 0 : (Y1 > 255 ? 255 : Y1));
            d[3] = (BYTE)(V  < 0 ? 0 : (V  > 255 ? 255 : V));

            s += 6;
            d += 4;
        }
    }
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
    void  ConfigureVideo();
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
    volatile PVOID   m_hBringUp;      // background bring-up thread
    volatile LONG    m_bringUpState;  // 0 idle, 1 running, 2 ok, 3 failed
    int              m_bringUpRetry;  // frames to wait before retrying
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

    FLog("filter QI %s -> %s", GuidName(riid), SUCCEEDED(hr) ? "OK" : "E_NOINTERFACE");
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
    , m_hBringUp(NULL)
    , m_bringUpState(0)
    , m_bringUpRetry(0)
{
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
    FLog("KickBringUp: camera bring-up started on a worker thread");
}

DWORD WINAPI CiSightStream::BringUpThunk(LPVOID p)
{
    CiSightStream *self = (CiSightStream *)p;
    bool ok = self->TryStart();
    InterlockedExchange(&self->m_bringUpState, ok ? 2 : 3);
    return 0;
}

// bring the camera up and start isochronous acquisition. never throws,
// returns false quietly when hardware is absent/unready.
bool CiSightStream::TryStart()
{
    CAutoLock lock(&m_csCamera);

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
            FLog("TryStart: RefreshCameraList -> %d (no camera)", cameras);
            return false;
        }
        if (m_pCam->SelectCamera(0) != CAM_SUCCESS)
        {
            FLog("TryStart: SelectCamera(0) failed");
            return false;
        }
        DWORD t1 = TickMs();

        int rc = m_pCam->InitCamera(FALSE);
        if (rc != CAM_SUCCESS)
        {
            FLog("TryStart: InitCamera -> %d (CAM_SUCCESS=%d)", rc, CAM_SUCCESS);
            return false;
        }
        DWORD t2 = TickMs();
        // NOTE: this is a slow call (it walks the whole capability table
        // register by register -- ~15 s on this driver/hardware). That is
        // why it must never run inside Active(): it is done here on the
        // streaming thread instead.
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
        FLog("TryStart: StartImageAcquisition -> %d, dropping init state", rc);
        m_bInit = false;          // allow a clean re-init on the next frame
        return false;
    }

    m_bAcquiring = true;
    m_consecFail = 0;
    FLog("TryStart: acquisition started (%ux%u @ %d)", m_width, m_height, m_rateIndex);
    return true;
}

void CiSightStream::ReleaseCamera()
{
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
            m_pCam->StopImageAcquisition();
            m_bAcquiring = false;
        }
        delete m_pCam;      // closes the 1394Camera.sys handle
        m_pCam = NULL;
        FLog("ReleaseCamera: handle closed after %lu frames", m_frameCount);
    }
    m_bInit = false;
    m_consecFail = 0;
    InterlockedExchange(&m_bringUpState, 0);   // allow a fresh bring-up later
    m_bringUpRetry = 0;
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
    HRESULT hr = CSourceStream::Active();
    if (FAILED(hr))
    {
        FLog("Active: CSourceStream::Active -> 0x%08X", (unsigned)hr);
        return hr;
    }
    FLog("Active: pin active");
    // DO NOT touch the camera here. Camera bring-up costs ~15 s (the CMU
    // library reads the whole capability table) and Active() runs inside
    // the host's Pause()/Run() call: blocking here makes every application
    // -- WeChat, QQ, OBS -- give up with "cannot open camera". We just
    // start it on its own thread and hand out black frames until it is up.
    if (IsConnected())
        KickBringUp();
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

HRESULT CiSightStream::FillBuffer(IMediaSample *pSample)
{
    CheckPointer(pSample, E_POINTER);

    // the connected type decides the layout we must produce
    SubTypeInfo si = SubTypeFor(&m_mt.subtype);
    ULONG need = FrameBytesFor(si);
    PBYTE pBuf = NULL;
    if (FAILED(pSample->GetPointer(&pBuf)) || pBuf == NULL)
        return E_FAIL;

    bool got = false;
    if (!m_bAcquiring)
    {
        // camera not live yet: keep the graph fed with black frames and
        // make sure the bring-up thread is running (or retry it after a
        // failure). Never block this thread on the ~15 s hardware init.
        LONG st = InterlockedCompareExchange(&m_bringUpState, 0, 0);
        if (st == 2 || st == 0)
        {
            if (st == 2)   // finished, but the stream was stopped again
                InterlockedExchange(&m_bringUpState, 0);
            KickBringUp();
        }
        else if (st == 3)
        {
            if (++m_bringUpRetry >= 150)   // ~10 s at 15 fps
            {
                m_bringUpRetry = 0;
                FLog("FillBuffer: retrying camera bring-up");
                InterlockedExchange(&m_bringUpState, 0);
                KickBringUp();
            }
        }
    }
    if (m_bAcquiring)
    {
        CAutoLock lock(&m_csCamera);
        int dropped = 0;
        int rc = m_pCam->AcquireImageEx(TRUE, &dropped);

        if (rc == CAM_SUCCESS)
        {
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

                if (*si.subtype == MEDIASUBTYPE_YUY2)
                    ConvToYUY2(src, pBuf, ISIGHT_WIDTH, ISIGHT_HEIGHT);
                else if (*si.subtype == MEDIASUBTYPE_RGB32)
                    ConvToRGB32(src, pBuf, ISIGHT_WIDTH * ISIGHT_HEIGHT);
                else
                    memcpy(pBuf, src, ISIGHT_DIB_BYTES);

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
            if (m_consecFail == 12)
            {
                // a 3 s-timeout grab failing a dozen times in a row means the
                // stream really is gone: drop it and let the bring-up thread
                // start a clean one (never here -- this is the streaming
                // thread and it must keep producing samples)
                FLog("FillBuffer: camera unresponsive, dropping stream for re-init");
                m_pCam->StopImageAcquisition();
                m_bAcquiring = false;
                m_bInit = false;
                m_consecFail = 0;
                InterlockedExchange(&m_bringUpState, 0);
                KickBringUp();
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
    vih.bmiHeader.biHeight      = ISIGHT_HEIGHT;   // positive = bottom-up (getDIB format)
    vih.bmiHeader.biPlanes      = 1;
    vih.bmiHeader.biBitCount    = (WORD)si.bpp;
    vih.bmiHeader.biCompression = si.compression;
    vih.bmiHeader.biSizeImage   = FrameBytesFor(si);

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
    if (!SubTypeKnown(&pmt->subtype)) return false;

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

    static const GUID *kSubs[] = { &MEDIASUBTYPE_RGB24, &MEDIASUBTYPE_YUY2, &MEDIASUBTYPE_RGB32 };
    if (iPosition >= (int)(sizeof(kSubs) / sizeof(kSubs[0])))
        return VFW_S_NO_MORE_ITEMS;

    BuildMediaType(kSubs[iPosition], kFrameDurations[kMaxRateIndex], pmt);
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

    CMediaType mt;
    if (MediaTypeCompatible(&m_mt, &mt))
    {
        *ppmt = CreateMediaType(&mt);
    }
    else
    {
        BuildMediaType(&MEDIASUBTYPE_RGB24, kFrameDurations[kMaxRateIndex], &mt);
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
    *piCount = 3;
    *piSize  = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP CiSightStream::GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC)
{
    CheckPointer(ppmt, E_POINTER);
    CheckPointer(pSCC, E_POINTER);

    static const GUID *kSubs[] = { &MEDIASUBTYPE_RGB24, &MEDIASUBTYPE_YUY2, &MEDIASUBTYPE_RGB32 };
    if (iIndex < 0 || iIndex >= 3)
        return S_FALSE;

    CAutoLock lock(m_pFilter->pStateLock());

    CMediaType mt;
    BuildMediaType(kSubs[iIndex], kFrameDurations[kMaxRateIndex], &mt);
    *ppmt = CreateMediaType(&mt);
    if (*ppmt == NULL)
        return E_OUTOFMEMORY;

    ULONG bytes = FrameBytesFor(SubTypeFor(kSubs[iIndex]));

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
    FLog("pin QI %s -> %s", GuidName(riid), SUCCEEDED(hr) ? "OK" : "E_NOINTERFACE");
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
        FLog("pin IKsPropertySet Get(PIN_CATEGORY) -> CAPTURE");
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
        if (pUnkOuter != NULL && !IsEqualIID(riid, IID_IUnknown))
            return CLASS_E_NOAGGREGATION;

        FLog("=== %s ===", ISIGHT_BUILD_TAG);
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
