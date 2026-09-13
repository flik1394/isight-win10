//=====================================================================
// audio.cpp - Apple iSight audio (microphone) bring-up probe
//
// The iSight's microphone is NOT an AV/C audio subunit and is not
// AM824/IEC 61883-6, so neither avcaudio.sys nor the in-box 61883 stack
// can drive it.  It is a second unit directory on the same 1394 node
// (specifier 0x000A27 = Apple OUI, version 0x000010) whose registers sit
// at their own CSR offset, and it streams a private isochronous packet
// format: a 16-byte header whose second quadlet is the ASCII "sght",
// followed by 48 kHz / 16-bit big-endian stereo samples.
// (Protocol taken from the Linux driver sound/firewire/isight.c.)
//
// This tool uses only the user-mode raw 1394 API that ships with the CMU
// driver (ReadRegisterUL / WriteRegisterUL / ISOCH_SETUP_STREAM), so it
// needs no kernel work to find out whether the microphone is reachable.
//
//   isight-audio.exe sweep
//       - enumerate, query bus resources,
//       - read every quadlet 0x000..0xFFC and decode the config ROM
//         (root directory + every unit directory + CSR_OFFSET),
//       - locate the Apple audio unit and dump its registers,
//       - if the ROM is not reachable, scan for the gain-range fingerprint.
//
//   isight-audio.exe cap <baseHex> [seconds] [channel]
//       - program the audio unit (48 kHz, iso tx config with our channel),
//       - set up an isochronous receive, dump the raw packets to
//         isight-audio-raw.bin and report the "sght" packets found.
//
// Everything is printed to stdout and to isight-audio.txt next to the exe.
//=====================================================================

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef MY1394CAMERA_EXPORTS
#define MY1394CAMERA_EXPORTS
#endif
#include "1394camapi.h"

//---------------------------------------------------------------------
// iSight audio registers, offsets from the audio unit's base address
//---------------------------------------------------------------------
#define A_AUDIO_ENABLE    0x000
#define A_DEF_GAIN        0x204
#define A_GAIN_RAW_START  0x210
#define A_GAIN_RAW_END    0x214
#define A_GAIN_DB_START   0x218
#define A_GAIN_DB_END     0x21c
#define A_RATE_INQUIRY    0x280
#define A_ISO_TX_CONFIG   0x300
#define A_SAMPLE_RATE     0x400
#define A_GAIN            0x500
#define A_MUTE            0x504

#define A_RATE_48000      0x80000000u
#define A_MAX_FRAMES      475
#define A_HEADER_BYTES    16
#define A_PAYLOAD_BYTES   (A_HEADER_BYTES + A_MAX_FRAMES * 4)   // 1916
#define A_SIGNATURE       0x73676874u                           // "sght"

// the four quadlets that make up the gain-range fingerprint
static const ULONG kGainOffs[4] = { A_GAIN_RAW_START, A_GAIN_RAW_END,
                                    A_GAIN_DB_START,  A_GAIN_DB_END };

static FILE *g_log = NULL;

static void LOG(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); printf("\n"); va_end(ap);
    if (g_log)
    {
        va_start(ap, fmt); vfprintf(g_log, fmt, ap); fprintf(g_log, "\n"); fflush(g_log); va_end(ap);
    }
}

static double NowMs(void)
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static DWORD RD(const char *dev, ULONG off, ULONG *v)
{
    ULONG tmp = 0;
    DWORD r = ReadRegisterUL((PSTR)dev, off, &tmp);
    if (v) *v = tmp;
    return r;
}

static DWORD WR(const char *dev, ULONG off, ULONG v)
{
    return WriteRegisterUL((PSTR)dev, off, v);
}

//---------------------------------------------------------------------
// the 0x000..0xFFC quadlet sweep
//---------------------------------------------------------------------
#define SW_N 0x400                       // 1024 quadlets = 4 KB
static ULONG g_sw[SW_N];
static BYTE  g_swOk[SW_N];

static int DoSweep(const char *dev)
{
    int ok = 0;
    for (int i = 0; i < SW_N; ++i)
    {
        ULONG v = 0;
        DWORD r = RD(dev, (ULONG)i * 4, &v);
        g_swOk[i] = (r == ERROR_SUCCESS) ? 1 : 0;
        g_sw[i]   = v;
        if (g_swOk[i]) ok++;
    }
    LOG("sweep: %d of %d quadlets read OK (offsets 0x000-0xFFC)", ok, SW_N);

    for (int i = 0; i < SW_N; i += 4)
        LOG("  %04X: %08X %08X %08X %08X", i * 4,
            g_sw[i], g_sw[i + 1], g_sw[i + 2], g_sw[i + 3]);
    return ok;
}

//---------------------------------------------------------------------
// config ROM: bus info block, root directory, unit directories
//---------------------------------------------------------------------
static int  g_romWord = -1;     // word index of the bus info block word 0
static int  g_romSwap = 0;      // 1 = the driver hands quads over byte-swapped

#define ROMW(k)  (g_romSwap ? _byteswap_ulong(g_sw[g_romWord + (k)]) : g_sw[g_romWord + (k)])

static int ParseRom(void)
{
    for (int i = 1; i < SW_N; ++i)
    {
        if (!g_swOk[i]) continue;
        if (g_sw[i] == 0x31333934) { g_romSwap = 0; g_romWord = i - 1; break; }
        if (g_sw[i] == 0x34393331) { g_romSwap = 1; g_romWord = i - 1; break; }
    }
    if (g_romWord < 0)
    {
        LOG("rom : bus name \"1394\" not found in the sweep");
        LOG("rom : -> ReadRegisterUL does NOT reach the config ROM with plain");
        LOG("rom :    offsets, so the audio unit base cannot be read from it");
        return 0;
    }

    LOG("rom : \"1394\" at word %d -> config ROM starts at word %d (offset 0x%X), quads %s",
        g_romWord + 1, g_romWord, g_romWord * 4, g_romSwap ? "byte-swapped" : "as sent");
    LOG("rom : bus info = %08X %08X %08X %08X %08X",
        ROMW(0), ROMW(1), ROMW(2), ROMW(3), ROMW(4));

    int rootLen = (int)((ROMW(5) >> 16) & 0xFFFF);
    LOG("rom : root directory header = %08X (length %d quadlets)", ROMW(5), rootLen);
    if (rootLen < 1 || rootLen > 64) { LOG("rom : implausible root length - stopping"); return 0; }

    int unitDirs[16], nUnits = 0;
    for (int k = 6; k < 5 + rootLen; ++k)
    {
        ULONG e = ROMW(k);
        int k8 = (int)((e >> 24) & 0xFF);
        LOG("rom :   root[%d] = %08X  key8=%02X val24=%06X  (key16=%04X val16=%04X)",
            k, e, k8, e & 0xFFFFFF, (e >> 16) & 0xFFFF, e & 0xFFFF);

        // 0x11 = Unit_Directory (immediate offset in quadlets from 0xFFFFF0000400)
        // 0xD1 = Unit_Directory as a leaf
        if (k8 == 0x11 || k8 == 0xD1)
        {
            int ud = g_romWord + (int)(e & 0xFFFFFF);
            if (nUnits < 16) unitDirs[nUnits++] = ud;
        }
    }

    for (int u = 0; u < nUnits; ++u)
    {
        int ud = unitDirs[u];
        if (ud < 0 || ud + 1 >= SW_N) continue;
        int ulen = (int)((ROMW(ud - g_romWord) >> 16) & 0xFFFF);
        LOG("rom : unit directory @word %d (offset 0x%X) length %d", ud, ud * 4, ulen);
        if (ulen < 1 || ud + ulen > SW_N) { LOG("rom :   implausible - skipped"); continue; }

        long spec = -1, ver = -1, csr = -1;
        for (int k = 1; k < ulen; ++k)
        {
            ULONG e = ROMW(ud - g_romWord + k);
            int k8 = (int)((e >> 24) & 0xFF);
            LOG("rom :     [%d] %08X  key8=%02X val24=%06X", k, e, k8, e & 0xFFFFFF);
            if (k8 == 0x12) spec = (long)(e & 0xFFFFFF);          // Unit_Spec_ID
            if (k8 == 0x13) ver  = (long)(e & 0xFFFFFF);          // Unit_Sw_Version
            if (k8 == 0x81) csr  = (long)(e & 0xFFFF);            // CSR_OFFSET
            if (((e >> 16) & 0xFFFF) == 0x8100) csr = (long)(e & 0xFFFF);
        }
        LOG("rom :     -> spec=0x%06X version=0x%06X csr_offset=0x%X%s",
            (unsigned)spec, (unsigned)ver, (unsigned)csr,
            (spec == 0x000A27 && ver == 0x000010) ? "   <<< APPLE ISIGHT AUDIO UNIT" : "");
    }
    return 1;
}

//---------------------------------------------------------------------
// fallback: find the gain-range fingerprint among the swept quadlets
//---------------------------------------------------------------------
static int FingerprintScan(void)
{
    int found = 0;
    for (int b = 0; b < SW_N; ++b)
    {
        int i0 = b + (A_GAIN_RAW_START / 4);
        int i1 = b + (A_GAIN_RAW_END   / 4);
        int i2 = b + (A_GAIN_DB_START  / 4);
        int i3 = b + (A_GAIN_DB_END    / 4);
        if (i3 >= SW_N) break;

        ULONG rawMin = g_sw[i0], rawMax = g_sw[i1];
        long  dbMin  = (long)g_sw[i2], dbMax = (long)g_sw[i3];

        if (rawMin == rawMax || rawMax == 0xFFFFFFFF || rawMin == 0xFFFFFFFF) continue;
        if (!g_swOk[i0] || !g_swOk[i1] || !g_swOk[i2] || !g_swOk[i3]) continue;

        long span = (long)rawMax - (long)rawMin;
        long dspan = dbMax - dbMin;
        if (span <= 0 || span > 0x100000) continue;
        if (dspan <= 0 || dspan > 20000) continue;          // dB span, x100
        if (dbMin > 0 || dbMax > 2000) continue;            // dB range must sit at/below 0

        LOG("scan: candidate audio base 0x%X : raw %u..%u  dB %ld..%ld (span %ld)",
            (unsigned)b * 4, rawMin, rawMax, dbMin, dbMax, dspan);
        found++;
    }
    if (!found) LOG("scan: no gain-range fingerprint in the swept range");
    return found;
}

//---------------------------------------------------------------------
// capture
//---------------------------------------------------------------------
static int DoCapture(const char *dev, ULONG base, int seconds, int forcedCh)
{
    LOG("");
    LOG("== capture: audio base = 0x%X, %d s, channel %s ==",
        base, seconds, forcedCh >= 0 ? "forced" : "auto");

    ULONG v = 0;
    if (RD(dev, base + A_GAIN_RAW_START, &v) == ERROR_SUCCESS) LOG("  reg 0x210 gain raw start = %08X (%u)", v, v);
    if (RD(dev, base + A_GAIN_RAW_END,   &v) == ERROR_SUCCESS) LOG("  reg 0x214 gain raw end   = %08X (%u)", v, v);
    if (RD(dev, base + A_GAIN_DB_START,  &v) == ERROR_SUCCESS) LOG("  reg 0x218 gain dB start  = %08X (%ld)", v, (long)v);
    if (RD(dev, base + A_GAIN_DB_END,    &v) == ERROR_SUCCESS) LOG("  reg 0x21c gain dB end    = %08X (%ld)", v, (long)v);
    if (RD(dev, base + A_RATE_INQUIRY,   &v) == ERROR_SUCCESS) LOG("  reg 0x280 rate inquiry   = %08X", v);
    if (RD(dev, base + A_GAIN,           &v) == ERROR_SUCCESS) LOG("  reg 0x500 gain           = %08X (%ld)", v, (long)v);
    if (RD(dev, base + A_MUTE,           &v) == ERROR_SUCCESS) LOG("  reg 0x504 mute           = %08X", v);
    if (RD(dev, base + A_AUDIO_ENABLE,   &v) == ERROR_SUCCESS) LOG("  reg 0x000 audio enable   = %08X", v);
    if (RD(dev, base + A_SAMPLE_RATE,    &v) == ERROR_SUCCESS) LOG("  reg 0x400 sample rate    = %08X", v);
    if (RD(dev, base + A_ISO_TX_CONFIG,  &v) == ERROR_SUCCESS) LOG("  reg 0x300 iso tx config  = %08X", v);

    ULONG speed = 0;
    if (GetMaxIsochSpeed((PSTR)dev, &speed) != ERROR_SUCCESS || speed == 0)
    {
        LOG("  GetMaxIsochSpeed -> %lu; assuming 400 Mbps (3)", speed);
        speed = 3;
    }
    LOG("  max isoch speed = %u", speed);

    ISOCH_QUERY_RESOURCES qr;
    ZeroMemory(&qr, sizeof(qr));
    ULONG ch = 63;
    if (t1394IsochQueryResources((PSTR)dev, &qr) == ERROR_SUCCESS)
    {
        LOG("  channels available = %08X%08X, bytes/frame available = %u",
            qr.ChannelsAvailable.HighPart, qr.ChannelsAvailable.LowPart, qr.BytesPerFrameAvailable);
        for (int c = 0; c < 64; ++c)
        {
            ULONG bit = (c < 32) ? (1u << c) : 0u;
            ULONG hi  = (c >= 32) ? (1u << (c - 32)) : 0u;
            if ((qr.ChannelsAvailable.LowPart & bit) || (qr.ChannelsAvailable.HighPart & hi)) { ch = (ULONG)c; break; }
        }
    }
    else LOG("  IsochQueryResources failed (%lu)", GetLastError());
    if (forcedCh >= 0) ch = (ULONG)forcedCh;
    LOG("  using isochronous channel %u", ch);

    // ask the device for 48 kHz and tell it which channel to transmit on
    DWORD r = WR(dev, base + A_SAMPLE_RATE, A_RATE_48000);
    LOG("  write sample rate 0x400 = 0x80000000 -> %lu", r);
    r = WR(dev, base + A_ISO_TX_CONFIG, ch | (speed << 16));
    LOG("  write iso tx config 0x300 = 0x%08X -> %lu", ch | (speed << 16), r);
    if (RD(dev, base + A_SAMPLE_RATE, &v) == ERROR_SUCCESS) LOG("  readback sample rate   = %08X", v);
    if (RD(dev, base + A_ISO_TX_CONFIG, &v) == ERROR_SUCCESS) LOG("  readback iso tx config = %08X", v);

    ULARGE_INTEGER dmaMax; dmaMax.QuadPart = 0;
    t1394_GetHostDmaCapabilities(dev, NULL, &dmaMax);
    LOG("  host max DMA buffer = %I64u bytes", dmaMax.QuadPart);

    const ULONG bytesPerFrame = A_PAYLOAD_BYTES;
    const ULONG frameBytes    = bytesPerFrame * 512;         // ~1 MB per buffer
    PACQUISITION_BUFFER buf = dc1394BuildAcquisitonBuffer(frameBytes,
                                                           (ULONG)dmaMax.QuadPart,
                                                           bytesPerFrame, 0);
    if (!buf) { LOG("  BuildAcquisitionBuffer FAILED"); return 2; }
    LOG("  frame buffer: %u sub-buffers, first %u bytes", buf->nSubBuffers, buf->subBuffers[0].ulSize);

    ISOCH_STREAM_PARAMS sp;
    ZeroMemory(&sp, sizeof(sp));
    sp.fulSpeed          = speed;
    sp.nMaxBytesPerFrame = bytesPerFrame;
    sp.nChannel          = ch;
    sp.nMaxBufferSize    = buf->subBuffers[0].ulSize;
    sp.nNumberOfBuffers  = 4 * buf->nSubBuffers + 1;

    r = t1394IsochSetupStream((PSTR)dev, &sp);
    LOG("  SetupStream -> %lu  (channel %u, %u bytes/frame, %u buffers @ %u)",
        r, sp.nChannel, sp.nMaxBytesPerFrame, sp.nNumberOfBuffers, sp.nMaxBufferSize);
    if (r != ERROR_SUCCESS) { dc1394FreeAcquisitionBuffer(buf); return 3; }

    if (sp.nChannel != ch)
    {
        ch = sp.nChannel;
        LOG("  driver chose channel %u - telling the camera", ch);
        WR(dev, base + A_ISO_TX_CONFIG, ch | (speed << 16));
    }

    HANDLE hdev = OpenDevice(dev, TRUE);
    if (hdev == INVALID_HANDLE_VALUE)
    {
        LOG("  OpenDevice(overlapped) FAILED (%lu)", GetLastError());
        t1394IsochTearDownStream((PSTR)dev);
        dc1394FreeAcquisitionBuffer(buf);
        return 4;
    }

    r = dc1394AttachAcquisitionBuffer(hdev, buf);
    LOG("  AttachAcquisitionBuffer -> %lu", r);
    if (r != ERROR_SUCCESS)
    {
        CloseHandle(hdev); t1394IsochTearDownStream((PSTR)dev); dc1394FreeAcquisitionBuffer(buf);
        return 5;
    }

    r = t1394IsochListen((PSTR)dev);
    LOG("  IsochListen -> %lu", r);
    if (r != ERROR_SUCCESS)
    {
        CloseHandle(hdev); t1394IsochTearDownStream((PSTR)dev); dc1394FreeAcquisitionBuffer(buf);
        return 6;
    }

    FILE *raw = fopen("isight-audio-raw.bin", "wb");
    double t0 = NowMs();
    int    completions = 0;
    unsigned long total = 0;

    while ((NowMs() - t0) < (double)seconds * 1000.0)
    {
        HANDLE evs[MAX_SUB_BUFFERS];
        for (unsigned int i = 0; i < buf->nSubBuffers; ++i) evs[i] = buf->subBuffers[i].overLapped.hEvent;

        DWORD w = WaitForMultipleObjects(buf->nSubBuffers, evs, FALSE, 200);
        if (w == WAIT_TIMEOUT || w == WAIT_FAILED) continue;

        unsigned int idx = w - WAIT_OBJECT_0;
        if (idx >= buf->nSubBuffers) continue;

        DWORD bytes = 0;
        if (GetOverlappedResult(hdev, &buf->subBuffers[idx].overLapped, &bytes, FALSE))
        {
            completions++;
            if (bytes)
            {
                LOG("  t=%6.0f ms  sub-buffer %u completed: %lu bytes", NowMs() - t0, idx, bytes);
                if (raw) { fwrite(buf->subBuffers[idx].pData, 1, bytes, raw); fflush(raw); }
                total += bytes;
            }
        }
        else LOG("  sub-buffer %u overlapped error %lu", idx, GetLastError());

        ResetEvent(evs[idx]);
        buf->subBuffers[idx].overLapped.Offset     = 0;
        buf->subBuffers[idx].overLapped.OffsetHigh = 0;
        ISOCH_BUFFER_PARAMS bp;
        bp.ulFlags = (idx == 0) ? ISOCH_BUFFER_PRIMARY : ISOCH_BUFFER_SECONDARY;
        t1394IsochAttachBuffer(hdev, buf->subBuffers[idx].pData,
                               buf->subBuffers[idx].ulSize, &bp,
                               &buf->subBuffers[idx].overLapped);
    }

    if (raw) fclose(raw);
    LOG("  capture done: %d completions, %lu bytes -> isight-audio-raw.bin", completions, total);

    t1394IsochStop((PSTR)dev);
    WR(dev, base + A_AUDIO_ENABLE, 0);
    t1394IsochTearDownStream((PSTR)dev);
    CloseHandle(hdev);
    dc1394FreeAcquisitionBuffer(buf);
    LOG("  stream stopped, audio enable cleared");
    return 0;
}

//---------------------------------------------------------------------
int main(int argc, char **argv)
{
    g_log = fopen("isight-audio.txt", "w");
    LOG("=== iSight audio probe %s ===", __TIMESTAMP__);

    const char *mode = (argc > 1) ? argv[1] : "sweep";
    LOG("mode: %s", mode);

    // device list
    HDEVINFO hDev = t1394CmdrGetDeviceList();
    if (hDev == INVALID_HANDLE_VALUE) { LOG("t1394CmdrGetDeviceList FAILED"); return 1; }

    char path[MAX_PATH] = "";
    ULONG len = sizeof(path);
    DWORD got = t1394CmdrGetDevicePath(hDev, 0, path, &len);
    LOG("t1394CmdrGetDevicePath(0) -> %lu  path='%s'", got, path);
    if (got <= 0 || path[0] == 0) { LOG("no CMU 1394 device found"); return 1; }

    if (!_stricmp(mode, "sweep"))
    {
        DoSweep(path);
        int romOk = ParseRom();
        if (!romOk) FingerprintScan();
        else
        {
            LOG("");
            LOG("reg : probing the audio unit offsets relative to csr_offset*4");
            for (int i = 0; i < 4; ++i) LOG("      (see the sweep above for base+0x%03X)", kGainOffs[i]);
        }
    }
    else if (!_stricmp(mode, "cap"))
    {
        ULONG base = 0;
        int   secs = 3;
        int   fch  = -1;
        if (argc > 2) base = (ULONG)strtoul(argv[2], NULL, 0);
        if (argc > 3) secs = atoi(argv[3]);
        if (argc > 4) fch  = atoi(argv[4]);
        if (base == 0) { LOG("usage: isight-audio.exe cap <baseHex> [seconds] [channel]"); return 1; }
        int rc = DoCapture(path, base, secs, fch);
        LOG("cap exit code %d", rc);
    }
    else
    {
        LOG("usage: isight-audio.exe sweep");
        LOG("       isight-audio.exe cap <baseHex> [seconds] [channel]");
    }

    LOG("=== done ===");
    if (g_log) fclose(g_log);
    return 0;
}
