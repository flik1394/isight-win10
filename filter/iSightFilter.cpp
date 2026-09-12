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
class CiSightStream : public CSourceStream, public IAMStreamConfig
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

    // expose IAMStreamConfig
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv);

private:
    bool  TryStart();          // lazily init camera + start acquisition
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
};

//---------------------------------------------------------------------
// CiSightSource : the filter
//---------------------------------------------------------------------
class CiSightSource : public CSource
{
public:
    DECLARE_IUNKNOWN
    static CUnknown *WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT *phr);

private:
    CiSightSource(LPUNKNOWN lpunk, HRESULT *phr);
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

        int rc = m_pCam->InitCamera(FALSE);
        if (rc != CAM_SUCCESS)
        {
            FLog("TryStart: InitCamera -> %d (CAM_SUCCESS=%d)", rc, CAM_SUCCESS);
            return false;
        }
        FLog("TryStart: InitCamera OK, MaxSpeed=%d", m_pCam->GetMaxSpeed());
        ConfigureVideo();
        m_bInit = true;
    }

    int rc = m_pCam->StartImageAcquisition();
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
}

// prefer Format 0 / Mode 2 (640x480 YUV422). The iSight is an S100
// (100 Mbit/s) device: 640x480 YUV422 @ 30fps needs ~150 Mbit/s of
// isochronous bandwidth and does NOT fit. Cap the rate at 15 fps
// (~74 Mbit/s), which is the classic working setting for this camera.
// The iSight always exposes mode 2.
void CiSightStream::ConfigureVideo()
{
    bool found = false;

    for (unsigned long f = 0; f < 3 && !found; f++)
    {
        if (!m_pCam->HasVideoFormat(f))
            continue;
        for (unsigned long m = 0; m < 8 && !found; m++)
        {
            if (!m_pCam->HasVideoMode(f, m))
                continue;
            for (int r = kMaxRateIndex; r >= 0 && !found; r--)
            {
                if (!m_pCam->HasVideoFrameRate(f, m, (unsigned long)r))
                    continue;
                m_pCam->SetVideoFormat(f);
                m_pCam->SetVideoMode(m);
                m_pCam->SetVideoFrameRate((unsigned long)r);
                m_rateIndex = r;
                found = true;
                FLog("ConfigureVideo: format=%lu mode=%lu rate=%d", f, m, r);
            }
        }
    }

    if (!found)
    {
        m_rateIndex = kMaxRateIndex;
        FLog("ConfigureVideo: no supported format/mode/rate found, keeping %d", m_rateIndex);
    }
    else
    {
        unsigned long w = 0, h = 0;
        m_pCam->GetVideoFrameDimensions(&w, &h);
        if (w && h)
        {
            m_width = w;
            m_height = h;
        }
        if (w != ISIGHT_WIDTH || h != ISIGHT_HEIGHT)
            FLog("ConfigureVideo: unexpected frame size %lux%lu", w, h);
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
    TryStart();   // failure is not fatal; FillBuffer retries per-frame
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
    if (TryStart())
    {
        CAutoLock lock(&m_csCamera);
        int dropped = 0;
        int rc = m_pCam->AcquireImageEx(TRUE, &dropped);

        if (rc == CAM_SUCCESS)
        {
            if (m_pScratch == NULL)
            {
                m_pScratch = new BYTE[ISIGHT_DIB_BYTES];
                m_scratchBytes = m_pScratch ? ISIGHT_DIB_BYTES : 0;
            }
            if (m_pScratch &&
                m_pCam->getDIB(m_pScratch, m_scratchBytes) == CAM_SUCCESS)
            {
                if (*si.subtype == MEDIASUBTYPE_YUY2)
                    ConvToYUY2(m_pScratch, pBuf, m_width, m_height);
                else if (*si.subtype == MEDIASUBTYPE_RGB32)
                    ConvToRGB32(m_pScratch, pBuf, m_width * m_height);
                else
                    memcpy(pBuf, m_pScratch, ISIGHT_DIB_BYTES);

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
                FLog("FillBuffer: AcquireImageEx -> %d (fail streak %d)", rc, m_consecFail);
            if (m_consecFail == 100)
            {
                FLog("FillBuffer: camera unresponsive, re-initialising");
                m_pCam->StopImageAcquisition();
                m_bAcquiring = false;
                m_bInit = false;
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
    if (!SubTypeKnown(pmt->subtype)) return false;

    const VIDEOINFOHEADER *pvi = (const VIDEOINFOHEADER *)pmt->pbFormat;
    if (pvi->bmiHeader.biWidth != (LONG)ISIGHT_WIDTH) return false;
    if (labs(pvi->bmiHeader.biHeight) != (LONG)ISIGHT_HEIGHT) return false;

    SubTypeInfo si = SubTypeFor(pmt->subtype);
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
        BuildMediaType(pmt->subtype, interval, pNormalized);
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
             pmt ? SubTypeName(pmt->subtype) : "null",
             pmt ? ((pmt->formattype == FORMAT_VideoInfo) ? "VideoInfo" :
                    (pmt->formattype == FORMAT_VideoInfo2) ? "VideoInfo2" : "other") : "-",
             pvi ? (long)pvi->bmiHeader.biWidth : -1,
             pvi ? (long)pvi->bmiHeader.biHeight : -1,
             pvi ? (unsigned)pvi->bmiHeader.biBitCount : 0,
             pvi ? (unsigned)pvi->bmiHeader.biCompression : 0);
        return E_FAIL;
    }

    FLog("CheckMediaType: accept %s %ldx%ld bpp=%u interval=%lld",
         SubTypeName(pmt->subtype),
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
             SubTypeName(pmt->subtype),
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
         SubTypeName(pmt->subtype),
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
    caps->MinSampleSize         = bytes;
    caps->MaxSampleSize         = bytes;
    return S_OK;
}

STDMETHODIMP CiSightStream::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    CheckPointer(ppv, E_POINTER);
    if (riid == IID_IAMStreamConfig)
        return GetInterface((IAMStreamConfig *)this, ppv);
    return CSourceStream::NonDelegatingQueryInterface(riid, ppv);
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

        FLog("CreateInstance: riid requested by %s", HostExeName());

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
