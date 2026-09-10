//=====================================================================
// testraw.cpp - raw async-read probe for the CMU t1394cmdr device
//
// 1. Enumerates t1394cmdr device paths directly
// 2. Tries ReadRegister at several offsets: CSR-relative (0x000/0x100/0x400)
//    and absolute (0xF0000400 bus info block) to distinguish
//    "camera not responding" from "stale CSR offset after bus reset"
// 3. Retries in a loop for ~40s so it can catch a camera that comes
//    back after a bus reset / power cycle
//=====================================================================

#include <windows.h>
#include <stdio.h>
#include <time.h>

#ifndef MY1394CAMERA_EXPORTS
#define MY1394CAMERA_EXPORTS
#endif

#include "1394camapi.h"

static FILE *g_log = NULL;
static void LOG(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap); printf("\n");
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); fprintf(g_log, "\n"); fflush(g_log); }
    va_end(ap);
}

int main(void)
{
    g_log = fopen("isight-raw.log", "w");
    LOG("=== raw read probe %s ===", __TIMESTAMP__);

    // 1. enumerate device paths
    HDEVINFO h = t1394CmdrGetDeviceList();
    char path[512] = "";
    if (h != INVALID_HANDLE_VALUE)
    {
        char buf[512]; ULONG sz = 0;
        for (DWORD i = 0; i < 8; i++)
        {
            ZeroMemory(buf, sizeof(buf));
            sz = sizeof(buf);
            if (t1394CmdrGetDevicePath(h, i, buf, &sz) > 0 && buf[0])
            {
                LOG("t1394cmdr device[%lu]: %s", (unsigned long)i, buf);
                if (!path[0]) strcpy_s(path, sizeof(path), buf);
            }
        }
        SetupDiDestroyDeviceInfoList(h);
    }
    else
    {
        LOG("t1394CmdrGetDeviceList failed, GetLastError=%lu", GetLastError());
    }
    if (!path[0])
    {
        LOG("no device path found - aborting");
        if (g_log) fclose(g_log);
        return 1;
    }

    // 2. characterize reads
    struct { ULONG off; const char *what; } probes[] =
    {
        { 0xF0000400, "abs bus info block (first quadlet, 0x04040302-ish)" },
        { 0xF000040C, "abs vendor magic ('Apple' = 0x4170706C)" },
        { 0x000,      "rel CSR quadrant 0" },
        { 0x100,      "rel ROM: root directory header" },
        { 0x400,      "rel ROM: unit dep vendor (offset 0xC + 0x400?)" },
        { 0x404,      "rel ROM: unit dep model" },
    };

    // 3. retry loop: catches recovery after unplug/replug
    for (int attempt = 1; attempt <= 12; attempt++)
    {
        LOG("--- attempt %d ---", attempt);
        int anyOK = 0;
        for (int p = 0; p < 6; p++)
        {
            unsigned char b[4] = {0,0,0,0};
            DWORD r = ReadRegister(path, probes[p].off, b);
            if (r == 0)
            {
                anyOK++;
                LOG("  Read 0x%08X %-50s OK  [%02X %02X %02X %02X]",
                    probes[p].off, probes[p].what, b[0], b[1], b[2], b[3]);
            }
            else
            {
                LOG("  Read 0x%08X %-50s ERR 0x%08X (GetLastError=%lu)",
                    probes[p].off, probes[p].what, r, GetLastError());
            }
        }
        if (anyOK == 6)
        {
            LOG(">>> ALL READS OK - camera alive!");
            break;
        }
        Sleep(3000);
    }

    LOG("=== done ===");
    if (g_log) fclose(g_log);
    printf("\nPress Enter to exit...");
    getchar();
    return 0;
}
