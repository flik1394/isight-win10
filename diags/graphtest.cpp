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
    if (*g == MEDIASUBTYPE_NV12)  return "NV12";
    if (*g == MEDIASUBTYPE_I420)  return "I420";
    return "other";
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
    long code = 0, p1 = 0, p2 = 0;
    while (pEvent->GetEvent(&code, &p1, &p2, 0) == S_OK)
    {
        LOG("  graph event: code=0x%04X p1=%ld p2=%ld", (unsigned)code, p1, p2);
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
                DeleteMediaType(pmt);
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

            DeleteMediaType(pmt0);
        }

        AM_MEDIA_TYPE *pCur = NULL;
        if (SUCCEEDED(pCfg->GetFormat(&pCur)) && pCur)
        {
            DescribeMediaType("GetFormat", pCur);
            DeleteMediaType(pCur);
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
        hr = CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER,
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
                FreeMediaType(mt);
            }

            IMediaControl *pCtl = NULL;
            IMediaEvent   *pEvt = NULL;
            pGraph->QueryInterface(IID_IMediaControl, (void **)&pCtl);
            pGraph->QueryInterface(IID_IMediaEvent, (void **)&pEvt);

            if (pCtl)
            {
                hr = pCtl->Run();
                LOG("Run -> %s", HrName(hr));
                PumpFor(round == 1 ? 5000 : 3000);
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

done:
    LOG("=== done (see also %%LOCALAPPDATA%%\\iSightCam.log for the filter trace) ===");
    if (g_log) { fclose(g_log); g_log = NULL; }
    printf("\nPress Enter to exit...");
    getchar();
    return 0;
}
