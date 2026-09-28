// pinprobe.cpp -- dump KSPROPSETID_Pin basics for our wave filter side by side
// with a known-good capture filter on the same machine.
//
// Why: miccheck's FindStreamingCapturePin() reports "[OURS] no OUT/SINK
// streaming pin" for iSightMic in EVERY one of the nine V46 switch combos,
// even though its own [2b] listing shows pin 1 is dataflow=OUT, comm=SINK.
// That routine requires three things at once:
//     df == KSPIN_DATAFLOW_OUT
//     cat.Data1 == 0xFB6C4281      (PIN_CATEGORY_CAPTURE)
//     comm == SINK or BOTH
// So either the category query fails outright, or it returns something else.
// Both of those would also stop the audio engine from treating the pin as a
// capture source.  This tool prints the raw answer, including the Win32 error
// when a property query does not succeed.
//
// Build (normal cmd, WDK 10.0.26100.0):
//   cl /nologo /O2 /DUNICODE /D_UNICODE pinprobe.cpp ^
//      /I "%WDK%\Include\10.0.26100.0\shared" /I "%WDK%\Include\10.0.26100.0\um" ^
//      /link /LIBPATH:"%WDK%\Lib\10.0.26100.0\um\x64"
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ksguid.lib")
#pragma comment(lib, "ole32.lib")

static void PrintGuid(const GUID* g) {
    if (!g) { printf("<null>"); return; }
    printf("%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           g->Data1, g->Data2, g->Data3,
           g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
           g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static BOOL KsGet(HANDLE f, ULONG pinId, ULONG propId, void* out, DWORD outLen) {
    KSP_PIN kp;
    ZeroMemory(&kp, sizeof(kp));
    kp.Property.Set   = KSPROPSETID_Pin;
    kp.Property.Id    = propId;
    kp.Property.Flags = KSPROPERTY_TYPE_GET;
    kp.PinId          = pinId;
    DWORD got = 0;
    return DeviceIoControl(f, IOCTL_KS_PROPERTY, &kp, sizeof(kp),
                           out, outLen, &got, NULL);
}

static void DumpFilter(const WCHAR* path, const char* tag) {
    // shorten the symlink for printing
    char shortPath[256] = "";
    {
        char mb[1024] = "";
        WideCharToMultiByte(CP_UTF8, 0, path, -1, mb, sizeof(mb), NULL, NULL);
        const char* p = strstr(mb, "isightmic");
        if (!p) p = strstr(mb, "rtmicin");
        if (!p) p = mb;
        strncpy_s(shortPath, sizeof(shortPath), p, _TRUNCATE);
    }
    printf("\n=== [%s] %s\n", tag, shortPath);

    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        printf("  open FAILED err=%u\n", GetLastError());
        return;
    }

    ULONG ctypes = 0;
    if (!KsGet(f, 0, KSPROPERTY_PIN_CTYPES, &ctypes, sizeof(ctypes))) {
        printf("  CTYPES FAILED err=%u\n", GetLastError());
        CloseHandle(f);
        return;
    }
    printf("  pin count: %u\n", ctypes);

    for (ULONG id = 0; id < ctypes; id++) {
        ULONG df = 0, comm = 0, nec = 0;
        GUID cat; ZeroMemory(&cat, sizeof(cat));
        BOOL okDf   = KsGet(f, id, KSPROPERTY_PIN_DATAFLOW, &df, sizeof(df));
        DWORD eDf   = GetLastError();
        BOOL okCm   = KsGet(f, id, KSPROPERTY_PIN_COMMUNICATION, &comm, sizeof(comm));
        DWORD eCm   = GetLastError();
        BOOL okCat  = KsGet(f, id, KSPROPERTY_PIN_CATEGORY, &cat, sizeof(cat));
        DWORD eCat  = GetLastError();
        BOOL okNec  = KsGet(f, id, KSPROPERTY_PIN_NECESSARYINSTANCES, &nec, sizeof(nec));
        DWORD eNec  = GetLastError();

        printf("  pin %u:\n", id);
        if (okDf) printf("     DATAFLOW       = %s\n",
                         df == KSPIN_DATAFLOW_IN ? "IN" : "OUT");
        else      printf("     DATAFLOW       = QUERY FAILED err=%u\n", eDf);

        const char* cmName = "?";
        if (okCm) {
            switch (comm) {
            case KSPIN_COMMUNICATION_NONE: cmName = "NONE"; break;
            case KSPIN_COMMUNICATION_SINK: cmName = "SINK"; break;
            case KSPIN_COMMUNICATION_SOURCE: cmName = "SOURCE"; break;
            case KSPIN_COMMUNICATION_BOTH:  cmName = "BOTH";  break;
            case KSPIN_COMMUNICATION_BRIDGE: cmName = "BRIDGE"; break;
            default: cmName = "other"; break;
            }
            printf("     COMMUNICATION  = %s (%u)\n", cmName, comm);
        } else {
            printf("     COMMUNICATION  = QUERY FAILED err=%u\n", eCm);
        }

        if (okCat) {
            printf("     CATEGORY       = "); PrintGuid(&cat);
            printf("  %s\n", cat.Data1 == 0xFB6C4281 ? "<-- PIN_CATEGORY_CAPTURE"
                                                      : "(NOT capture)");
        } else {
            printf("     CATEGORY       = QUERY FAILED err=%u\n", eCat);
        }

        if (okNec) printf("     NECESSARYINST  = %u\n", nec);
        else       printf("     NECESSARYINST  = QUERY FAILED err=%u\n", eNec);
    }
    CloseHandle(f);
}

int main(void) {
    HDEVINFO set = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed err=%u\n", GetLastError());
        return 1;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    for (DWORD i = 0; i < 128; i++) {
        SP_DEVICE_INTERFACE_DATA di;
        di.cbSize = sizeof(di);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &KSCATEGORY_AUDIO, i, &di))
            break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &di, NULL, 0, &need, NULL);
        if (!need) continue;
        BYTE* buf = (BYTE*)malloc(need);
        if (!buf) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* d =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &di, d, need, NULL, NULL)) {
            free(buf);
            continue;
        }
        // only the two we care about: our filter and a working Realtek capture
        if (wcsstr(d->DevicePath, L"isightmic"))
            DumpFilter(d->DevicePath, "OURS ");
        else if (wcsstr(d->DevicePath, L"rtmicin"))
            DumpFilter(d->DevicePath, "REF  ");
        free(buf);
    }
    SetupDiDestroyDeviceInfoList(set);
    CoUninitialize();
    return 0;
}
