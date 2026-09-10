//=====================================================================
// testcam.cpp - standalone CMU 1394 camera stress/diagnostic tool
//
// Bypasses the DirectShow filter entirely and talks to the CMU
// library directly, so any failure here is at the driver/hardware
// layer, not our filter.
//
// For each frame rate (15 / 30 / 3.75 fps) it:
//   - sets Format 0 / Mode 2 (640x480 YUV422)
//   - starts isoch acquisition
//   - acquires up to 90 frames, timing each call
//   - reports timeouts/errors, stops after 10 consecutive failures
//
// Log is written to stdout AND to isight-diag.log next to the exe.
//=====================================================================

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef MY1394CAMERA_EXPORTS
#define MY1394CAMERA_EXPORTS
#endif

#include "1394camapi.h"
#include "1394Camera.h"

// t1394cmdr custom IOCTLs (1394common.h, CMDR1394_IOCTL_INDEX = 0x0800)
#define CMDR_IOCTL_INDEX          0x0800
#define CMDR_IOCTL_BUS_RESET      CTL_CODE(FILE_DEVICE_UNKNOWN, CMDR_IOCTL_INDEX + 25, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define CMDR_IOCTL_GET_GENERATION CTL_CODE(FILE_DEVICE_UNKNOWN, CMDR_IOCTL_INDEX + 26, METHOD_BUFFERED, FILE_ANY_ACCESS)

static FILE *g_log = NULL;

// ---- bus generation monitor -------------------------------------------
// Polls IOCTL_GET_GENERATION_COUNT on a private handle to the camera PDO.
// Every 1394 bus reset bumps the generation, so a change = a bus reset
// happened at that moment (host or camera initiated).
static HANDLE g_genHandle = INVALID_HANDLE_VALUE;
static HANDLE g_genStop = NULL;
static HANDLE g_genThread = NULL;
static volatile LONG g_genChanges = 0;

static void LOG(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
    va_end(ap);
}

static const char *CamErr(int r)
{
    switch (r)
    {
    case CAM_SUCCESS:                        return "CAM_SUCCESS";
    case CAM_ERROR:                          return "CAM_ERROR";
    case CAM_ERROR_UNSUPPORTED:              return "CAM_ERROR_UNSUPPORTED";
    case CAM_ERROR_NOT_INITIALIZED:          return "CAM_ERROR_NOT_INITIALIZED";
    case CAM_ERROR_INVALID_VIDEO_SETTINGS:   return "CAM_ERROR_INVALID_VIDEO_SETTINGS";
    case CAM_ERROR_BUSY:                     return "CAM_ERROR_BUSY";
    case CAM_ERROR_INSUFFICIENT_RESOURCES:   return "CAM_ERROR_INSUFFICIENT_RESOURCES";
    case CAM_ERROR_PARAM_OUT_OF_RANGE:       return "CAM_ERROR_PARAM_OUT_OF_RANGE";
    case CAM_ERROR_FRAME_TIMEOUT:            return "CAM_ERROR_FRAME_TIMEOUT";
    default:                                 return "?";
    }
}

static double NowMs()
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static DWORD WINAPI GenMonitorThread(LPVOID)
{
    ULONG lastGen = 0xFFFFFFFF;
    while (WaitForSingleObject(g_genStop, 150) == WAIT_TIMEOUT)
    {
        ULONG gen = 0;
        DWORD ret = 0;
        if (!DeviceIoControl(g_genHandle, CMDR_IOCTL_GET_GENERATION, NULL, 0,
                             &gen, sizeof(gen), &ret, NULL))
            continue;
        if (lastGen != 0xFFFFFFFF && gen != lastGen)
        {
            InterlockedIncrement(&g_genChanges);
            LOG("[gen-monitor] t=%.1fs  GENERATION %lu -> %lu   <<< BUS RESET #%d",
                NowMs() / 1000.0, lastGen, gen, g_genChanges);
        }
        lastGen = gen;
    }
    return 0;
}

static void StartGenMonitor(const char *devicePath)
{
    g_genHandle = CreateFileA(devicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (g_genHandle == INVALID_HANDLE_VALUE)
    {
        LOG("gen-monitor: cannot open device (%lu) - monitoring disabled", GetLastError());
        return;
    }
    // sanity check the IOCTL works
    ULONG gen = 0; DWORD ret = 0;
    if (!DeviceIoControl(g_genHandle, CMDR_IOCTL_GET_GENERATION, NULL, 0,
                         &gen, sizeof(gen), &ret, NULL))
    {
        LOG("gen-monitor: GET_GENERATION_COUNT failed (%lu) - monitoring disabled", GetLastError());
        CloseHandle(g_genHandle); g_genHandle = INVALID_HANDLE_VALUE;
        return;
    }
    LOG("gen-monitor: started, generation=%lu", gen);
    g_genStop = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_genThread = CreateThread(NULL, 0, GenMonitorThread, NULL, 0, NULL);
}

static void StopGenMonitor()
{
    if (g_genThread)
    {
        SetEvent(g_genStop);
        WaitForSingleObject(g_genThread, 3000);
        CloseHandle(g_genThread); g_genThread = NULL;
        CloseHandle(g_genStop); g_genStop = NULL;
    }
    if (g_genHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_genHandle); g_genHandle = INVALID_HANDLE_VALUE;
    }
    LOG("gen-monitor: stopped, total bus resets observed=%d", g_genChanges);
}

static void RunRateTest(C1394Camera &cam, unsigned long rate)
{
    static const char *rateName[] = { "1.875fps", "3.75fps", "7.5fps", "15fps", "30fps", "60fps" };

    LOG("");
    LOG("===== rate test: Format 0 / Mode 2 @ %s =====", rateName[rate]);

    if (cam.IsAcquiring())
        cam.StopImageAcquisition();

    int r = cam.SetVideoFormat(0);
    LOG("SetVideoFormat(0)  -> %d (%s)", r, CamErr(r));
    r = cam.SetVideoMode(2);
    LOG("SetVideoMode(2)    -> %d (%s)", r, CamErr(r));
    r = cam.SetVideoFrameRate(rate);
    LOG("SetVideoFrameRate(%lu) -> %d (%s)", rate, r, CamErr(r));

    LONG qpp = dc1394GetQuadletsPerPacket(0, 2, rate);
    LOG("quadlets/packet = %ld  (~%ld bytes/packet)", qpp, qpp * 4);

    cam.UpdateParameters(TRUE);

    unsigned long w = 0, h = 0;
    cam.GetVideoFrameDimensions(&w, &h);
    LOG("frame dims = %lux%lu", w, h);

    r = cam.StartImageAcquisitionEx(8, 2000, ACQ_START_VIDEO_STREAM);
    LOG("StartImageAcquisitionEx -> %d (%s)  GetLastError=%lu", r, CamErr(r), GetLastError());
    if (r != CAM_SUCCESS)
        return;

    const int kMaxFrames = 90;
    int ok = 0, timeouts = 0, errors = 0, consecFail = 0, dropped_total = 0;
    double t0 = NowMs();

    for (int i = 0; i < kMaxFrames; i++)
    {
        int dropped = 0;
        double ta = NowMs();
        int ra = cam.AcquireImageEx(TRUE, &dropped);
        double dt = NowMs() - ta;
        dropped_total += dropped;

        if (ra == CAM_SUCCESS)
        {
            ok++; consecFail = 0;
            if (i < 5 || (i % 15) == 0)
                LOG("frame %3d OK   %6.1f ms   dropped_total=%d", i, dt, dropped_total);
        }
        else
        {
            consecFail++;
            if (ra == CAM_ERROR_FRAME_TIMEOUT) timeouts++; else errors++;
            LOG("frame %3d FAIL %6.1f ms   %s (GetLastError=%lu)  consec=%d",
                i, dt, CamErr(ra), GetLastError(), consecFail);
            if (consecFail >= 10)
            {
                LOG("10 consecutive failures - aborting this rate");
                break;
            }
        }
    }

    double elapsed = NowMs() - t0;
    LOG("rate %s result: ok=%d timeouts=%d errors=%d dropped=%d elapsed=%.0fms effective=%.1ffps",
        rateName[rate], ok, timeouts, errors, dropped_total, elapsed,
        elapsed > 0 ? (ok * 1000.0 / elapsed) : 0.0);

    r = cam.StopImageAcquisition();
    LOG("StopImageAcquisition -> %d (%s)", r, CamErr(r));
    Sleep(1000);   // let the bus settle between tests

    // aliveness check: did this rate kill the camera?
    LOG("aliveness: CheckLink=%d", cam.CheckLink());
    int ri = cam.InitCamera(FALSE);
    LOG("aliveness: InitCamera -> %d (%s)", ri, CamErr(ri));

    // post-mortem experiment: does a software bus reset revive the camera?
    if (ri != CAM_SUCCESS && g_genHandle != INVALID_HANDLE_VALUE)
    {
        LOG("post-mortem: issuing software BUS_RESET to test if camera revives...");
        DWORD ret = 0;
        BOOL br = DeviceIoControl(g_genHandle, CMDR_IOCTL_BUS_RESET, NULL, 0,
                                  NULL, 0, &ret, NULL);
        LOG("post-mortem: BUS_RESET -> %d (GetLastError=%lu)", br, GetLastError());
        Sleep(3000);
        LOG("post-mortem: CheckLink=%d", cam.CheckLink());
        ri = cam.InitCamera(FALSE);
        LOG("post-mortem: InitCamera after bus reset -> %d (%s)", ri, CamErr(ri));
    }
}

int main(int argc, char **argv)
{
    char logpath[MAX_PATH];
    if (GetModuleFileNameA(NULL, logpath, MAX_PATH))
    {
        char *slash = strrchr(logpath, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - logpath), "isight-diag.log");
    }
    g_log = fopen("isight-diag.log", "w");
    if (!g_log && logpath[0]) g_log = fopen(logpath, "w");

    LOG("=== iSight CMU diagnostic %s ===", __TIMESTAMP__);

    C1394Camera cam;

    int n = cam.RefreshCameraList();
    LOG("RefreshCameraList -> %d cameras", n);
    if (n <= 0)
    {
        LOG("NO CAMERA FOUND - check CMU driver install / cable");
        if (g_log) fclose(g_log);
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    char name[256] = "", vendor[256] = "";
    LARGE_INTEGER uid = {};
    cam.SelectCamera(0);
    cam.GetCameraName(name, 256);
    cam.GetCameraVendor(vendor, 256);
    cam.GetCameraUniqueID(&uid);
    LOG("camera: vendor='%s' name='%s' uid=0x%08X%08X", vendor, name,
        uid.HighPart, uid.LowPart);
    LOG("device path: %s", cam.GetDevicePath());

    // CheckLink re-enumerates and clears init state, so call it BEFORE InitCamera
    LOG("CheckLink -> %d", cam.CheckLink());
    cam.SelectCamera(0);

    int r = cam.InitCamera(FALSE);
    LOG("InitCamera(FALSE) -> %d (%s)", r, CamErr(r));
    if (r != CAM_SUCCESS)
    {
        if (g_log) fclose(g_log);
        printf("\nPress Enter to exit...");
        getchar();
        return 2;
    }

    ULONG speed = 0;
    LOG("Has1394b=%d Status1394b=%d HasPowerControl=%d",
        (int)cam.Has1394b(), (int)cam.Status1394b(), (int)cam.HasPowerControl());
    LOG("GetMaxSpeed -> %d Mbps", cam.GetMaxSpeed());

    StartGenMonitor(cam.GetDevicePath());

    // rate selection from command line: 15 | 375 | 30 | all (default 15)
    unsigned long rateArg = 3;
    bool doRun = true;
    if (argc > 1)
    {
        if (!_stricmp(argv[1], "375")) rateArg = 1;
        else if (!_stricmp(argv[1], "30")) rateArg = 4;
        else if (!_stricmp(argv[1], "all")) { doRun = false; RunRateTest(cam, 3); RunRateTest(cam, 1); RunRateTest(cam, 4); }
    }
    if (doRun)
        RunRateTest(cam, rateArg);

    if (cam.IsAcquiring())
        cam.StopImageAcquisition();

    StopGenMonitor();

    LOG("");
    LOG("=== done ===");
    if (g_log) fclose(g_log);
    printf("\nPress Enter to exit...");
    getchar();
    return 0;
}
