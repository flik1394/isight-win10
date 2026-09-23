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
#include <newdev.h>     // UpdateDriverForPlugAndPlayDevicesW + INSTALLFLAG_FORCE
#include <devguid.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")

// {65E8773D-8F56-11D0-A3B9-00A0C9223196}
static const GUID KSCATEGORY_CAPTURE = {
    0x65E8773D, 0x8F56, 0x11D0, {0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}
};

#define HWID  L"ISIGHTMIC\\Mic"

#define ISIGHT_MICDEV_TAG "ISIGHT-MICDEV-BUILD-V21-20260923-ROOTID"

// The string handed to SetupDiCreateDeviceInfo when DICD_GENERATE_ID is set is
// NOT the hardware ID.  It has to be a *root-enumerated device ID*: no
// "Enumerator\" prefix and no instance suffix (the docs' example is "*PNP0500").
// Passing the hardware ID "ISIGHTMIC\Mic" fails with
//     0xE0000205  SPAPI_E_INVALID_DEVINST_NAME
// because the backslash makes it look like an instance ID whose enumerator is
// "ISIGHTMIC" -- an enumerator that does not exist.  That is exactly the error
// the v20 package died on.
//
// Keep a few spellings and take the first one Windows accepts, so the exact
// accepted shape never has to be guessed again; the winner is printed.
static const wchar_t *kRootDeviceIds[] = {
    L"iSightMic",        // bare root device ID -- the documented form
    L"*ISIGHTMIC",       // the shape the docs use for their example
    L"Root\\iSightMic",  // the prefixed form older samples pass around
    L"ISIGHTMIC\\Mic",   // the hardware ID; known to be rejected, kept as a witness
    NULL
};

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
    wprintf(L"tool: %hs\n", ISIGHT_MICDEV_TAG);

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

    // 3. the device information element.  Pick the first root device ID that
    //    Windows will take (see kRootDeviceIds above).
    SP_DEVINFO_DATA did;
    const wchar_t *devId = NULL;
    DWORD lastErr = 0;
    for (int i = 0; kRootDeviceIds[i]; i++) {
        ZeroMemory(&did, sizeof(did));
        did.cbSize = sizeof(did);
        SetLastError(0);
        if (SetupDiCreateDeviceInfoW(h, kRootDeviceIds[i], &classGuid, NULL, NULL,
                                     DICD_GENERATE_ID, &did)) {
            devId = kRootDeviceIds[i];
            break;
        }
        lastErr = GetLastError();
        wprintf(L"  %-28s \"%s\" rejected (0x%08lx)\n",
                L"create device info", kRootDeviceIds[i], lastErr);
    }
    if (!devId) {
        wprintf(L"  %-28s no root device ID was accepted\n", L"create device info");
        if (lastErr == 0xE0000205)
            wprintf(L"        0xE0000205 is SPAPI_E_INVALID_DEVINST_NAME: with\n"
                    L"        DICD_GENERATE_ID the name must be a bare root device ID,\n"
                    L"        so a \"ISIGHTMIC\\\" prefix is not allowed here.\n");
        SetupDiDestroyDeviceInfoList(h);
        return 4;
    }

    wchar_t instId[512] = L"";
    SetupDiGetDeviceInstanceIdW(h, &did, instId, 511, NULL);
    wprintf(L"  %-28s ok  %s  (from \"%s\")\n",
            L"create device info", instId, devId);

    // 4. the hardware ID the INF's [Models] section is keyed on
    wchar_t hwids[64] = HWID;
    hwids[wcslen(HWID) + 1] = L'\0';   // MULTI_SZ: double null
    if (!SetupDiSetDeviceRegistryPropertyW(h, &did, SPDRP_HARDWAREID,
                                           (PBYTE)hwids,
                                           (DWORD)((wcslen(HWID) + 2) * sizeof(wchar_t)))) {
        PrintErr(L"set hardware id", GetLastError());
        SetupDiDestroyDeviceInfoList(h);
        return 5;
    }
    wprintf(L"  %-28s %s\n", L"hardware id", HWID);

    // 5. register the node with PnP, then let PnP itself choose and install the
    //    driver.  This is the order devcon uses for a root-enumerated device:
    //    DIF_REGISTERDEVICE first, UpdateDriverForPlugAndPlayDevices second.
    //    Driving the DIF codes by hand only happens if that fails.
    if (SetupDiCallClassInstaller(DIF_REGISTERDEVICE, h, &did))
        wprintf(L"  %-28s ok\n", L"register device node");
    else
        PrintErr(L"register device node", GetLastError());

    BOOL ok = FALSE;
    BOOL reboot = FALSE;
    if (UpdateDriverForPlugAndPlayDevicesW(
            NULL, HWID, inf,
            INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot)) {
        ok = TRUE;
        wprintf(L"  %-28s ok%s\n", L"install device",
                reboot ? L" -- a reboot was requested" : L"");
    } else {
        PrintErr(L"install device", GetLastError());
        if (SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, h, &did)) {
            wprintf(L"  %-28s ok\n", L"select best driver");
            if (SetupDiCallClassInstaller(DIF_INSTALLDEVICE, h, &did)) {
                ok = TRUE;
                wprintf(L"  %-28s ok\n", L"install device (DIF)");
            } else {
                PrintErr(L"install device (DIF)", GetLastError());
            }
        } else {
            PrintErr(L"select best driver", GetLastError());
        }
    }

    // 6. start it.  PnP normally has already; this is the belt to that braces.
    if (ok) {
        SP_PROPCHANGE_PARAMS pcp;
        ZeroMemory(&pcp, sizeof(pcp));
        pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pcp.StateChange = DICS_ENABLE;
        pcp.Scope = DICS_FLAG_GLOBAL;
        pcp.HwProfile = 0;
        if (SetupDiSetClassInstallParamsW(h, &did, &pcp.ClassInstallHeader, sizeof(pcp)) &&
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, &did))
            wprintf(L"  %-28s ok\n", L"enable device");
    }

    SetupDiDestroyDeviceInfoList(h);

    if (!ok) {
        wprintf(L"\nIf the error above is 0xE0000247 (no matching device) or a signing\n"
                L"error, check: the .cer is in the machine Root + TrustedPublisher stores\n"
                L"and the machine booted once with testsigning on.\n");
        return 6;
    }
    wprintf(L"\niSight virtual microphone installed as %s.\n"
            L"Look for \"iSight Microphone (FireWire)\" in the recording devices list.\n",
            instId);
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
