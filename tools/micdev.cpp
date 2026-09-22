// isight-micdev.exe -- create and remove the iSight virtual microphone device
// node, and list what Windows currently exposes as a capture endpoint.
//
//   isight-micdev.exe install <isightmic.inf>
//   isight-micdev.exe remove
//   isight-micdev.exe list
//
// There is no hardware for PnP to discover, so the device node has to be made
// on purpose: stage the INF into the driver store, create a (root enumerated)
// device info element carrying the ISIGHTMIC\Mic hardware ID, then install the
// driver onto it.  That is what devcon's "install" verb does; we do it here so
// no extra tool has to be downloaded.
//
// Build (user mode): cl /nologo /O2 /W3 /MD /DUNICODE /D_UNICODE tools\micdev.cpp
//                       setupapi.lib newdev.lib /Fe:isight-micdev.exe

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")

// {65E8773D-8F56-11D0-A3B9-00A0C9223196}
static const GUID KSCATEGORY_CAPTURE = {
    0x65E8773D, 0x8F56, 0x11D0, {0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}
};

#define HWID  L"ISIGHTMIC\\Mic"

static void PrintErr(const wchar_t *what, DWORD err) {
    wchar_t *msg = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    wprintf(L"  %-28s FAILED (0x%08lx) %s\n", what, err, msg ? msg : L"");
    if (msg) LocalFree(msg);
}

static int DoInstall(const wchar_t *infArg) {
    wchar_t inf[MAX_PATH];
    if (!GetFullPathNameW(infArg, MAX_PATH, inf, NULL)) {
        wprintf(L"cannot resolve %s\n", infArg);
        return 2;
    }
    if (GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"no such INF: %s\n", inf);
        return 2;
    }
    wprintf(L"INF: %s\n", inf);

    // 1. stage into the driver store (this is what makes the signed .cat count)
    wchar_t dest[MAX_PATH];
    DWORD destSize = MAX_PATH * sizeof(wchar_t);
    if (SetupCopyOEMInfW(inf, NULL, SPOST_PATH, 0, dest, destSize, NULL, NULL)) {
        wprintf(L"  %-28s %s\n", L"staged in driver store", dest);
    } else {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS) {
            wprintf(L"  %-28s already staged\n", L"staged in driver store");
        } else {
            PrintErr(L"stage in driver store", e);
            // keep going: the INF may still be usable by direct path
        }
    }

    // 2. the class the INF wants (Media)
    GUID classGuid = GUID_NULL;
    wchar_t className[256];
    if (!SetupDiGetINFClassW(inf, &classGuid, className,
                             sizeof(className) / sizeof(className[0]), NULL)) {
        PrintErr(L"read INF class", GetLastError());
        classGuid = GUID_DEVCLASS_MEDIA;
        wcscpy_s(className, L"Media");
    }
    wprintf(L"  %-28s %s\n", L"class", className);

    HDEVINFO h = SetupDiCreateDeviceInfoList(&classGuid, NULL);
    if (h == INVALID_HANDLE_VALUE) { PrintErr(L"create device info list", GetLastError()); return 3; }

    SP_DEVINFO_DATA did;
    ZeroMemory(&did, sizeof(did));
    did.cbSize = sizeof(did);
    if (!SetupDiCreateDeviceInfoW(h, L"ISIGHTMIC\\Mic", &classGuid, NULL, NULL,
                                  DICD_GENERATE_ID, &did)) {
        PrintErr(L"create device info", GetLastError());
        SetupDiDestroyDeviceInfoList(h);
        return 4;
    }

    // 3. the hardware ID the INF's [Models] section is keyed on
    wchar_t hwids[64] = HWID;
    hwids[wcslen(HWID) + 1] = L'\0';   // MULTI_SZ: double null
    if (!SetupDiSetDeviceRegistryPropertyW(h, &did, SPDRP_HARDWAREID,
                                           (PBYTE)hwids,
                                           (DWORD)((wcslen(HWID) + 2) * sizeof(wchar_t)))) {
        PrintErr(L"set hardware id", GetLastError());
        SetupDiDestroyDeviceInfoList(h);
        return 5;
    }

    // 4. pick the driver and install it.  DIF_SELECTBESTCOMPATDRV searches the
    //    driver store (where we just staged the INF); DIF_INSTALLDEVICE then
    //    registers the node and starts the driver.
    BOOL ok = SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, h, &did);
    if (!ok) PrintErr(L"select best driver", GetLastError());

    ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, h, &did);
    if (!ok) PrintErr(L"register device node", GetLastError());
    else wprintf(L"  %-28s ok\n", L"register device node");

    ok = SetupDiCallClassInstaller(DIF_INSTALLDEVICE, h, &did);
    if (!ok) {
        DWORD e = GetLastError();
        PrintErr(L"install device", e);
        // second chance: hand the hardware ID to PnP directly
        BOOL reboot = FALSE;
        if (UpdateDriverForPlugAndPlayDevicesW(NULL, HWID, inf, INSTALLFLAG_FORCE, &reboot)) {
            wprintf(L"  %-28s ok\n", L"install (PnP update)");
            ok = TRUE;
        } else {
            PrintErr(L"install (PnP update)", GetLastError());
        }
    } else {
        wprintf(L"  %-28s ok\n", L"install device");
    }

    // 5. start it
    if (ok) {
        if (SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, &did)) {
            wprintf(L"  %-28s ok\n", L"start device");
        }
    }

    SetupDiDestroyDeviceInfoList(h);

    if (!ok) {
        wprintf(L"\nIf the error above is 0xE0000247 (no matching device) or a signing\n"
                L"error, check: the .cer is in the machine Root + TrustedPublisher stores\n"
                L"and the machine booted once with testsigning on.\n");
        return 6;
    }
    wprintf(L"\niSight virtual microphone installed.  Look for\n"
            L"\"iSight Microphone (FireWire)\" in the recording devices list.\n");
    return 0;
}

static int DoRemove() {
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVCLASS_MEDIA, NULL, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) { PrintErr(L"enum media devices", GetLastError()); return 3; }
    int removed = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVINFO_DATA did;
        ZeroMemory(&did, sizeof(did));
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInfo(h, i, &did)) break;
        wchar_t ids[512];
        if (SetupDiGetDeviceRegistryPropertyW(h, &did, SPDRP_HARDWAREID, NULL,
                                              (PBYTE)ids, sizeof(ids), NULL)) {
            for (const wchar_t *p = ids; *p; p += wcslen(p) + 1) {
                if (_wcsicmp(p, HWID) == 0) {
                    wchar_t name[256] = L"";
                    SetupDiGetDeviceRegistryPropertyW(h, &did, SPDRP_FRIENDLYNAME, NULL,
                                                      (PBYTE)name, sizeof(name), NULL);
                    wprintf(L"removing %s\n", name[0] ? name : HWID);
                    SP_REMOVEDEVICE_PARAMS rp;
                    ZeroMemory(&rp, sizeof(rp));
                    rp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                    rp.ClassInstallHeader.InstallFunction = DIF_REMOVE;
                    rp.Scope = DI_REMOVEDEVICE_GLOBAL;
                    rp.HwProfile = 0;
                    if (SetupDiSetClassInstallParamsW(h, &did, &rp.ClassInstallHeader, sizeof(rp)) &&
                        SetupDiCallClassInstaller(DIF_REMOVE, h, &did)) {
                        removed++;
                    } else {
                        PrintErr(L"remove", GetLastError());
                    }
                    break;
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(h);
    wprintf(L"%d device node(s) removed.\n", removed);
    return 0;
}

static int DoList() {
    HDEVINFO h = SetupDiGetClassDevsW(&KSCATEGORY_CAPTURE, NULL, NULL,
                                      DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) { PrintErr(L"enum capture endpoints", GetLastError()); return 3; }
    wprintf(L"capture endpoints Windows exposes right now:\n");
    int n = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA ifd;
        ZeroMemory(&ifd, sizeof(ifd));
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(h, NULL, &KSCATEGORY_CAPTURE, i, &ifd)) break;
        SP_DEVINFO_DATA did;
        ZeroMemory(&did, sizeof(did));
        did.cbSize = sizeof(did);
        wchar_t name[512] = L"";
        SetupDiGetDeviceInterfaceDetailW(h, &ifd, NULL, 0, NULL, &did);
        SetupDiGetDeviceRegistryPropertyW(h, &did, SPDRP_FRIENDLYNAME, NULL,
                                          (PBYTE)name, sizeof(name), NULL);
        if (!name[0])
            SetupDiGetDeviceRegistryPropertyW(h, &did, SPDRP_DEVICEDESC, NULL,
                                              (PBYTE)name, sizeof(name), NULL);
        wprintf(L"  [%d] %s\n", n, name[0] ? name : L"(no name)");
        n++;
    }
    SetupDiDestroyDeviceInfoList(h);
    if (n == 0) wprintf(L"  (none)\n");
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        wprintf(L"usage: isight-micdev.exe install <isightmic.inf> | remove | list\n");
        return 1;
    }
    if (_wcsicmp(argv[1], L"install") == 0 && argc >= 3) return DoInstall(argv[2]);
    if (_wcsicmp(argv[1], L"remove") == 0) return DoRemove();
    if (_wcsicmp(argv[1], L"list") == 0) return DoList();
    wprintf(L"usage: isight-micdev.exe install <isightmic.inf> | remove | list\n");
    return 1;
}
