//=====================================================================
// graphtest.cpp  ->  isight-graphtest.exe
//
// Independent harness for the "Apple iSight (FireWire)" DirectShow
// capture filter.  It does exactly what a mainstream application does
// (WeChat / QQ / OBS) and reports every step, so we can tell whether a
// failure lives in our filter or in the host application:
//
//   1. enumerate CLSID_VideoInputDeviceCategory like a video app
//   2. bind our filter, walk its pins, dump IAMStreamConfig caps
//   3. probe SetFormat() variants (NULL reset, own caps, 30 fps request)
//   4. build filter -> NullRenderer, connect, Run for 5 s, stop
//   5. run a second time (hosts open/close the device repeatedly)
//
// Everything is written to isight-graphtest.log next to the exe; the
// filter writes its own trace to %LOCALAPPDATA%\iSightCam.log
//=====================================================================

#include <windows.h>
#include <dshow.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_log = NULL;

static void LOG(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_log)
    {
        fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, msg);
        fflush(g_log);
    }
    printf("%s\n", msg);
}

static const char *HrName(HRESULT hr)
{
    static char buf[128];
    switch ((unsigned)hr)
    {
    case S_OK:                          return "S_OK";
    case S_FALSE:                       return "S_FALSE";
    case E_FAIL:                        return "E_FAIL";
    case E_POINTER:                     return "E_POINTER";
    case E_INVALIDARG:                  return "E_INVALIDARG";
    case E_UNEXPECTED:                  return "E_UNEXPECTED";
    case E_OUTOFMEMORY:                 return "E_OUTOFMEMORY";
    case VFW_E_INVALIDMEDIATYPE:        return "VFW_E_INVALIDMEDIATYPE";
    case VFW_E_CANNOT_CONNECT:          return "VFW_E_CANNOT_CONNECT";
    case VFW_E_NO_ACCEPTABLE_TYPES:     return "VFW_E_NO_ACCEPTABLE_TYPES";
    case VFW_E_TYPE_NOT_ACCEPTED:       return "VFW_E_TYPE_NOT_ACCEPTED";
    case VFW_E_NOT_CONNECTED:           return "VFW_E_NOT_CONNECTED";
    case VFW_E_CANNOT_RENDER:           return "VFW_E_CANNOT_RENDER";
    case VFW_S_NO_MORE_ITEMS:           return "VFW_S_NO_MORE_ITEMS";
    default:
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08X", (unsigned)hr);
        return buf;
    }
}

static const char *SubTypeName(const GUID *g)
{
    if (!g) return "null";
    if (*g == MEDIASUBTYPE_RGB24) return "RGB24";
    if (*g == MEDIASUBTYPE_RGB32) return "RGB32";
    if (*g == MEDIASUBTYPE_YUY2)  return "YUY2";
    if (*g == MEDIASUBTYPE_YUYV)  return "YUYV";
    if (*g == MEDIASUBTYPE_UYVY)  return "UYVY";
    if (*g == MEDIASUBTYPE_MJPG)  return "MJPG";
    return "other";
}

// CLSID_NullRenderer lives in strmiids/uuids but is not always declared
// by the Windows SDK headers; define it locally.
static const GUID CLSID_NullRendererLocal =
{ 0xc1f400a4, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };

// same for the system reference clock
static const GUID CLSID_SystemClockLocal =
{ 0xe436ebb1, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };

// FreeMediaType/DeleteMediaType come from the DirectShow base classes,
// which this tool deliberately does not link.
static void FreeMediaTypeLocal(AM_MEDIA_TYPE &mt)
{
    if (mt.cbFormat)
    {
        CoTaskMemFree((PVOID)mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = NULL;
    }
}

static void DeleteMediaTypeLocal(AM_MEDIA_TYPE *pmt)
{
    if (!pmt) return;
    FreeMediaTypeLocal(*pmt);
    CoTaskMemFree((PVOID)pmt);
}

static void DescribeMediaType(const char *prefix, const AM_MEDIA_TYPE *pmt)
{
    if (!pmt)
    {
        LOG("%s: null media type", prefix);
        return;
    }
    const VIDEOINFOHEADER *pvi = (pmt->cbFormat >= sizeof(VIDEOINFOHEADER))
                               ? (const VIDEOINFOHEADER *)pmt->pbFormat : NULL;
    LOG("%s: subtype=%-5s fmt=%s %ldx%ld bpp=%u comp=%08X interval=%lld image=%lu",
        prefix, SubTypeName(&pmt->subtype),
        (pmt->formattype == FORMAT_VideoInfo) ? "VideoInfo" :
        (pmt->formattype == FORMAT_VideoInfo2) ? "VideoInfo2" : "other",
        pvi ? (long)pvi->bmiHeader.biWidth : -1,
        pvi ? (long)pvi->bmiHeader.biHeight : -1,
        pvi ? (unsigned)pvi->bmiHeader.biBitCount : 0,
        pvi ? (unsigned)pvi->bmiHeader.biCompression : 0,
        pvi ? (long long)pvi->AvgTimePerFrame : 0,
        pvi ? (unsigned long)pvi->bmiHeader.biSizeImage : 0);
}

//---------------------------------------------------------------------
// pin category -- the exact lookup ICaptureGraphBuilder2::FindPin() does.
// If the pin does not answer this, RenderStream() returns E_INVALIDARG
// and the host gives up before negotiating any media type.
//---------------------------------------------------------------------
static const GUID kAMPROPSETID_PinLocal =
{ 0x9b00f101, 0x1567, 0x11d1, { 0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5 } };
#define AMPROPERTY_PIN_CATEGORY_LOCAL 0

// IID_IKsPropertySet lives in ksuser/uuid rather than strmiids, which is all
// this tool links -- so carry the value locally.
static const GUID kIID_IKsPropertySet =
{ 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0xdb } };

static void GuidText(const GUID &g, char *out, size_t cb)
{
    _snprintf_s(out, cb, _TRUNCATE,
                "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                (unsigned long)g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static const char *CategoryName(const GUID &g)
{
    if (IsEqualGUID(g, PIN_CATEGORY_CAPTURE)) return "PIN_CATEGORY_CAPTURE";
    return "(not PIN_CATEGORY_CAPTURE)";
}

static void ProbePinCategory(IPin *pPin)
{
    LOG("pin category lookup (what the capture graph builder runs first):");
    if (!pPin) { LOG("  no pin"); return; }

    IKsPropertySet *pPs = NULL;
    HRESULT hr = pPin->QueryInterface(kIID_IKsPropertySet, (void **)&pPs);
    if (FAILED(hr) || pPs == NULL)
    {
        LOG("  pin QI IKsPropertySet -> %s   <=== RenderStream(PIN_CATEGORY_CAPTURE) WILL FAIL", HrName(hr));
        return;
    }

    DWORD supported = 0;
    hr = pPs->QuerySupported(kAMPROPSETID_PinLocal, AMPROPERTY_PIN_CATEGORY_LOCAL, &supported);
    LOG("  QuerySupported(AMPROPSETID_Pin, CATEGORY) -> %s support=0x%X", HrName(hr), supported);

    GUID cat;
    ZeroMemory(&cat, sizeof(cat));
    DWORD cb = 0;
    hr = pPs->Get(kAMPROPSETID_PinLocal, AMPROPERTY_PIN_CATEGORY_LOCAL,
                  NULL, 0, &cat, sizeof(cat), &cb);
    if (SUCCEEDED(hr))
    {
        char txt[64];
        GuidText(cat, txt, sizeof(txt));
        LOG("  Get(PIN_CATEGORY) -> %s  %s", CategoryName(cat), txt);
        LOG("  ==> capture graph builder can find this pin  (GOOD)");
    }
    else
    {
        LOG("  Get(PIN_CATEGORY) -> %s   <=== RenderStream WILL FAIL", HrName(hr));
    }
    pPs->Release();
}

//---------------------------------------------------------------------
// The filter traces to %LOCALAPPDATA%\iSightCam.log.  Read it back and
// report what actually happened inside the host process: which build was
// loaded, how long the camera bring-up took and how many frames the
// filter really delivered.  Only lines belonging to THIS process count.
//---------------------------------------------------------------------
static void ReportFilterLog(void)
{
    char path[MAX_PATH] = "";
    const char *lad = getenv("LOCALAPPDATA");
    if (lad && *lad)
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\iSightCam.log", lad);

    char pidTag[48];
    _snprintf_s(pidTag, sizeof(pidTag), _TRUNCATE, "pid=%lu ", (unsigned long)GetCurrentProcessId());

    FILE *f = (path[0] != 0) ? fopen(path, "r") : NULL;
    if (f == NULL)
    {
        LOG("filter log: cannot open %s", path);
        return;
    }

    LOG("--- filter log (%s), this process only ---", path);
    char line[2048];
    char lastBuild[400] = "";
    char lastInit[400] = "";
    char lastCfg[400] = "";
    char lastStart[400] = "";
    char lastErr[400] = "";
    unsigned long maxFrame = 0;
    int framesLogged = 0, acquireErrors = 0, qis = 0, buildSeen = 0;

    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, pidTag) == NULL)
            continue;
        if (strstr(line, "ISIGHTFILTER-BUILD-"))  { buildSeen++; _snprintf_s(lastBuild, sizeof(lastBuild), _TRUNCATE, "%s", line); }
        if (strstr(line, "init="))                { _snprintf_s(lastInit,  sizeof(lastInit),  _TRUNCATE, "%s", line); }
        if (strstr(line, "ConfigureVideo done"))  { _snprintf_s(lastCfg,   sizeof(lastCfg),   _TRUNCATE, "%s", line); }
        if (strstr(line, "acquisition started"))  { _snprintf_s(lastStart, sizeof(lastStart), _TRUNCATE, "%s", line); }
        if (strstr(line, "AcquireImageEx ->"))    { acquireErrors++; _snprintf_s(lastErr, sizeof(lastErr), _TRUNCATE, "%s", line); }

        char *p = strstr(line, "FillBuffer #");
        if (p)
        {
            framesLogged++;
            unsigned long n = strtoul(p + 11, NULL, 10);
            if (n > maxFrame) maxFrame = n;
        }
        if (strstr(line, "QI ")) qis++;
    }
    fclose(f);

    LOG("  loaded build ....... %s%s", buildSeen ? "" : "<no build tag line found>",
        buildSeen ? lastBuild : "");
    LOG("  bring-up ........... %s", lastInit[0]  ? lastInit  : "<none>");
    LOG("  video config ....... %s", lastCfg[0]   ? lastCfg   : "<none>");
    LOG("  stream start ....... %s", lastStart[0] ? lastStart : "<none>");
    LOG("  frames delivered ... %lu  (filter logs frame #1..3 and every 150th; %d such lines)",
        maxFrame, framesLogged);
    if (acquireErrors)
        LOG("  capture errors ..... %d, last: %s", acquireErrors, lastErr);
    LOG("  queryinterface lines %d", qis);
    LOG("  VERDICT: %s", maxFrame > 0 ? "FILTER DELIVERS REAL FRAMES" :
                          "NO FRAMES DELIVERED BY THE FILTER");
}

// keep the pump alive so DirectShow messages/events get processed
static void PumpFor(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end)
    {
        DWORD left = end - GetTickCount();
        MsgWaitForMultipleObjects(0, NULL, FALSE, left > 100 ? 100 : left, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

static void DrainEvents(IMediaEvent *pEvent)
{
    if (!pEvent) return;
    long code = 0;
    LONG_PTR p1 = 0, p2 = 0;
    while (pEvent->GetEvent(&code, &p1, &p2, 0) == S_OK)
    {
        LOG("  graph event: code=0x%04X p1=%ld p2=%ld", (unsigned)code, (long)p1, (long)p2);
        pEvent->FreeEventParams(code, p1, p2);
    }
}

static bool FindInputPin(IBaseFilter *pFilter, IPin **ppPin, const char **pName)
{
    IEnumPins *pEnum = NULL;
    if (FAILED(pFilter->EnumPins(&pEnum)) || !pEnum)
        return false;

    IPin *pPin = NULL;
    bool found = false;
    while (!found && pEnum->Next(1, &pPin, NULL) == S_OK)
    {
        PIN_DIRECTION dir;
        if (SUCCEEDED(pPin->QueryDirection(&dir)) && dir == PINDIR_INPUT)
        {
            *ppPin = pPin;
            *pName = "input";
            found = true;
            break;
        }
        pPin->Release();
        pPin = NULL;
    }
    pEnum->Release();
    return found;
}

// Which interfaces does our filter/pin actually expose? Hosts refuse to
// open a capture device over one missing interface far more often than
// over a bad media type, and this list is the only way to see it.
#define PROBE(unk, what, iface)                                                \
    do {                                                                       \
        void *pv__ = NULL;                                                     \
        HRESULT hr__ = (unk)->QueryInterface(__uuidof(iface), &pv__);           \
        LOG("  %s QI %-22s -> %s", what, #iface, SUCCEEDED(hr__) ? "OK" : HrName(hr__)); \
        if (SUCCEEDED(hr__) && pv__) ((IUnknown *)pv__)->Release();            \
    } while (0)

static void ProbeFilterInterfaces(IBaseFilter *pF)
{
    LOG("interface probe on the filter:");
    PROBE(pF, "filter", IBaseFilter);
    PROBE(pF, "filter", IMediaFilter);
    PROBE(pF, "filter", IPersist);
    PROBE(pF, "filter", IPersistStream);
    PROBE(pF, "filter", IAMFilterMiscFlags);
    PROBE(pF, "filter", ISpecifyPropertyPages);
    PROBE(pF, "filter", IAMStreamConfig);
    PROBE(pF, "filter", IAMVideoProcAmp);
    PROBE(pF, "filter", IAMCameraControl);
    PROBE(pF, "filter", IKsPropertySet);
    PROBE(pF, "filter", IAMBufferNegotiation);
    PROBE(pF, "filter", IReferenceClock);
}

static void ProbePinInterfaces(IPin *pP)
{
    LOG("interface probe on the capture pin:");
    PROBE(pP, "pin", IPin);
    PROBE(pP, "pin", IAMStreamConfig);
    PROBE(pP, "pin", IQualityControl);
    PROBE(pP, "pin", IMemInputPin);
    PROBE(pP, "pin", IKsPropertySet);
    PROBE(pP, "pin", IAMBufferNegotiation);
    PROBE(pP, "pin", IAMVideoProcAmp);
    PROBE(pP, "pin", IAMCameraControl);
    PROBE(pP, "pin", ISpecifyPropertyPages);
    PROBE(pP, "pin", IReferenceClock);
}

int main(void)
{
    HRESULT hr = S_OK;
    ICreateDevEnum *pDevEnum = NULL;
    IEnumMoniker   *pEnumMon = NULL;
    IBaseFilter    *pFilter  = NULL;
    IEnumPins      *pPins    = NULL;
    IPin           *pOutPin  = NULL;
    IAMStreamConfig *pCfg    = NULL;
    int nCaps = 0, cbCaps = 0;

    g_log = fopen("isight-graphtest.log", "w");
    LOG("=== iSight filter graph test %s ===", __TIMESTAMP__);
    LOG("host process pid=%lu (32/64-bit build matches this process)",
        (unsigned long)GetCurrentProcessId());

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    LOG("CoInitializeEx -> %s", HrName(hr));

    hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void **)&pDevEnum);
    LOG("create device enumerator -> %s", HrName(hr));
    if (FAILED(hr))
        goto done;

    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnumMon, 0);
    LOG("enumerate VideoInputDeviceCategory -> %s", HrName(hr));
    if (FAILED(hr) || pEnumMon == NULL)
        goto done;

    //---------------------------------------------------------------
    // 1. list every capture device, keep the iSight
    //---------------------------------------------------------------
    {
        IMoniker *pMon = NULL;
        ULONG fetched = 0;
        int index = 0;
        while (pEnumMon->Next(1, &pMon, &fetched) == S_OK && fetched == 1)
        {
            char name[256] = "";
            IPropertyBag *pBag = NULL;
            if (SUCCEEDED(pMon->BindToStorage(NULL, NULL, IID_IPropertyBag, (void **)&pBag)) && pBag)
            {
                VARIANT var;
                VariantInit(&var);
                if (SUCCEEDED(pBag->Read(L"FriendlyName", &var, NULL)) && var.bstrVal)
                    WideCharToMultiByte(CP_ACP, 0, var.bstrVal, -1, name, sizeof(name), NULL, NULL);
                VariantClear(&var);
                pBag->Release();
            }
            LOG("device[%d]: \"%s\"", index, name);

            if (!pFilter && strstr(name, "iSight"))
            {
                hr = pMon->BindToObject(NULL, NULL, IID_IBaseFilter, (void **)&pFilter);
                LOG("  ^ bound iSight filter -> %s", HrName(hr));
            }
            pMon->Release();
            pMon = NULL;
            index++;
        }
        if (index == 0)
            LOG("  (no capture devices at all)");
    }

    if (!pFilter)
    {
        LOG("FATAL: our filter is not in the video capture category");
        goto done;
    }

    //---------------------------------------------------------------
    // 2. pin + IAMStreamConfig capability dump
    //---------------------------------------------------------------
    hr = pFilter->EnumPins(&pPins);
    LOG("EnumPins -> %s", HrName(hr));

    if (SUCCEEDED(hr) && pPins)
    {
        IPin *pPin = NULL;
        while (pPins->Next(1, &pPin, NULL) == S_OK)
        {
            PIN_INFO info;
            if (SUCCEEDED(pPin->QueryPinInfo(&info)))
            {
                char pname[128] = "";
                WideCharToMultiByte(CP_ACP, 0, info.achName, -1, pname, sizeof(pname), NULL, NULL);
                LOG("pin \"%s\" dir=%s", pname, info.dir == PINDIR_OUTPUT ? "out" : "in");
                if (info.pFilter) info.pFilter->Release();
                if (info.dir == PINDIR_OUTPUT && !pOutPin)
                    pOutPin = pPin;
                else
                    pPin->Release();
                if (pOutPin) break;
            }
            else
            {
                pPin->Release();
            }
        }
    }

    ProbeFilterInterfaces(pFilter);
    if (pOutPin)
        ProbePinInterfaces(pOutPin);
    ProbePinCategory(pOutPin);

    if (pOutPin)
    {
        hr = pOutPin->QueryInterface(IID_IAMStreamConfig, (void **)&pCfg);
        LOG("QI IAMStreamConfig -> %s", HrName(hr));
    }
    else
    {
        LOG("FATAL: no output pin");
    }

    if (pCfg)
    {
        hr = pCfg->GetNumberOfCapabilities(&nCaps, &cbCaps);
        LOG("GetNumberOfCapabilities -> %s (count=%d cbSize=%d)", HrName(hr), nCaps, cbCaps);

        for (int i = 0; i < nCaps && i < 8; i++)
        {
            AM_MEDIA_TYPE *pmt = NULL;
            VIDEO_STREAM_CONFIG_CAPS caps;
            ZeroMemory(&caps, sizeof(caps));
            hr = pCfg->GetStreamCaps(i, &pmt, (BYTE *)&caps);
            if (SUCCEEDED(hr) && pmt)
            {
                char tag[32];
                _snprintf_s(tag, sizeof(tag), _TRUNCATE, "cap[%d]", i);
                DescribeMediaType(tag, pmt);
                LOG("       interval=[%lld..%lld] output=%ldx%ld bps=[%lu..%lu]",
                    (long long)caps.MinFrameInterval, (long long)caps.MaxFrameInterval,
                    (long)caps.MinOutputSize.cx, (long)caps.MinOutputSize.cy,
                    (unsigned long)caps.MinBitsPerSecond, (unsigned long)caps.MaxBitsPerSecond);
                DeleteMediaTypeLocal(pmt);
            }
            else
            {
                LOG("cap[%d] -> %s", i, HrName(hr));
            }
        }

        //-----------------------------------------------------------
        // 3. SetFormat probes: what a host application actually does
        //-----------------------------------------------------------
        hr = pCfg->SetFormat(NULL);
        LOG("SetFormat(NULL) -> %s", HrName(hr));

        AM_MEDIA_TYPE *pmt0 = NULL;
        VIDEO_STREAM_CONFIG_CAPS c0;
        ZeroMemory(&c0, sizeof(c0));
        if (nCaps > 0 && SUCCEEDED(pCfg->GetStreamCaps(0, &pmt0, (BYTE *)&c0)) && pmt0)
        {
            hr = pCfg->SetFormat(pmt0);
            LOG("SetFormat(own cap[0]) -> %s", HrName(hr));

            // many hosts ask for 30 fps and expect the filter to cope
            VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER *)pmt0->pbFormat;
            REFERENCE_TIME saved = pvi->AvgTimePerFrame;
            pvi->AvgTimePerFrame = 333667;
            hr = pCfg->SetFormat(pmt0);
            LOG("SetFormat(cap[0] @30fps) -> %s", HrName(hr));

            // and hosts may ask for a completely bogus size
            pvi->AvgTimePerFrame = saved;
            LONG savedW = pvi->bmiHeader.biWidth;
            pvi->bmiHeader.biWidth = 320;
            hr = pCfg->SetFormat(pmt0);
            LOG("SetFormat(cap[0] 320px) -> %s (expected VFW_E_INVALIDMEDIATYPE)", HrName(hr));
            pvi->bmiHeader.biWidth = savedW;

            DeleteMediaTypeLocal(pmt0);
        }

        AM_MEDIA_TYPE *pCur = NULL;
        if (SUCCEEDED(pCfg->GetFormat(&pCur)) && pCur)
        {
            DescribeMediaType("GetFormat", pCur);
            DeleteMediaTypeLocal(pCur);
        }
    }

    //---------------------------------------------------------------
    // 4. build a real graph: filter -> NullRenderer
    //---------------------------------------------------------------
    for (int round = 1; round <= 2; round++)
    {
        LOG("----- streaming round %d -----", round);

        IGraphBuilder *pGraph = NULL;
        hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                              IID_IGraphBuilder, (void **)&pGraph);
        if (FAILED(hr) || !pGraph)
        {
            LOG("create graph -> %s", HrName(hr));
            break;
        }

        hr = pGraph->AddFilter(pFilter, L"iSight");
        LOG("AddFilter -> %s", HrName(hr));

        IBaseFilter *pNull = NULL;
        hr = CoCreateInstance(CLSID_NullRendererLocal, NULL, CLSCTX_INPROC_SERVER,
                              IID_IBaseFilter, (void **)&pNull);
        LOG("create NullRenderer -> %s", HrName(hr));

        bool connected = false;
        if (SUCCEEDED(hr) && pNull)
        {
            hr = pGraph->AddFilter(pNull, L"Null");
            LOG("AddFilter(NullRenderer) -> %s", HrName(hr));

            IPin *pIn = NULL;
            const char *nm = "";
            if (FindInputPin(pNull, &pIn, &nm) && pIn && pOutPin)
            {
                hr = pGraph->Connect(pOutPin, pIn);
                LOG("Connect(capture -> NullRenderer) -> %s", HrName(hr));
                connected = SUCCEEDED(hr);
                pIn->Release();
            }
            else
            {
                LOG("FATAL: NullRenderer has no input pin");
            }
        }

        if (!connected && pOutPin)
        {
            LOG("Connect failed, retrying with intelligent connect (RenderStream)");
            hr = pGraph->Render(pOutPin);
            LOG("Render(pin) -> %s", HrName(hr));
            connected = SUCCEEDED(hr);
        }

        if (connected)
        {
            AM_MEDIA_TYPE mt;
            ZeroMemory(&mt, sizeof(mt));
            if (pOutPin && SUCCEEDED(pOutPin->ConnectionMediaType(&mt)))
            {
                DescribeMediaType("negotiated", &mt);
                FreeMediaTypeLocal(mt);
            }

            IMediaControl *pCtl = NULL;
            IMediaEvent   *pEvt = NULL;
            pGraph->QueryInterface(IID_IMediaControl, (void **)&pCtl);
            pGraph->QueryInterface(IID_IMediaEvent, (void **)&pEvt);

            if (pCtl)
            {
                // A NullRenderer paces itself on the graph clock.  Without
                // one it never asks the source for a sample at all -- that
                // is how an earlier run managed to report "0 frames
                // delivered" while WeChat was pulling 811 frames from the
                // very same filter minutes later.
                IReferenceClock *pClock = NULL;
                hr = CoCreateInstance(CLSID_SystemClockLocal, NULL, CLSCTX_INPROC_SERVER,
                                      __uuidof(IReferenceClock), (void **)&pClock);
                LOG("create SystemClock -> %s", HrName(hr));
                if (SUCCEEDED(hr) && pClock)
                {
                    IMediaFilter *pMf = NULL;
                    if (SUCCEEDED(pGraph->QueryInterface(__uuidof(IMediaFilter), (void **)&pMf)) && pMf)
                    {
                        hr = pMf->SetSyncSource(pClock);
                        LOG("SetSyncSource(SystemClock) -> %s", HrName(hr));
                        pMf->Release();
                    }
                    pClock->Release();
                }

                hr = pCtl->Run();
                LOG("Run -> %s", HrName(hr));
                PumpFor(round == 1 ? 15000 : 10000);
                DrainEvents(pEvt);

                OAFilterState state = State_Stopped;
                pCtl->GetState(1000, &state);
                LOG("state after streaming = %s", state == State_Running ? "Running" :
                                                  state == State_Paused  ? "Paused" : "Stopped");

                hr = pCtl->Stop();
                LOG("Stop -> %s", HrName(hr));
                PumpFor(300);
                pCtl->Release();
            }
            if (pEvt) pEvt->Release();
        }

        if (pNull) pNull->Release();
        pGraph->Release();
    }

    //---------------------------------------------------------------
    // 5. the mainstream host path: ICaptureGraphBuilder2::RenderStream on
    //    PIN_CATEGORY_CAPTURE -- exactly what WeChat / QQ / OBS execute.
    //    It also exercises live-source detection (IAMFilterMiscFlags),
    //    because that is what makes the builder insert a Smart Tee.
    //---------------------------------------------------------------
    {
        LOG("----- capture-graph-builder round -----");

        IGraphBuilder *pGraph = NULL;
        ICaptureGraphBuilder2 *pBuild = NULL;
        IBaseFilter *pNull = NULL;

        hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                              IID_IGraphBuilder, (void **)&pGraph);
        LOG("create graph -> %s", HrName(hr));

        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER,
                              __uuidof(ICaptureGraphBuilder2), (void **)&pBuild);
        LOG("create CaptureGraphBuilder2 -> %s", HrName(hr));

        if (pGraph && pBuild)
        {
            hr = pBuild->SetFiltergraph(pGraph);
            LOG("SetFiltergraph -> %s", HrName(hr));

            hr = pGraph->AddFilter(pFilter, L"iSight");
            LOG("AddFilter -> %s", HrName(hr));

            hr = CoCreateInstance(CLSID_NullRendererLocal, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IBaseFilter, (void **)&pNull);
            if (SUCCEEDED(hr) && pNull)
            {
                hr = pBuild->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                          pFilter, NULL, pNull);
                LOG("RenderStream(CAPTURE, Video, iSight, NULL, NullRenderer) -> %s", HrName(hr));

                if (SUCCEEDED(hr))
                {
                    IMediaControl *pCtl = NULL;
                    IMediaEvent   *pEvt = NULL;
                    pGraph->QueryInterface(IID_IMediaControl, (void **)&pCtl);
                    pGraph->QueryInterface(IID_IMediaEvent, (void **)&pEvt);
                    if (pCtl)
                    {
                        hr = pCtl->Run();
                        LOG("Run -> %s", HrName(hr));
                        PumpFor(12000);
                        DrainEvents(pEvt);
                        hr = pCtl->Stop();
                        LOG("Stop -> %s", HrName(hr));
                        PumpFor(300);
                        pCtl->Release();
                    }
                    if (pEvt) pEvt->Release();
                }
            }
            else
            {
                LOG("create NullRenderer -> %s", HrName(hr));
            }
        }

        if (pNull)  pNull->Release();
        if (pBuild) pBuild->Release();
        if (pGraph) pGraph->Release();
    }

done:
    ReportFilterLog();
    LOG("=== done (see also %%LOCALAPPDATA%%\\iSightCam.log for the filter trace) ===");
    if (g_log) { fclose(g_log); g_log = NULL; }
    printf("\nPress Enter to exit...");
    getchar();
    return 0;
}
