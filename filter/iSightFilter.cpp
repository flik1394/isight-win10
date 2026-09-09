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
// Format: Format 0 / Mode 2 (640x480 YUV 4:2:2) -> RGB24 via the CMU
// library's getDIB() (bottom-up BGR = DirectShow MEDIASUBTYPE_RGB24
// with positive biHeight).
//=====================================================================

#include <windows.h>
#include <streams.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <strsafe.h>

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
// still referenced by dllsetup.obj (AMovieDllRegisterServer)
HINSTANCE g_hInst = NULL;

// dllentry.cpp replacement: dllsetup uses g_hInst to compute this DLL's path
// during DllRegisterServer. Without this, GetModuleFileName(NULL) returns the
// host executable (e.g. regsvr32.exe) and the COM registration is corrupted.
extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
        g_hInst = hInst;
    return TRUE;
}

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

#define ISIGHT_WIDTH  640
#define ISIGHT_HEIGHT 480
#define ISIGHT_BPP     24
#define FRAME_BYTES   (ISIGHT_WIDTH * ISIGHT_HEIGHT * ISIGHT_BPP / 8)

//---------------------------------------------------------------------
// CiSightStream : one output pin streaming RGB24 frames
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
    void  StopIfNeeded();
    void  ConfigureVideo();
    void  BuildMediaType(CMediaType *pmt);
    void  DeliverBlackFrame(IMediaSample *pSample);

    CCritSec         m_csCamera;      // serialize camera access
    C1394Camera      m_cam;
    bool             m_bInit;         // camera selected+initialized
    bool             m_bAcquiring;    // isoch acquisition running
    unsigned long    m_width, m_height;
    int              m_rateIndex;
    REFERENCE_TIME   m_rtNext;        // timestamp of next frame
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
    , m_bInit(false)
    , m_bAcquiring(false)
    , m_width(ISIGHT_WIDTH)
    , m_height(ISIGHT_HEIGHT)
    , m_rateIndex(4)
    , m_rtNext(0)
{
}

CiSightStream::~CiSightStream()
{
    StopIfNeeded();
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
        if (m_cam.RefreshCameraList() <= 0)
            return false;
        if (m_cam.SelectCamera(0) != CAM_SUCCESS)
            return false;
        if (m_cam.InitCamera(FALSE) != CAM_SUCCESS)
            return false;
        ConfigureVideo();
        m_bInit = true;
    }

    if (m_cam.StartImageAcquisition() != CAM_SUCCESS)
        return false;

    m_bAcquiring = true;
    return true;
}

void CiSightStream::StopIfNeeded()
{
    CAutoLock lock(&m_csCamera);
    if (m_bAcquiring)
    {
        m_cam.StopImageAcquisition();
        m_bAcquiring = false;
    }
}

// prefer Format 0 / Mode 2 (640x480 YUV422) @ 30fps; fall back to the
// highest available mode/rate pair. The iSight always exposes mode 2.
void CiSightStream::ConfigureVideo()
{
    bool found = false;

    for (unsigned long f = 0; f < 3 && !found; f++)
    {
        if (!m_cam.HasVideoFormat(f))
            continue;
        for (unsigned long m = 0; m < 8 && !found; m++)
        {
            if (!m_cam.HasVideoMode(f, m))
                continue;
            for (int r = 5; r >= 0 && !found; r--)
            {
                if (!m_cam.HasVideoFrameRate(f, m, (unsigned long)r))
                    continue;
                m_cam.SetVideoFormat(f);
                m_cam.SetVideoMode(m);
                m_cam.SetVideoFrameRate((unsigned long)r);
                m_rateIndex = r;
                found = true;
            }
        }
    }

    if (found)
    {
        unsigned long w = 0, h = 0;
        m_cam.GetVideoFrameDimensions(&w, &h);
        if (w != ISIGHT_WIDTH || h != ISIGHT_HEIGHT)
        {
            // only the fixed 640x480 mode is supported by this filter
            DbgLog((LOG_ERROR, 0, TEXT("iSight: unexpected frame size %lux%lu"), w, h));
        }
    }
    m_cam.UpdateParameters(TRUE);
}

HRESULT CiSightStream::Active()
{
    CAutoLock lock(m_pFilter->pStateLock());
    HRESULT hr = CSourceStream::Active();
    if (FAILED(hr))
        return hr;
    TryStart();   // failure is not fatal; FillBuffer retries per-frame
    return S_OK;
}

HRESULT CiSightStream::Inactive()
{
    CAutoLock lock(m_pFilter->pStateLock());
    HRESULT hr = CSourceStream::Inactive();  // stops the worker thread first
    StopIfNeeded();
    return hr;
}

void CiSightStream::DeliverBlackFrame(IMediaSample *pSample)
{
    PBYTE pBuf = NULL;
    if (SUCCEEDED(pSample->GetPointer(&pBuf)) && pBuf)
        ZeroMemory(pBuf, pSample->GetSize());
}

HRESULT CiSightStream::FillBuffer(IMediaSample *pSample)
{
    CheckPointer(pSample, E_POINTER);

    if (!TryStart())
    {
        DeliverBlackFrame(pSample);
        Sleep(static_cast<DWORD>(kFrameDurations[m_rateIndex] / 10000));   // ms
    }
    else
    {
        PBYTE pBuf = NULL;
        HRESULT hr = pSample->GetPointer(&pBuf);
        if (FAILED(hr) || pBuf == NULL)
            return E_FAIL;

        int dropped = 0;
        CAutoLock lock(&m_csCamera);
        if (m_cam.AcquireImageEx(TRUE, &dropped) != CAM_SUCCESS ||
            m_cam.getDIB(pBuf, (unsigned long)pSample->GetSize()) != CAM_SUCCESS)
        {
            DeliverBlackFrame(pSample);
        }
    }

    // timestamp
    REFERENCE_TIME rtStart = m_rtNext;
    REFERENCE_TIME rtEnd   = rtStart + kFrameDurations[m_rateIndex];
    m_rtNext = rtEnd;
    pSample->SetTime(&rtStart, &rtEnd);
    pSample->SetSyncPoint(TRUE);
    pSample->SetDiscontinuity(m_rtNext == kFrameDurations[m_rateIndex]);

    return S_OK;
}

HRESULT CiSightStream::DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pprop)
{
    CheckPointer(pAlloc, E_POINTER);
    CheckPointer(pprop, E_POINTER);

    CAutoLock lock(m_pFilter->pStateLock());

    pprop->cBuffers = 2;
    pprop->cbBuffer = FRAME_BYTES;
    pprop->cbAlign  = 1;
    pprop->cbPrefix = 0;

    ALLOCATOR_PROPERTIES actual;
    HRESULT hr = pAlloc->SetProperties(pprop, &actual);
    if (FAILED(hr))
        return hr;
    if (actual.cbBuffer < pprop->cbBuffer)
        return E_OUTOFMEMORY;
    return S_OK;
}

void CiSightStream::BuildMediaType(CMediaType *pmt)
{
    pmt->InitMediaType();
    pmt->SetType(&MEDIATYPE_Video);
    pmt->SetSubtype(&MEDIASUBTYPE_RGB24);
    pmt->SetFormatType(&FORMAT_VideoInfo);
    pmt->SetTemporalCompression(FALSE);

    VIDEOINFOHEADER vih;
    ZeroMemory(&vih, sizeof(vih));
    vih.AvgTimePerFrame = kFrameDurations[m_rateIndex];
    vih.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    vih.bmiHeader.biWidth       = ISIGHT_WIDTH;
    vih.bmiHeader.biHeight      = ISIGHT_HEIGHT;   // positive = bottom-up (getDIB format)
    vih.bmiHeader.biPlanes      = 1;
    vih.bmiHeader.biBitCount    = ISIGHT_BPP;
    vih.bmiHeader.biCompression = BI_RGB;
    vih.bmiHeader.biSizeImage   = FRAME_BYTES;

    pmt->SetFormat((PBYTE)&vih, sizeof(vih));
    pmt->SetSampleSize(FRAME_BYTES);
}

HRESULT CiSightStream::GetMediaType(int iPosition, CMediaType *pmt)
{
    CAutoLock lock(m_pFilter->pStateLock());
    if (iPosition < 0)
        return E_INVALIDARG;
    if (iPosition > 0)
        return VFW_S_NO_MORE_ITEMS;
    BuildMediaType(pmt);
    return S_OK;
}

HRESULT CiSightStream::CheckMediaType(const CMediaType *pmt)
{
    CAutoLock lock(m_pFilter->pStateLock());
    CMediaType accepted;
    BuildMediaType(&accepted);
    return (accepted == *pmt) ? S_OK : E_FAIL;
}

//---------------------------------------------------------------------
// IAMStreamConfig
//---------------------------------------------------------------------
STDMETHODIMP CiSightStream::GetFormat(AM_MEDIA_TYPE **ppmt)
{
    CheckPointer(ppmt, E_POINTER);
    CAutoLock lock(m_pFilter->pStateLock());
    CMediaType mt;
    BuildMediaType(&mt);
    *ppmt = CreateMediaType(&mt);
    return (*ppmt != NULL) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CiSightStream::SetFormat(AM_MEDIA_TYPE *pmt)
{
    CheckPointer(pmt, E_POINTER);
    CAutoLock lock(m_pFilter->pStateLock());
    CMediaType accepted;
    BuildMediaType(&accepted);
    if (accepted == *pmt)
        return S_OK;
    return VFW_E_INVALIDMEDIATYPE;
}

STDMETHODIMP CiSightStream::GetNumberOfCapabilities(int *piCount, int *piSize)
{
    CheckPointer(piCount, E_POINTER);
    CheckPointer(piSize, E_POINTER);
    *piCount = 1;
    *piSize  = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP CiSightStream::GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC)
{
    CheckPointer(ppmt, E_POINTER);
    CheckPointer(pSCC, E_POINTER);
    if (iIndex != 0)
        return S_FALSE;

    CAutoLock lock(m_pFilter->pStateLock());

    CMediaType mt;
    BuildMediaType(&mt);
    *ppmt = CreateMediaType(&mt);
    if (*ppmt == NULL)
        return E_OUTOFMEMORY;

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
    caps->MinBitsPerSecond      = FRAME_BYTES * 8 * 15;
    caps->MaxBitsPerSecond      = FRAME_BYTES * 8 * 30;
    caps->MinFrameInterval      = kFrameDurations[4];
    caps->MaxFrameInterval      = kFrameDurations[3];
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

        HRESULT hr = S_OK;
        CUnknown *punk = m_pTemplate->CreateInstance(pUnkOuter, &hr);
        if (punk == NULL)
            return FAILED(hr) ? hr : E_OUTOFMEMORY;
        hr = punk->NonDelegatingQueryInterface(riid, ppv);
        punk->NonDelegatingRelease();
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
        REGPINTYPES rgTypes = { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 };
        REGFILTERPINS2 rgPin =
        {
            0,                    // dwFlags
            1,                    // cInstances
            1,                    // nMediaTypes
            &rgTypes,
            0,                    // nMediums
            NULL,                 // lpMedium
            &PIN_CATEGORY_CAPTURE
        };
        REGFILTER2 rf;
        ZeroMemory(&rf, sizeof(rf));
        rf.dwVersion = 2;
        rf.dwMerit   = MERIT_PREFERRED;
        rf.cPins2    = 1;
        rgPin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
        rf.rgPins2 = &rgPin;

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
