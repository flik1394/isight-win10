//=====================================================================
// audio.cpp - Apple iSight audio (microphone) bring-up probe   v2
//
// The iSight's microphone is NOT an AV/C audio subunit and is not
// AM824/IEC 61883-6, so neither avcaudio.sys nor the in-box 61883 stack
// can drive it.  It is a second unit directory on the same 1394 node
// (specifier 0x000A27, version 0x000010) whose registers sit at their own
// CSR offset, and it streams a private isochronous packet format: a
// 16-byte header whose second quadlet is the ASCII "sght", followed by
// 48 kHz / 16-bit big-endian stereo samples.
// (Protocol taken from the Linux driver sound/firewire/isight.c.)
//
// Why v2:  the v1 tool blindly swept 1024 quadlets and printed nothing
// until the whole sweep was done.  Some of those offsets are simply not
// implemented by the camera, so every probe waited for a 1394 transaction
// timeout; after five minutes there was still no output and no way to see
// where it was stuck.  v2 instead:
//   * reads the CONFIG ROM first, at the absolute address that every node
//     is required to implement, and derives the audio unit's CSR offset
//     from the unit directories - no guessing, no timeouts;
//   * wraps every register access in a worker thread with its own timeout,
//     so a non-acknowledging address can never wedge the tool;
//   * prints (and flushes) every single access, so a stall is always
//     visible in isight-audio.txt.
//
// CMU raw-1394 addressing (see 1394main.c / 1394Camera.hpp):
//   0x00000600      -> relative,   CSR_OFFSET + 0x600  (camera video unit)
//   0xF0000600      -> absolute,   0xFFFFF0000000 | 0xF0000600
//                                 = 0xFFFFF0000600
//   0xF0F00000      -> absolute,   0xFFFFF0F00000
// So: if (offset & 0xF0000000) == 0xF0000000 the driver ORs it into
// 0xFFFFF0000000, otherwise it is relative to the camera's unit base.
// Every unit base lives in 0xFFFFF0000800..0xFFFFFFFFFFFF, whose low
// 32 bits always start with 0xF, so any unit is reachable this way.
//
//   isight-audio.exe                       == rom
//   isight-audio.exe rom       [quads]
//       dump + parse the config ROM (root dir, every unit directory),
//       identify the Apple audio unit (spec 0x000A27 / ver 0x000010),
//       and print candidate absolute bases for every CSR-offset-looking
//       entry (calibrated against the known DCAM video unit base).
//   isight-audio.exe regs    <absHex>
//       read the audio register block at that absolute base.
//   isight-audio.exe scan    <fromHex> <toHex> <stepHex> [timeoutMs]
//       fallback: look for the gain-range fingerprint over a range of
//       candidate bases.
//   isight-audio.exe cap     <absHex> [seconds] [channel]
//       program 48 kHz + an isochronous channel, capture the private
//       "sght" packet stream into isight-audio-raw.bin.
//   isight-audio.exe sweep   [fromHex] [count]     (legacy, now timed)
//       read a range of RELATIVE quadlets, timed + logged one by one.
//   isight-audio.exe poke    <absOffsetHex> <valueHex>
//   isight-audio.exe listen  <chan|auto> [seconds] [bytesPerFrame]
//       isochronous receive only - writes NOTHING to the camera.
//
// WHERE THE AUDIO UNIT ACTUALLY IS (measured, 2026-09-13):
//   the config ROM carries per-unit "CSR_OFFSET" entries whose key byte is
//   0x40 (IEEE 1212 / linux/firewire.h: #define CSR_OFFSET 0x40).  For the
//   Apple audio unit (spec 0x000A27, version 0x000010) that entry reads
//   40008000, i.e. 0x8000, and linux/sound/firewire/isight.c computes
//
//       audio_base = CSR_REGISTER_BASE + value * 4
//                  = 0xFFFFF0000000 + 0x20000 = 0xFFFFF0020000
//
//   which the CMU driver addresses as the absolute offset 0xF0020000.
//   All eleven audio registers answer there, with a gain range of
//   -30 dB .. +12 dB (raw 1..43) and 48 kHz already selected.
//   (Beware: key byte 0x81 is CSR_DESCRIPTOR|CSR_LEAF - the textual
//   descriptor "iSight" - NOT a CSR offset.  That mistake costs an hour.)
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

// where to point the camera's video engine relative to our receive channel
#define VID_SAME          (-1)      // the channel we are listening on
#define VID_OTHER         (-2)      // a different channel (Apple: never the same)

// v9: how many acquisition buffers to attach in CMU style.  C1394Camera asks
// for 8 and that is the number that receives 90/90 frames on this machine.
#define MAX_ACQ           8

//---------------------------------------------------------------------
// the DCAM video unit (0xFFFFF0F00000), used here as a *known good
// transmitter* so the receive path can be validated without the CMU
// video code (which would occupy the device's single isoch stream).
//
// NOTE on addressing: the video unit must be reached with *relative*
// offsets.  CMU's driver keeps the camera's CSR offset (0xF00000, i.e.
// the IIDC base 0xFFFFF0F00000) in its device extension and adds it to
// any offset that does not start with 0xF, so plain 0x60C is what
// C1394Camera itself uses - reading 0xF0F00600 on the other hand fails,
// because the absolute form encodes 0x0FFFF0000000 + (offset & 0x0FFFFFFF).
//---------------------------------------------------------------------
#define VIDEO_ABS_BASE    0UL       // relative offsets - see note above
#define V_FRAME_RATE      0x600
#define V_VIDEO_MODE      0x604
#define V_VIDEO_FORMAT    0x608
#define V_ISO_CHANNEL     0x60C     // channel/speed, packed per 1394a/b mode
#define V_ISO_ENABLE      0x614     // 0x80000000 starts the video stream
#define V_ISO_ENABLE_ON   0x80000000u

// the CMU driver treats offsets whose top nibble is 0xF as absolute
#define ABS_FLAG          0xF0000000UL
#define ABS_IS(o)         (((o) & ABS_FLAG) == ABS_FLAG)
// address of the node's config ROM, as an absolute driver offset
#define ROM_ABS           0xF0000400UL

// the DCAM video unit's base, quoted by the CMU documentation
#define VIDEO_BASE        0xF0F00000UL          // -> 0xFFFFF0F00000

//---------------------------------------------------------------------
// IRM (isochronous resource manager) registers  --  the experiment that
// matters most right now.
//
// Every node implements the CSR space at 0xFFFFF0000000, and the IRM
// registers live at +0x220..0x22C inside it.  Same absolute encoding as
// the config ROM, so the driver offsets are 0xF0000220 / 0xF0000224 /
// 0xF0000228 / 0xF000022C.  (Linux: firewire-ohci.c
// BANDWIDTH_AVAILABLE 0x220, CHANNELS_AVAILABLE_HI 0x224,
// CHANNELS_AVAILABLE_LO 0x228, BROADCAST_CHANNEL 0x22C.)
//
// A *set* bit in CHANNELS_AVAILABLE means the channel is FREE.  Proper
// 1394 allocation is a compare-swap lock, which CMU does not expose, so
// this does the plain read-modify-write the register also accepts.
//
// Why it matters: the camera never transmits audio even after it is
// fully programmed, and CMU's own IsochQueryResources always reports
// "bytes/frame available = 0" and never locks a channel.  Real drivers
// reserve the channel and the bandwidth *before* pointing a device at
// it.  iSight's firmware may simply refuse to start its audio engine on
// a channel that nobody has claimed.
//
// MEASURED on 2026-09-13: reading these four offsets works and returns
// the pristine bus state (BANDWIDTH_AVAILABLE = 4915, CHANNELS_AVAILABLE
// LO = FFFFFFFF, HI = FFFFFFFE), but every WRITE is refused by the driver
// and the register reads back unchanged.  CMU can only address the camera
// node, and the isochronous resource manager is the host controller, so
// a genuine allocation cannot be done from here - it needs code that can
// send a lock/write transaction to the LOCAL node (i.e. a driver).
//---------------------------------------------------------------------
#define IRM_BW_AVAIL      0xF0000220UL          // -> 0xFFFFF0000220
#define IRM_CH_HI         0xF0000224UL          // channels 32..63
#define IRM_CH_LO         0xF0000228UL          // channels  0..31
#define IRM_BCAST         0xF000022CUL

#define IRM_BW_UNITS      2000UL   // generous reserve for 1916 B/frame @ S400

static FILE *g_log = NULL;
static int   g_leakedThreads = 0;

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

//---------------------------------------------------------------------
// timed register access
//
// The CMU library's ReadRegister/WriteRegister open the device, issue a
// blocking DeviceIoControl and close it again.  A read aimed at an
// address the node does not implement has to run into a 1394 transaction
// timeout, and we have no control over how long the driver waits.  Running
// the call on its own thread lets us cap the wait and carry on; the thread
// is simply abandoned (the process is short-lived and these are diagnostics).
//---------------------------------------------------------------------
struct RegJob {
    const char *dev;
    ULONG       off;
    ULONG       val;
    DWORD       rc;
    BOOL        isWrite;
    ULONG       wval;
};

static DWORD WINAPI RegThread(LPVOID p)
{
    RegJob *j = (RegJob *)p;
    if (j->isWrite) j->rc = WriteRegisterUL((PSTR)j->dev, j->off, j->wval);
    else            j->rc = ReadRegisterUL((PSTR)j->dev, j->off, &j->val);
    return 0;
}

// returns TRUE if the call finished inside the timeout
static BOOL TimedReg(const char *dev, ULONG off, ULONG *val, DWORD timeoutMs,
                     BOOL isWrite, ULONG wval, DWORD *rcOut)
{
    RegJob *j = new RegJob;
    j->dev = dev; j->off = off; j->val = 0; j->rc = (DWORD)-1;
    j->isWrite = isWrite; j->wval = wval;

    // small stack - abandoned threads must stay cheap
    HANDLE h = CreateThread(NULL, 64 * 1024, RegThread, j, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { *val = 0; *rcOut = GetLastError(); delete j; return FALSE; }

    DWORD w = WaitForSingleObject(h, timeoutMs);
    if (w == WAIT_OBJECT_0)
    {
        if (val) *val = j->val;
        *rcOut = j->rc;
        CloseHandle(h);
        delete j;
        return TRUE;
    }
    g_leakedThreads++;
    CloseHandle(h);
    if (val) *val = 0;
    *rcOut = ERROR_TIMEOUT;              // 1460
    return FALSE;
}

static ULONG g_timeout = 700;            // ms per register access

// read one quadlet; returns TRUE when the node answered in time
static BOOL RD(const char *dev, ULONG off, ULONG *out, const char *what)
{
    ULONG v = 0; DWORD rc = 0;
    double t0 = NowMs();
    BOOL ok = TimedReg(dev, off, &v, g_timeout, FALSE, 0, &rc);
    double dt = NowMs() - t0;
    if (ok && rc == ERROR_SUCCESS) { if (out) *out = v; return TRUE; }
    if (out) *out = 0;
    if (what) LOG("    !! %-22s off 0x%08X : %s (rc=%lu, %.0f ms)",
                  what, off, ok ? "driver error" : "NO ANSWER (timeout)", rc, dt);
    return FALSE;
}

static BOOL WR(const char *dev, ULONG off, ULONG v, const char *what)
{
    DWORD rc = 0;
    BOOL ok = TimedReg(dev, off, NULL, g_timeout, TRUE, v, &rc);
    LOG("    write %-20s off 0x%08X <- 0x%08X : %s", what, off, v,
        (ok && rc == ERROR_SUCCESS) ? "OK" : (ok ? "error" : "NO ANSWER (timeout)"));
    return ok && rc == ERROR_SUCCESS;
}

//---------------------------------------------------------------------
// config ROM
//---------------------------------------------------------------------
#define ROM_MAXQ 256
static ULONG g_rom[ROM_MAXQ];
static BYTE  g_romOk[ROM_MAXQ];
static int   g_romSwap = 0;

#define RQ(k)  (g_romSwap ? _byteswap_ulong(g_rom[k]) : g_rom[k])

static const char *KeyName(int k)
{
    switch (k)
    {
    case 0x00: return "Textual_Descriptor";
    case 0x01: return "Descriptor_Leaf";
    case 0x03: return "Module_Vendor_Id";
    case 0x04: return "Hardware_Version";
    case 0x05: return "Node_Unique_Id_Offset";
    case 0x0C: return "Node_Units_Extent";
    case 0x11: return "Unit_Directory";
    case 0x12: return "Unit_Spec_Id";
    case 0x13: return "Unit_Sw_Version";
    case 0x14: return "Unit_Dependent_Info";
    case 0x15: return "Unit_Location";
    case 0x17: return "Unit_Model_Id / Model";
    case 0x18: return "Unit_Command_Set_Spec_Id";
    case 0x19: return "Unit_Command_Set";
    case 0x38: return "Cmd_Set_Spec_Id";
    case 0x39: return "Cmd_Set";
    case 0x3A: return "Cmd_Set_Rev";
    case 0x54: return "Unit_Unique_Id";
    case 0x81: return "CSR_OFFSET(?)";
    case 0xD1: return "Unit_Directory(leaf)";
    default:   return "";
    }
}

// candidate absolute driver offsets for a 24-bit value taken from a
// unit-directory entry; we print the ones that land on a 0x???0000
// boundary and flag the one matching the known video unit base
static void PrintCandidates(const char *tag, ULONG val24)
{
    static const int shifts[7] = { 0, 4, 8, 12, 16, 20, 24 };
    ULONG hits[7]; int nh = 0;

    for (int i = 0; i < 7; ++i)
    {
        unsigned long long v = (unsigned long long)val24 << shifts[i];
        if (v > 0xFFFFFFFFULL) continue;
        ULONG a = (ULONG)v;
        if ((a & 0x0000FFFF) != 0) continue;              // want a page-aligned-ish base
        if (ABS_IS(a) == FALSE) continue;                 // must be expressible absolutely
        hits[nh++] = a;
    }
    if (!nh) { LOG("      %s: 0x%06X -> no absolute candidate", tag, val24); return; }

    for (int i = 0; i < nh; ++i)
        LOG("      %s: 0x%06X -> abs offset 0x%08X %s", tag, val24, hits[i],
            hits[i] == VIDEO_BASE ? "  <<< = DCAM VIDEO BASE (calibrates the shift!)" : "");
}

static int g_audioBase = -1;          // absolute driver offset of the audio unit

static void ParseUnit(int dirIndex, int len, const char *from)
{
    long spec = -1, ver = -1;
    int   offEntry = -1;

    LOG("  unit directory @quad %d (%s), %d entries", dirIndex, from, len);
    for (int k = 1; k < len; ++k)
    {
        int i = dirIndex + k;
        if (i >= ROM_MAXQ) break;
        ULONG e = RQ(i);
        int k8 = (int)((e >> 24) & 0xFF);
        ULONG v24 = e & 0xFFFFFF;
        LOG("    [%2d] %08X  key=%02X %-24s val24=%06X%s", k, e, k8, KeyName(k8), v24,
            ((e >> 30) == 0) ? "   (leaf -> quad offset)" : "");
        if (k8 == 0x12) spec = (long)v24;
        if (k8 == 0x13) ver  = (long)v24;
        if (k8 == 0x81 || k8 == 0x03 || k8 == 0x14) offEntry = k;
    }
    LOG("    -> spec=0x%06lX version=0x%06lX", spec, ver);

    int isAudio = (spec == 0x000A27 && ver == 0x000010);
    LOG("    -> %s", isAudio ? "*** THIS IS THE APPLE ISIGHT AUDIO UNIT ***"
                             : "(some other unit)");

    for (int k = 1; k < len; ++k)
    {
        int i = dirIndex + k;
        if (i >= ROM_MAXQ) break;
        ULONG e = RQ(i);
        int k8 = (int)((e >> 24) & 0xFF);
        if ((e >> 30) != 0 && (k8 == 0x81 || k8 == 0x03 || k8 == 0x14 || k8 == 0x18))
            PrintCandidates(isAudio ? "AUDIO" : "unit ", e & 0xFFFFFF);
    }
    (void)offEntry;
}

static int DoRom(const char *dev, int quads)
{
    if (quads < 16)    quads = 16;
    if (quads > ROM_MAXQ) quads = ROM_MAXQ;

    LOG("");
    LOG("== config ROM dump: absolute offset 0x%08X, %d quadlets ==", ROM_ABS, quads);
    int ok = 0;
    for (int k = 0; k < quads; ++k)
    {
        ULONG v = 0; DWORD rc = 0;
        if (TimedReg(dev, ROM_ABS + (ULONG)k * 4, &v, g_timeout, FALSE, 0, &rc)
            && rc == ERROR_SUCCESS)
        {
            g_rom[k] = v; g_romOk[k] = 1; ok++;
        }
        else { g_rom[k] = 0xDEADBEEF; g_romOk[k] = 0; }
    }
    LOG("rom : %d of %d quadlets answered", ok, quads);
    if (ok < 8) { LOG("rom : the config ROM is unreachable - stopping"); return 0; }

    for (int k = 0; k < quads; k += 4)
        LOG("  %02X: %08X %08X %08X %08X", k,
            g_rom[k], g_rom[k + 1], g_rom[k + 2], g_rom[k + 3]);

    g_romSwap = 0;
    if (RQ(1) != 0x31333934 && (g_rom[1] == 0x34393331)) g_romSwap = 1;
    LOG("rom : quad1 = %08X -> bus name \"%c%c%c%c\", order = %s",
        RQ(1), (char)((RQ(1) >> 24) & 0xFF), (char)((RQ(1) >> 16) & 0xFF),
        (char)((RQ(1) >> 8) & 0xFF), (char)(RQ(1) & 0xFF),
        g_romSwap ? "BYTE-SWAPPED" : "as sent (big-endian)");
    if (RQ(1) != 0x31333934) LOG("rom : WARNING - \"1394\" not at quad 1, parsing anyway");

    LOG("rom : bus info = %08X %08X %08X %08X %08X",
        RQ(0), RQ(1), RQ(2), RQ(3), RQ(4));

    int rootLen = (int)(RQ(5) >> 16);
    LOG("rom : root dir header = %08X -> %d entries", RQ(5), rootLen);
    if (rootLen < 1 || rootLen > 64) { LOG("rom : implausible root length - stopping"); return 0; }

    int unitDirs[16], nUnits = 0;
    for (int k = 0; k < rootLen; ++k)
    {
        int i = 6 + k;
        ULONG e = RQ(i);
        int k8 = (int)((e >> 24) & 0xFF);
        LOG("rom : root[%d] = %08X  key=%02X %-22s val24=%06X", k, e, k8, KeyName(k8), e & 0xFFFFFF);
        if (k8 == 0x11 || k8 == 0xD1)
        {
            int ud = (int)(e & 0xFFFFFF);         // quadlet offset from the ROM base
            if (nUnits < 16) unitDirs[nUnits++] = ud;
        }
        else if ((e >> 30) != 0 && (k8 == 0x03 || k8 == 0x04))
            PrintCandidates("root ", e & 0xFFFFFF);
    }
    LOG("rom : %d unit director%s found", nUnits, nUnits == 1 ? "y" : "ies");

    for (int u = 0; u < nUnits; ++u)
    {
        int ud = unitDirs[u];
        if (ud < 0 || ud + 1 >= ROM_MAXQ) { LOG("  unit dir quad %d out of range", ud); continue; }
        int ulen = (int)(RQ(ud) >> 16);
        ParseUnit(ud, ulen, "root entry");
    }
    return 1;
}

//---------------------------------------------------------------------
// audio register block + fallback fingerprint scan
//---------------------------------------------------------------------
static const struct { ULONG off; const char *name; } kRegs[] = {
    { A_AUDIO_ENABLE,   "AUDIO_ENABLE  " },
    { A_DEF_GAIN,       "DEF_GAIN      " },
    { A_GAIN_RAW_START, "GAIN_RAW_START" },
    { A_GAIN_RAW_END,   "GAIN_RAW_END  " },
    { A_GAIN_DB_START,  "GAIN_DB_START " },
    { A_GAIN_DB_END,    "GAIN_DB_END   " },
    { A_RATE_INQUIRY,   "RATE_INQUIRY  " },
    { A_ISO_TX_CONFIG,  "ISO_TX_CONFIG " },
    { A_SAMPLE_RATE,    "SAMPLE_RATE   " },
    { A_GAIN,           "GAIN          " },
    { A_MUTE,           "MUTE          " },
};

static int DoRegs(const char *dev, ULONG base)
{
    LOG("");
    LOG("== audio register block @ abs 0x%08X ==", base);
    int ok = 0;
    for (int i = 0; i < (int)(sizeof(kRegs) / sizeof(kRegs[0])); ++i)
    {
        ULONG v = 0;
        double t0 = NowMs();
        if (RD(dev, base + kRegs[i].off, &v, NULL))
        {
            LOG("  base+0x%03X %s = %08X  (%u / %ld)", kRegs[i].off, kRegs[i].name, v, v, (long)v);
            ok++;
        }
        else LOG("  base+0x%03X %-14s : NO ANSWER (%.0f ms)", kRegs[i].off, kRegs[i].name,
                 NowMs() - t0);
    }
    LOG("regs: %d of %d answered", ok, (int)(sizeof(kRegs) / sizeof(kRegs[0])));

    // fingerprint: an audio gain range has raw_min < raw_max and a dB range at/below 0
    ULONG rs = 0, re = 0, dbs = 0, dbe = 0;
    if (RD(dev, base + A_GAIN_RAW_START, &rs, NULL) && RD(dev, base + A_GAIN_RAW_END, &re, NULL))
    {
        long span = (long)re - (long)rs;
        LOG("     gain span (raw) = %ld", span);
    }
    if (RD(dev, base + A_GAIN_DB_START, &dbs, NULL) && RD(dev, base + A_GAIN_DB_END, &dbe, NULL))
        LOG("     gain span (dB x100) = %ld (%ld .. %ld)", (long)dbe - (long)dbs, (long)dbs, (long)dbe);
    return ok;
}

static int DoScan(const char *dev, ULONG from, ULONG to, ULONG step)
{
    LOG("");
    LOG("== fallback scan 0x%08X .. 0x%08X step 0x%X, probing base+0x210 ==", from, to, step);
    int found = 0;
    ULONG cands[8]; int n = 0;
    for (ULONG b = from; b <= to && g_leakedThreads < 400; b += step)
    {
        ULONG v = 0;
        if (RD(dev, b + A_GAIN_RAW_START, &v, NULL))
        {
            ULONG e = 0, rq = 0;
            BOOL eOk = RD(dev, b + A_GAIN_RAW_END, &e, NULL);
            BOOL qOk = RD(dev, b + A_RATE_INQUIRY, &rq, NULL);
            LOG("scan: base 0x%08X answered: 0x210=%u  0x214=%u  0x280=%08X%s",
                b, v, e, rq, (eOk && qOk) ? "" : "   (partial)");
            if (eOk && e != v && v != 0xFFFFFFFF && e != 0xFFFFFFFF)
            {
                LOG("       ^^ plausible gain range - candidate #%d", found + 1);
                if (n < 8) cands[n++] = b;
                found++;
            }
        }
    }
    for (int i = 0; i < n; ++i) LOG("scan: candidate base 0x%08X  (abs 0x%08X)", cands[i], cands[i]);
    if (!found) LOG("scan: no responding base in that range");
    LOG("scan: leaked (still-blocked) register threads so far: %d", g_leakedThreads);
    return found;
}

//---------------------------------------------------------------------
// capture
//
// The CMU driver wants nChannel == -1 on input to mean "allocate one for
// me" (that is what C1394Camera::InitResources passes); the driver then
// writes the chosen channel back into the structure.  Handing it channel 0
// instead makes it return -1, i.e. no stream at all - which is exactly what
// the first attempt did.
//
// The camera's own transmit channel lives in ISO_TX_CONFIG and is a
// (channel | speed_index << 16) pair - the speed there is the 0=S100 ..
// 3=S800 *index*, not the SPEED_FLAGS_* bitmask, so it has to be converted
// with SpeedFlagToIndex().
//---------------------------------------------------------------------
static int SpeedIndex(ULONG flag)
{
    LONG i = SpeedFlagToIndex(flag);
    if (i < 0 || i > 3) i = 2;                 // S400
    return (int)i;
}

static int g_lastChannel = -1;
static int g_lastCompletions = -1;

// v9: use C1394Camera::StartImageAcquisitionEx()'s exact buffer parameters
// (8 acquisition buffers of one full 640x480 YUV 4:1:1 frame, 960 bytes per
// packet, nNumberOfBuffers = subBuffers + 1).  Defaults to on because that
// is the only configuration known to receive on this machine; "plain" on the
// command line switches back to v8's ad-hoc numbers for an A/B comparison.
static BOOL g_cmu = TRUE;

// v10: optional DCAM frame-rate override for the video engine we start as
// the microphone's clock source.  -1 leaves the camera's setting alone.
static int g_vidRate = -1;

//---------------------------------------------------------------------
// IRM helpers
//---------------------------------------------------------------------
static BOOL g_irm       = FALSE;   // "irm" anywhere on the command line
static BOOL g_irmLocked = FALSE;
static ULONG g_irmChannel = 0;
static ULONG g_irmBwTaken = 0;

static void IrmDump(const char *dev)
{
    ULONG bw = 0, hi = 0, lo = 0, bc = 0;
    LOG("");
    LOG("== IRM registers (isochronous resource manager) ==");
    BOOL ok = RD(dev, IRM_BW_AVAIL, &bw, "BANDWIDTH_AVAILABLE");
    RD(dev, IRM_CH_HI, &hi, "CHANNELS_AVAILABLE_HI");
    RD(dev, IRM_CH_LO, &lo, "CHANNELS_AVAILABLE_LO");
    RD(dev, IRM_BCAST, &bc, "BROADCAST_CHANNEL");
    if (!ok)
    {
        LOG("  *** the IRM registers do not answer.  Either this host is not the");
        LOG("      IRM (the camera may be), or the OHCI driver filters writes here.");
        return;
    }
    LOG("  bandwidth available : %u units", bw);
    LOG("  free channels  0..31: %08X", lo);
    LOG("  free channels 32..63: %08X", hi);
    LOG("  broadcast channel   : %08X", bc);
    char list[256]; size_t n = 0; list[0] = 0;
    for (int c = 0; c < 64; ++c)
    {
        ULONG v = (c < 32) ? lo : hi;
        if (v & (1u << (c & 31)))
        {
            if (n > sizeof(list) - 12) { strcat_s(list, sizeof(list), ",..."); break; }
            _snprintf_s(list + n, sizeof(list) - n, _TRUNCATE, "%s%d", n ? "," : "", c);
            n = strlen(list);
        }
    }
    LOG("  free channel list   : %s", list[0] ? list : "(none)");
}

static BOOL IrmSetChannelBit(const char *dev, ULONG ch, BOOL freeIt)
{
    ULONG off = (ch < 32) ? IRM_CH_LO : IRM_CH_HI;
    ULONG bit = 1u << (ch & 31);
    ULONG v = 0;
    if (!RD(dev, off, &v, NULL)) return FALSE;
    ULONG want = freeIt ? (v | bit) : (v & ~bit);
    if (v == want)
    {
        LOG("  channel %u: bit already %s (no change written)", ch, freeIt ? "set/free" : "clear/taken");
        return TRUE;
    }
    if (!WR(dev, off, want, freeIt ? "CHANNELS_AVAILABLE (release)" : "CHANNELS_AVAILABLE (acquire)"))
        return FALSE;
    ULONG rb = 0;
    if (RD(dev, off, &rb, NULL))
        LOG("  channel %u: %08X -> %08X (%s)", ch, v, rb,
            ((rb & bit) ? "FREE" : "TAKEN"));
    return (rb & bit) ? freeIt : !freeIt;
}

static BOOL IrmReserveBandwidth(const char *dev, ULONG units)
{
    ULONG v = 0;
    if (!RD(dev, IRM_BW_AVAIL, &v, NULL)) { LOG("  cannot read BANDWIDTH_AVAILABLE"); return FALSE; }
    if (v <= units) { LOG("  only %u units free, refusing to reserve %u", v, units); return FALSE; }
    if (!WR(dev, IRM_BW_AVAIL, v - units, "BANDWIDTH_AVAILABLE (reserve)")) return FALSE;
    ULONG rb = 0;
    if (RD(dev, IRM_BW_AVAIL, &rb, NULL))
    {
        LOG("  bandwidth %u -> %u units (reserved %u)", v, rb, units);
        g_irmBwTaken = (v > rb) ? (v - rb) : 0;
    }
    return TRUE;
}

static void IrmReleaseBandwidth(const char *dev)
{
    if (!g_irmBwTaken) return;
    ULONG v = 0;
    if (!RD(dev, IRM_BW_AVAIL, &v, NULL)) return;
    if (WR(dev, IRM_BW_AVAIL, v + g_irmBwTaken, "BANDWIDTH_AVAILABLE (release)"))
    {
        ULONG rb = 0;
        if (RD(dev, IRM_BW_AVAIL, &rb, NULL)) LOG("  bandwidth restored %u -> %u units", v, rb);
    }
    g_irmBwTaken = 0;
}

//---------------------------------------------------------------------
// Scan a captured isochronous dump for the iSight audio signature.
// Byte offsets inside a DMA buffer are not trustworthy - the driver hands
// back whole isochronous frames whose internal layout we do not control -
// so this slides a 4-byte window over the data and counts every "sght".
// When it finds one it decodes the header Apple documents (sample_count,
// signature, sample_total) and the first few 16-bit big-endian stereo
// samples, which is enough to tell music from noise.
//---------------------------------------------------------------------
static void ScanRaw(const char *fn)
{
    FILE *f = fopen(fn, "rb");
    if (!f) { LOG("  (no %s to scan)", fn); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 4) { fclose(f); LOG("  %s is only %ld bytes - nothing to scan", fn, n); return; }

    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (!b) { fclose(f); return; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    n = (long)got;

    long hits = 0, first = -1;
    for (long i = 0; i + 4 <= n; ++i)
        if (b[i] == 0x73 && b[i + 1] == 0x67 && b[i + 2] == 0x68 && b[i + 3] == 0x74)
        { if (hits == 0) first = i; hits++; }

    LOG("  scanning %s: %ld bytes, \"sght\" signature found %ld time(s)%s",
        fn, n, hits, hits ? "" : "   <-- NO audio payload in this dump");
    if (first >= 12)
    {
        ULONG sc = ((ULONG)b[first - 12] << 24) | ((ULONG)b[first - 11] << 16) |
                   ((ULONG)b[first - 10] << 8) | (ULONG)b[first - 9];
        ULONG st = ((ULONG)b[first + 4] << 24) | ((ULONG)b[first + 5] << 16) |
                   ((ULONG)b[first + 6] << 8) | (ULONG)b[first + 7];
        LOG("  first packet: sample_count=%lu  sample_total=%lu  (offset %ld)", sc, st, first);
        char line[256];
        int p = 0;
        for (int k = 0; k < 8 && (first + 18 + k * 2) < n; ++k)
        {
            short v = (short)((b[first + 16 + k * 2] << 8) | b[first + 17 + k * 2]);
            p += _snprintf(line + p, sizeof(line) - (size_t)p, "%d ", v);
            if (p < 0) { p = 0; line[0] = 0; break; }
        }
        line[p] = 0;
        LOG("  first 8 samples (16-bit BE stereo, L/R): %s", line);
    }
    free(b);
}

//---------------------------------------------------------------------
// Tell the camera's DCAM video engine where to send and start it.
// This is v8's replacement for the inline block v6/v7 carried: it now
// reads the channel register back afterwards, because "the write
// reported OK" is not the same as "the camera accepted it" - every
// silent failure we have hit so far looked like a successful write.
//---------------------------------------------------------------------
// decode the transmit channel out of a DCAM 0x60C quadlet: 1394a packs
// it in bits 31-28, 1394b (bit 15 set) in bits 13-8
static ULONG ChannelOf60C(ULONG v)
{
    if (v & 0x00008000) return (v >> 8) & 0x3F;
    return (v >> 28) & 0x0F;
}

static void VideoEngineOn(const char *dev, ULONG flag, int vidCh, ULONG ourCh,
                          ULONG *pOldCh, ULONG *pOldEn, BOOL *pSaved, int forceRate)
{
    ULONG oldCh = 0, oldEn = 0;
    *pSaved = RD(dev, VIDEO_ABS_BASE + V_ISO_CHANNEL, &oldCh, NULL) &&
              RD(dev, VIDEO_ABS_BASE + V_ISO_ENABLE,  &oldEn, NULL);
    *pOldCh = oldCh;
    *pOldEn = oldEn;

    ULONG vc;
    if (vidCh >= 0)              vc = (ULONG)vidCh;               // explicit
    else if (vidCh == VID_SAME)  vc = ourCh;                      // what we listen on
    else
    {
        // VID_OTHER: prefer the channel the camera was already using for
        // video (channel 2 on this unit); only if that collides with our
        // receive channel fall back to a different one.  Audio and video
        // must never share an isochronous channel.
        ULONG prev = ChannelOf60C(oldCh);
        vc = (prev != ourCh) ? prev : ((ourCh == 0) ? 1u : 0u);
    }

    ULONG v = oldCh, si = (ULONG)SpeedIndex(flag);
    if (oldCh & 0x00008000) v = (v & 0xFFFF8000u) | ((vc & 0x3F) << 8) | si;
    else                    v = (v & 0x0000FFFFu) | (vc << 28) | (si << 24);

    LOG("  -> video 0x60C = 0x%08X (video channel %lu, we listen on %u), was 0x%08X",
        v, vc, ourCh, oldCh);
    WR(dev, VIDEO_ABS_BASE + V_ISO_CHANNEL, v, "V_ISO_CHANNEL");

    // Optional frame-rate override.  DCAM packs its FMR registers as
    // value << 29, and the camera sends audio in the cycles the video
    // leaves free - so a slower video frame rate means more free cycles,
    // which is the one knob that should directly improve audio continuity.
    if (forceRate >= 0)
    {
        ULONG before = 0;
        RD(dev, VIDEO_ABS_BASE + V_FRAME_RATE, &before, NULL);
        LOG("  -> video 0x600 (frame rate) = rate %d, was rate %u", forceRate, before >> 29);
        WR(dev, VIDEO_ABS_BASE + V_FRAME_RATE, (ULONG)forceRate << 29, "V_FRAME_RATE");
    }

    WR(dev, VIDEO_ABS_BASE + V_ISO_ENABLE,  V_ISO_ENABLE_ON, "V_ISO_ENABLE");

    ULONG rb = 0;
    if (RD(dev, VIDEO_ABS_BASE + V_ISO_CHANNEL, &rb, NULL))
        LOG("     0x60C reads back 0x%08X  %s", rb,
            (rb == v) ? "(the write took)" : "(THE WRITE DID NOT TAKE!)");
    if (RD(dev, VIDEO_ABS_BASE + V_ISO_ENABLE, &rb, NULL))
        // NOTE: in the v9 run where this read back 0, a full YUV video
        // picture was nevertheless present in the capture - so this read
        // is reported but must NOT be trusted as "is it streaming".
        LOG("     0x614 reads back 0x%08X  %s", rb,
            (rb & V_ISO_ENABLE_ON) ? "(streaming)" : "(says not streaming - not conclusive)");
}

//---------------------------------------------------------------------
// Program the *audio* unit: sample rate, transmit channel/speed, enable.
// Exactly Linux isight.c's order (SAMPLE_RATE -> ISO_TX_CONFIG ->
// AUDIO_ENABLE) and exactly its packing for 0x300: channel in the low
// 16 bits, speed index in bits 16..31.
//---------------------------------------------------------------------
static void CameraAudioOn(const char *dev, ULONG cfgBase, ULONG ch, ULONG flag,
                          BOOL enableAudio)
{
    WR(dev, cfgBase + A_SAMPLE_RATE, A_RATE_48000, "SAMPLE_RATE");
    ULONG txv = ch | ((ULONG)SpeedIndex(flag) << 16);
    LOG("  -> camera audio: channel %u, speed index %d (0x300 <- 0x%08X)",
        ch, SpeedIndex(flag), txv);
    WR(dev, cfgBase + A_ISO_TX_CONFIG, txv, "ISO_TX_CONFIG");
    if (enableAudio)
        WR(dev, cfgBase + A_AUDIO_ENABLE, 0x80000000, "AUDIO_ENABLE");

    ULONG rb = 0;
    if (RD(dev, cfgBase + A_ISO_TX_CONFIG, &rb, NULL))
        LOG("     0x300 reads back 0x%08X  %s", rb,
            (rb == txv) ? "(the write took)" : "(THE WRITE DID NOT TAKE!)");
}

static void FreeAcq(PACQUISITION_BUFFER *acq, int n)
{
    for (int i = 0; i < n; ++i)
        if (acq[i]) { dc1394FreeAcquisitionBuffer(acq[i]); acq[i] = NULL; }
}

static int DoReceive(const char *dev, int chIn, int seconds, ULONG bpf, BOOL dumpRaw,
                     ULONG cfgBase, int restoreCh, BOOL enableAudio, BOOL vidEnable,
                     int vidCh, BOOL vidFirst, BOOL cmuStyle, int vidRate)
{
    ULONG flag = 0;
    if (GetMaxIsochSpeed((PSTR)dev, &flag) != ERROR_SUCCESS || flag == 0) flag = SPEED_FLAGS_400;
    LOG("  max isoch speed flag = 0x%X -> speed index %d (%d Mbps)",
        flag, SpeedIndex(flag), 100 << SpeedIndex(flag));

    ISOCH_QUERY_RESOURCES qr;
    ZeroMemory(&qr, sizeof(qr));
    if (t1394IsochQueryResources((PSTR)dev, &qr) == ERROR_SUCCESS)
        LOG("  resources: fulSpeed=0x%X  bytes/frame available=%u  channels=%08X%08X",
            qr.fulSpeed, qr.BytesPerFrameAvailable,
            qr.ChannelsAvailable.HighPart, qr.ChannelsAvailable.LowPart);
    else LOG("  IsochQueryResources failed (%lu)", GetLastError());

    ULARGE_INTEGER dmaMax; dmaMax.QuadPart = 0;
    t1394_GetHostDmaCapabilities(dev, NULL, &dmaMax);
    LOG("  host max DMA buffer = %I64u bytes", dmaMax.QuadPart);

    // ---- buffers -----------------------------------------------------
    // v9: the parameter set is now switchable.  In 'cmuStyle' every number
    // is the one C1394Camera::StartImageAcquisitionEx() would use for
    // Format 0 / Mode 2 / 15 fps - 8 acquisition buffers of one full frame
    // (640x480 YUV 4:1:1 = 460800 bytes), a 960-byte packet budget and
    // nNumberOfBuffers = subBuffers + 1 - because that is the only
    // configuration on this machine that is *known* to receive (90/90
    // frames in isight-diag).  v8 used one 131072-byte buffer and told the
    // driver to expect 8, and received nothing at all; that difference is
    // what this variant exists to test.
    ULONG frameSize = bpf * 32;
    ULONG pktSize   = bpf;      // = nMaxBytesPerFrame; the caller knows the
                                // real packet budget (4096 for the video self
                                // test, 1916 for audio - Apple's largest
                                // audio packet).  Never shrink it to CMU's
                                // 960: that is a *video* number and would
                                // truncate every audio packet.
    int   nAcq      = 1;
    if (cmuStyle)
    {
        frameSize = 640UL * 480UL * 3UL / 2UL;   // one 640x480 YUV 4:1:1 frame
        nAcq      = MAX_ACQ;
    }

    PACQUISITION_BUFFER acq[MAX_ACQ];
    ZeroMemory(acq, sizeof(acq));
    for (int i = 0; i < nAcq; ++i)
    {
        acq[i] = dc1394BuildAcquisitonBuffer(frameSize, (ULONG)dmaMax.QuadPart, pktSize, (ULONG)i);
        if (!acq[i]) { LOG("  BuildAcquisitionBuffer(%d) FAILED", i); return 2; }
    }
    ULONG nSub = 0;
    for (int i = 0; i < nAcq; ++i) nSub += acq[i]->nSubBuffers;
    LOG("  buffers: %d acquisition buffer(s) of %u bytes covering %u sub-buffer(s), %u bytes/packet"
        "  (cmuStyle=%s, CMU would use 8 x 460800 @ 960)",
        nAcq, frameSize, nSub, pktSize, cmuStyle ? "ON" : "off");

    ISOCH_STREAM_PARAMS sp;
    ZeroMemory(&sp, sizeof(sp));
    sp.fulSpeed          = flag;
    sp.nMaxBytesPerFrame = pktSize;
    sp.nChannel          = (chIn < 0) ? (ULONG)-1 : (ULONG)chIn;
    sp.nMaxBufferSize    = acq[0]->subBuffers[0].ulSize;
    // exactly what C1394Camera::InitResources() computes:
    //   nNumberOfBuffers = (acquisitionBuffers * subBuffers) + 1
    sp.nNumberOfBuffers  = nSub + 1;

    LOG("  SetupStream(in): channel=%s, %u bytes/frame, %u buffers @ %u",
        chIn < 0 ? "auto(-1)" : "explicit", sp.nMaxBytesPerFrame, sp.nNumberOfBuffers, sp.nMaxBufferSize);
    DWORD r = t1394IsochSetupStream((PSTR)dev, &sp);
    LOG("  SetupStream -> %lu  (channel 0x%X, speed 0x%X, bpf %u, %u buffers)",
        r, sp.nChannel, sp.fulSpeed, sp.nMaxBytesPerFrame, sp.nNumberOfBuffers);
    if (r != ERROR_SUCCESS) { FreeAcq(acq, nAcq); return 3; }
    if (sp.nChannel == 0xFFFFFFFFu || sp.nChannel > 63)
    {
        if (chIn >= 0)
        {
            // Explicit channel, and this reply *might* be CMU's "subscribe"
            // outcome.  v7 asserted it was; v8's auto-allocated run made that
            // look doubtful, because the driver filled in a real channel (0)
            // when it was allowed to allocate.  So we no longer *believe* the
            // subscribe reading - we accept it and let the byte count decide.
            LOG("  the driver returned no channel (0x%X) for the explicit request %d ->",
                sp.nChannel, chIn);
            LOG("  assuming a subscribe on channel %d - the byte count will tell", chIn);
            sp.nChannel = (ULONG)chIn;
        }
        else
        {
            LOG("  *** the driver did NOT allocate an isochronous channel (0x%X) - cannot receive",
                sp.nChannel);
            t1394IsochTearDownStream((PSTR)dev);
            FreeAcq(acq, nAcq);
            return 7;
        }
    }
    else if (chIn >= 0 && (int)sp.nChannel != chIn)
        LOG("  NOTE: asked for channel %d, the driver answered %u", chIn, sp.nChannel);
    g_lastChannel = (int)sp.nChannel;
    LOG("  receiving on channel %u", sp.nChannel);

    // -- IRM experiment -------------------------------------------------
    // Claim the channel and the bandwidth *before* telling the camera to
    // transmit on it.  This is the step CMU never performs.
    if (g_irm)
    {
        IrmDump(dev);
        LOG("  -- reserving channel %u and %u bandwidth units --", sp.nChannel, IRM_BW_UNITS);
        IrmReserveBandwidth(dev, IRM_BW_UNITS);
        g_irmLocked = IrmSetChannelBit(dev, sp.nChannel, FALSE);
        g_irmChannel = sp.nChannel;
        IrmDump(dev);
    }

    ULONG vOldCh = 0, vOldEn = 0;
    BOOL  vSaved = FALSE;

    HANDLE hdev = OpenDevice(dev, TRUE);
    if (hdev == INVALID_HANDLE_VALUE)
    {
        LOG("  OpenDevice(overlapped) FAILED (%lu)", GetLastError());
        t1394IsochTearDownStream((PSTR)dev);
        FreeAcq(acq, nAcq);
        return 4;
    }

    int attached = 0;
    for (int i = 0; i < nAcq; ++i)
    {
        r = dc1394AttachAcquisitionBuffer(hdev, acq[i]);
        if (r != ERROR_SUCCESS)
        {
            LOG("  AttachAcquisitionBuffer(%d) -> %lu  FAILED", i, r);
            CloseHandle(hdev); t1394IsochTearDownStream((PSTR)dev); FreeAcq(acq, nAcq);
            return 5;
        }
        attached++;
    }
    LOG("  AttachAcquisitionBuffer x%d -> 0", attached);

    // CMU waits 50 ms here and then insists that every attached buffer still
    // report ERROR_IO_INCOMPLETE: a buffer that is already "ready" before
    // IsochListen means the attach did not take, and CMU treats that as a
    // hard failure.  We only report it - but we report it, because v8 had no
    // way of telling "attached" from "silently dropped".
    Sleep(50);
    int pending = 0, ready = 0, errs = 0;
    for (int i = 0; i < nAcq; ++i)
        for (unsigned int b = 0; b < acq[i]->nSubBuffers; ++b)
        {
            DWORD got = 0;
            if (GetOverlappedResult(hdev, &acq[i]->subBuffers[b].overLapped, &got, FALSE)) ready++;
            else if (GetLastError() == ERROR_IO_INCOMPLETE) pending++;
            else errs++;
        }
    LOG("  buffer validation: %d pending (good), %d ready-too-early (bad), %d error(s)", pending, ready, errs);

    r = t1394IsochListen((PSTR)dev);
    LOG("  IsochListen -> %lu", r);
    if (r != ERROR_SUCCESS)
    {
        CloseHandle(hdev); t1394IsochTearDownStream((PSTR)dev); FreeAcq(acq, nAcq);
        return 6;
    }

    // (v8) We are listening.  *Now* program the camera.  None of this is
    // host-side bookkeeping: writing a channel number only tells the camera
    // where to send, so the order relative to Listen is free - and doing it
    // after Listen means the very first packet is not thrown away.
    // vidFirst exists because the iSight derives its audio clock from the
    // video engine: with the video engine stopped there may be no clock at
    // all, so "video up, then audio" is a genuinely different experiment
    // from "audio up, then video".
    LOG("  -- programming the camera (video %s) --",
        vidFirst ? "FIRST, audio second" : "last, audio first");
    if (vidEnable && vidFirst)
        VideoEngineOn(dev, flag, vidCh, sp.nChannel, &vOldCh, &vOldEn, &vSaved, vidRate);
    if (cfgBase)
        CameraAudioOn(dev, cfgBase, sp.nChannel, flag, enableAudio);
    if (vidEnable && !vidFirst)
        VideoEngineOn(dev, flag, vidCh, sp.nChannel, &vOldCh, &vOldEn, &vSaved, vidRate);

    FILE *raw = fopen("isight-audio-raw.bin", "wb");
    double t0 = NowMs();
    int    completions = 0;
    unsigned long total = 0;

    while ((NowMs() - t0) < (double)seconds * 1000.0)
    {
        HANDLE evs[MAX_ACQ * MAX_SUB_BUFFERS];
        int    owner[MAX_ACQ * MAX_SUB_BUFFERS];
        unsigned int sub[MAX_ACQ * MAX_SUB_BUFFERS];
        unsigned int nev = 0, i;

        for (i = 0; i < (unsigned int)nAcq; ++i)
            for (unsigned int b = 0; b < acq[i]->nSubBuffers; ++b)
            {
                evs[nev]   = acq[i]->subBuffers[b].overLapped.hEvent;
                owner[nev] = (int)i;
                sub[nev]   = b;
                nev++;
            }
        (void)i;

        DWORD w = WaitForMultipleObjects(nev, evs, FALSE, 200);
        if (w == WAIT_TIMEOUT || w == WAIT_FAILED) continue;

        unsigned int slot = w - WAIT_OBJECT_0;
        if (slot >= nev) continue;

        int ai = owner[slot];
        unsigned int bi = sub[slot];
        DWORD bytes = 0;
        if (GetOverlappedResult(hdev, &acq[ai]->subBuffers[bi].overLapped, &bytes, FALSE))
        {
            completions++;
            if (bytes)
            {
                LOG("  t=%6.0f ms  buffer %d.%u completed: %lu bytes", NowMs() - t0, ai, bi, bytes);
                if (raw) { fwrite(acq[ai]->subBuffers[bi].pData, 1, bytes, raw); fflush(raw); }
                total += bytes;
            }
        }
        else LOG("  buffer %d.%u overlapped error %lu", ai, bi, GetLastError());

        ResetEvent(evs[slot]);
        acq[ai]->subBuffers[bi].overLapped.Offset     = 0;
        acq[ai]->subBuffers[bi].overLapped.OffsetHigh = 0;
        ISOCH_BUFFER_PARAMS bp;
        bp.ulFlags = (bi == 0) ? ISOCH_BUFFER_PRIMARY : ISOCH_BUFFER_SECONDARY;
        t1394IsochAttachBuffer(hdev, acq[ai]->subBuffers[bi].pData,
                               acq[ai]->subBuffers[bi].ulSize, &bp,
                               &acq[ai]->subBuffers[bi].overLapped);
    }

    if (raw) fclose(raw);
    g_lastCompletions = completions;
    LOG("  capture done: %d completions, %lu bytes -> isight-audio-raw.bin", completions, total);
    if (total) ScanRaw("isight-audio-raw.bin");
    else       LOG("  (no bytes at all - nothing to scan, the transmitter stayed silent)");

    t1394IsochStop((PSTR)dev);
    t1394IsochTearDownStream((PSTR)dev);
    CloseHandle(hdev);
    FreeAcq(acq, nAcq);
    LOG("  stream stopped");

    if (vidEnable)
    {
        WR(dev, VIDEO_ABS_BASE + V_ISO_ENABLE, 0, "V_ISO_ENABLE(stop)");
        if (vSaved)
            WR(dev, VIDEO_ABS_BASE + V_ISO_CHANNEL, vOldCh, "V_ISO_CHANNEL(restore)");
    }
    if (cfgBase && enableAudio)
        WR(dev, cfgBase + A_AUDIO_ENABLE, 0, "AUDIO_ENABLE");
    if (cfgBase && restoreCh >= 0)
    {
        ULONG txv = (ULONG)restoreCh | ((ULONG)SpeedIndex(flag) << 16);
        LOG("  restoring the camera's original transmit channel %d", restoreCh);
        WR(dev, cfgBase + A_ISO_TX_CONFIG, txv, "ISO_TX_CONFIG");
    }
    if (g_irmLocked)
    {
        LOG("  -- releasing IRM channel %u / bandwidth --", g_irmChannel);
        IrmSetChannelBit(dev, g_irmChannel, TRUE);
        IrmReleaseBandwidth(dev);
        g_irmLocked = FALSE;
    }
    (void)dumpRaw;
    return 0;
}

// capture: program the camera, then receive.  NOTE the order - SetupStream
// first (that is what allocates the channel), then the camera, then listen.
static int DoCapture(const char *dev, ULONG base, int seconds, int forcedCh, BOOL configTx)
{
    LOG("");
    LOG("== capture: audio base = 0x%08X, %d s, channel %s, camera config %s ==",
        base, seconds, forcedCh >= 0 ? "forced" : "auto",
        configTx ? "ON" : "OFF (listen only)");
    DoRegs(dev, base);

    ULONG tx = 0;
    int curCh = -1;
    if (RD(dev, base + A_ISO_TX_CONFIG, &tx, NULL))
    {
        curCh = (int)(tx & 0xFFFF);
        LOG("  camera currently transmits: channel %d, speed index %d (ISO_TX_CONFIG = 0x%08X)",
            curCh, (int)((tx >> 16) & 0xFFFF), tx);
        if (tx == 0 || curCh > 63) { curCh = -1; LOG("  -> that channel is invalid, treating as none"); }
    }

    // v10.  The v9 run of variant 3 finally produced audio - 1050 "sght"
    // packets, a real YUV picture alongside them - but with 74 dropouts
    // over 4.38 s: only ~43% of the microphone's frames.  The reason is
    // visible in the dump: the video engine was put on OUR OWN receive
    // channel, so the camera could only alternate one packet per cycle and
    // half the audio went missing.  v9 passed -1 for the video channel,
    // and -1 is VID_SAME, not VID_OTHER - the flag that was meant.
    // Apple's guide is explicit that audio and video use *different*
    // isochronous channels, so variant 3 now does exactly that.
    // Variant 4 keeps the video-first ordering, and variant 5 slows the
    // video frame rate down: the camera sends audio in the cycles video
    // leaves free, so fewer video frames means more room for the mic.
    struct { const char *what; BOOL audio, video, vidFirst; int vidCh; int vrate; }
    variants[] = {
        { "1/5  audio only: SAMPLE_RATE + ISO_TX_CONFIG, no enable",
          FALSE, FALSE, FALSE, 0,         -1 },
        { "2/5  + AUDIO_ENABLE, video engine still off",
          TRUE,  FALSE, FALSE, 0,         -1 },
        { "3/5  + AUDIO_ENABLE, video engine on ANOTHER channel",
          TRUE,  TRUE,  FALSE, VID_OTHER, -1 },
        { "4/5  video engine FIRST (another channel), then audio",
          TRUE,  TRUE,  TRUE,  VID_OTHER, -1 },
        { "5/5  as 4/5 but video frame rate lowered to 3.75 fps",
          TRUE,  TRUE,  TRUE,  VID_OTHER,  1 },
    };

    int rc = 0;
    if (!configTx)
    {
        rc = DoReceive(dev, forcedCh >= 0 ? forcedCh : -1, seconds, A_PAYLOAD_BYTES, TRUE,
                       0, -1, FALSE, FALSE, -1, FALSE, g_cmu, g_vidRate);
    }
    else
    {
        for (int v = 0; v < 5; ++v)
        {
            LOG("");
            LOG("  ---------- variant %s ----------", variants[v].what);
            rc = DoReceive(dev, forcedCh >= 0 ? forcedCh : -1, seconds, A_PAYLOAD_BYTES, TRUE,
                           base, curCh, variants[v].audio, variants[v].video,
                           variants[v].vidCh, variants[v].vidFirst, g_cmu,
                           variants[v].vrate >= 0 ? variants[v].vrate : g_vidRate);
            if (g_lastCompletions > 0)
            {
                LOG("  *** variant %d produced data - stopping the escalation ***", v + 1);
                break;
            }
        }
    }
    LOG("cap exit code %d", rc);
    return rc;
}

//---------------------------------------------------------------------
// validate the receive path with a known-good transmitter: the camera's
// own DCAM video engine, started by hand on a channel of our choosing.
// If this yields nothing, then "the camera stays silent" is not a
// conclusion we are allowed to draw - our listener would be at fault.
//---------------------------------------------------------------------
static int DoVidListen(const char *dev, int ch, int seconds, ULONG bpf)
{
    if (ch < -1 || ch > 63) { LOG("vidlisten needs a channel 0..63, or 'auto'"); return 1; }

    ULONG oldRate = 0, oldMode = 0, oldFmt = 0, oldCh = 0, oldEn = 0;
    RD(dev, VIDEO_ABS_BASE + V_FRAME_RATE,   &oldRate, NULL);
    RD(dev, VIDEO_ABS_BASE + V_VIDEO_MODE,   &oldMode, NULL);
    RD(dev, VIDEO_ABS_BASE + V_VIDEO_FORMAT, &oldFmt,  NULL);
    RD(dev, VIDEO_ABS_BASE + V_ISO_CHANNEL,  &oldCh,   NULL);
    RD(dev, VIDEO_ABS_BASE + V_ISO_ENABLE,   &oldEn,   NULL);

    LOG("");
    LOG("== receive-path self test: camera video engine -> %s channel, %d s, %u bytes/frame ==",
        ch < 0 ? "an auto-allocated" : "explicit", seconds, bpf);
    LOG("   video unit before: rate=0x%08X mode=0x%08X format=0x%08X 0x60C=0x%08X 0x614=0x%08X",
        oldRate, oldMode, oldFmt, oldCh, oldEn);
    // C1394Camera writes these as value<<29, so the low three bits of the
    // shift are the setting.  0/2/3 is Format 0 Mode 2 at 15 fps - exactly
    // what isight-diag.exe selects, i.e. the configuration that delivers
    // 90/90 frames.  If these numbers ever differ, the "known-good
    // transmitter" premise is broken and the test below means nothing.
    LOG("   decoded: format %u / mode %u / rate %u   (0/2/3 = 640x480 YUV 4:1:1 @15fps)",
        oldFmt >> 29, oldMode >> 29, oldRate >> 29);
    if (oldEn & 0x80000000u)
        LOG("   WARNING: the video engine is already enabled (0x614) - something else is streaming");

    // DoReceive does the whole sequence: SetupStream, buffers, Listen and then -
    // because vidEnable is set - the video 0x60C and 0x614 = 0x80000000 writes,
    // with both registers restored afterwards.
    int rc = DoReceive(dev, ch, seconds, bpf, TRUE, 0, -1, FALSE, TRUE, VID_SAME, FALSE,
                       g_cmu, g_vidRate);

    ULONG en = 0;
    RD(dev, VIDEO_ABS_BASE + V_ISO_ENABLE, &en, NULL);
    if (g_lastCompletions > 0)
        LOG("   ===> the receive path WORKS: %d sub-buffer completions came back", g_lastCompletions);
    else
        LOG("   ===> the receive path produced NOTHING even though the transmitter is known good");
    LOG("vidlisten exit code %d", rc);
    return rc;
}

//---------------------------------------------------------------------
// legacy relative sweep - now timed and logged one by one
//---------------------------------------------------------------------
static int DoSweep(const char *dev, ULONG from, ULONG count)
{
    LOG("");
    LOG("== relative sweep 0x%08X .. +%u quadlets, %lu ms timeout ==", from, count, g_timeout);
    int ok = 0;
    for (ULONG i = 0; i < count && g_leakedThreads < 300; ++i)
    {
        ULONG off = from + i * 4;
        ULONG v = 0; DWORD rc = 0;
        double t0 = NowMs();
        BOOL r = TimedReg(dev, off, &v, g_timeout, FALSE, 0, &rc);
        if (r && rc == ERROR_SUCCESS)
        {
            ok++;
            if ((i % 4) == 0) LOG("  %04X: %08X", off, v);
            else              LOG("  %04X: %08X", off, v);
        }
        else if (i % 32 == 0)
            LOG("  %04X: --- no answer (%.0f ms)", off, NowMs() - t0);
    }
    LOG("sweep: %d quadlets answered", ok);
    return ok;
}

//---------------------------------------------------------------------
int main(int argc, char **argv)
{
    g_log = fopen("isight-audio.txt", "w");
    LOG("=== iSight audio probe %s ===", __TIMESTAMP__);
    LOG("(v10 - THE MICROPHONE IS LIVE.  v9's variant 3 finally received audio:");
    LOG("      1050 'sght' packets, 210000 frames, alongside a real YUV picture -");
    LOG("      but with 74 dropouts, i.e. only ~43% of the microphone's frames.");
    LOG("      The dump explains why: v9 handed -1 as the video channel, and -1 is");
    LOG("      VID_SAME, not the VID_OTHER it was meant to be, so both streams");
    LOG("      ended up on our single receive channel and the camera could only");
    LOG("      alternate one packet per cycle.  Apple's guide is explicit that");
    LOG("      audio and video use DIFFERENT isochronous channels, so v10: (a)");
    LOG("      really does put the video engine on another channel, (b) adds a");
    LOG("      variant that also lowers the video frame rate - the camera sends");
    LOG("      audio in the cycles video leaves free, so a slower video means");
    LOG("      more room for the mic - and (c) stops trusting 0x614's readback,");
    LOG("      because in the run that produced a full video picture it read 0.)");
    LOG("(v9 - the receive path, reproduced from the one that works.  v8 let the");
    LOG("      driver ALLOCATE the channel, pointed the camera's video engine at");
    LOG("      it, saw 0x614 read back 0x80000000 ('streaming') - and still got");
    LOG("      ZERO bytes.  Since isight-diag.exe receives 90/90 frames from the");
    LOG("      same camera through the same driver, the difference has to be in");
    LOG("      how we set the stream up, not in the camera.  So v9 throws away");
    LOG("      v8's ad-hoc numbers and uses C1394Camera's exactly: 8 acquisition");
    LOG("      buffers of one full 640x480 YUV 4:1:1 frame (460800 bytes) built");
    LOG("      with a 960-byte packet budget, nNumberOfBuffers = subBuffers + 1,");
    LOG("      attach all 8, wait 50 ms and validate that every buffer is still");
    LOG("      ERROR_IO_INCOMPLETE (CMU treats 'already ready' as a hard error),");
    LOG("      and only then IsochListen.  'plain' on the command line gives");
    LOG("      v8's numbers back for an A/B; 'auto' for cap's channel argument is");
    LOG("      no longer eaten by atoi() and turned into channel 0.)");
    LOG("(v8 - the receive path, checked properly.  v7's vidlisten asked for");
    LOG("      an EXPLICIT channel, the driver answered 0xFFFFFFFF and we");
    LOG("      treated that as 'listening on the channel you named' - then got");
    LOG("      zero bytes from a transmitter we know is good, which proves");
    LOG("      nothing about the camera and everything about our assumption.");
    LOG("      So: vidlisten now takes 'auto' and lets the driver ALLOCATE the");
    LOG("      channel - the only thing the working CMU path ever does - which");
    LOG("      makes it a real test of the receive path.  Everything that");
    LOG("      writes a camera register now reads it back (0x60C, 0x614, 0x300:");
    LOG("      'the write reported OK' has fooled us twice), the capture dump is");
    LOG("      searched for the 'sght' signature and the first samples are");
    LOG("      decoded, and 'cap' gained a fourth variant - video engine FIRST");
    LOG("      and audio second, because the iSight's audio clock is derived");
    LOG("      from the video engine and that is the one ordering v6/v7 never");
    LOG("      tried.  The camera is also programmed after IsochListen instead");
    LOG("      of before SetupStream, and always on the channel SetupStream");
    LOG("      actually returned.)");
    LOG("(v7 - v6 plus one addressing fix: the DCAM video registers are only");
    LOG("      reachable with *relative* offsets (the CMU driver adds the camera's");
    LOG("      CSR offset 0xF00000 = the IIDC base), whereas the audio unit needs");
    LOG("      the absolute form 0xF0020000.  v6's vidlisten/vregs used the absolute");
    LOG("      form for video and every access silently failed.)");
    LOG("(v6 - two reference implementations read, both agree with our register set:");
    LOG("      Linux sound/firewire/isight.c and FreeBSD's new fwisound.c.  Their");
    LOG("      sequences are SAMPLE_RATE -> ISO_TX_CONFIG -> AUDIO_ENABLE, and the");
    LOG("      FreeBSD driver reserves NOTHING in the IRM, so the IRM hypothesis is");
    LOG("      dead.  v6 therefore (a) accepts CMU's 'subscribe' reply (explicit");
    LOG("      channel: success + 0xFFFFFFFF means 'no allocation, listening on the");
    LOG("      channel you named'), (b) adds vidlisten, a receive-path self test");
    LOG("      using the camera's own video engine, and (c) escalates cap through");
    LOG("      the three 'why is the camera silent' variants, the last of which");
    LOG("      runs the video engine on the same channel because Apple's guide says");
    LOG("      audio packets are only sent in the gaps between video packets.)");

    const char *mode = (argc > 1) ? argv[1] : "rom";
    LOG("mode: %s", mode);

    for (int i = 1; i < argc; ++i)
    {
        if (!_stricmp(argv[i], "irm"))   g_irm = TRUE;
        if (!_stricmp(argv[i], "cmu"))   g_cmu = TRUE;
        if (!_stricmp(argv[i], "plain")) g_cmu = FALSE;
        if (!_strnicmp(argv[i], "vrate=", 6)) g_vidRate = atoi(argv[i] + 6);
    }
    if (g_irm) LOG("IRM mode: on - the channel/bandwidth claim will be attempted (and will fail)");
    else       LOG("IRM mode: off (pass 'irm' as an extra argument to attempt the claim)");
    LOG("buffer mode: %s (pass 'plain' for v8's ad-hoc numbers, 'cmu' for C1394Camera's)",
        g_cmu ? "CMU-style - 8 x full-frame buffers, 960 B/packet, subBuffers+1"
              : "plain - one small buffer");
    if (g_vidRate >= 0)
        LOG("video frame rate override: rate %d (DCAM index, 0=1.875 .. 5=60 fps)", g_vidRate);

    HDEVINFO hDev = t1394CmdrGetDeviceList();
    if (hDev == INVALID_HANDLE_VALUE) { LOG("t1394CmdrGetDeviceList FAILED"); return 1; }

    char path[MAX_PATH] = "";
    ULONG len = sizeof(path);
    DWORD got = t1394CmdrGetDevicePath(hDev, 0, path, &len);
    LOG("t1394CmdrGetDevicePath(0) -> %lu  path='%s'", got, path);
    if (got <= 0 || path[0] == 0) { LOG("no CMU 1394 device found"); return 1; }

    ULONG from = 0, to = 0, step = 0;
    if (argc > 2) from = (ULONG)strtoul(argv[2], NULL, 0);

    if (!_stricmp(mode, "rom"))
    {
        int quads = (argc > 2) ? (int)strtoul(argv[2], NULL, 0) : 96;
        DoRom(path, quads);
    }
    else if (!_stricmp(mode, "regs"))
    {
        if (from == 0) { LOG("usage: isight-audio.exe regs <absBaseHex>"); return 1; }
        DoRegs(path, from);
    }
    else if (!_stricmp(mode, "scan"))
    {
        if (argc > 3) to   = (ULONG)strtoul(argv[3], NULL, 0);
        if (argc > 4) step = (ULONG)strtoul(argv[4], NULL, 0);
        if (argc > 5) g_timeout = (ULONG)strtoul(argv[5], NULL, 0);
        if (!to)   to   = from + 0x100000;
        if (!step) step = 0x10000;
        DoScan(path, from, to, step);
    }
    else if (!_stricmp(mode, "sweep"))
    {
        ULONG count = (argc > 3) ? (ULONG)strtoul(argv[3], NULL, 0) : 0x400;
        if (argc > 4) g_timeout = (ULONG)strtoul(argv[4], NULL, 0);
        DoSweep(path, from, count);
    }
    else if (!_stricmp(mode, "cap"))
    {
        int secs = 3, fch = -1;
        BOOL cfg = TRUE;
        if (from == 0) { LOG("usage: isight-audio.exe cap <absBaseHex> [seconds] [channel|auto] [noconfig]"); return 1; }
        if (argc > 3) secs = atoi(argv[3]);
        // "auto" must not go through atoi() - that turns it into channel 0,
        // which is an explicit channel, and the two are completely different
        // experiments (allocation vs subscribe).
        if (argc > 4) fch  = _stricmp(argv[4], "auto") ? atoi(argv[4]) : -1;
        if (argc > 5 && !_stricmp(argv[5], "noconfig")) cfg = FALSE;
        DoCapture(path, from, secs, fch, cfg);
    }
    else if (!_stricmp(mode, "listen"))
    {
        // pure isochronous receive: touches no camera register at all
        int secs = 3, ch = -1;
        ULONG bpf = A_PAYLOAD_BYTES;
        if (argc > 2 && _stricmp(argv[2], "auto")) ch = atoi(argv[2]);
        if (argc > 3) secs = atoi(argv[3]);
        if (argc > 4) bpf  = (ULONG)strtoul(argv[4], NULL, 0);
        LOG("");
        LOG("== listen only: channel %s, %d s, %u bytes/frame max ==",
            ch < 0 ? "auto" : "explicit", secs, bpf);
        int rc = DoReceive(path, ch, secs, bpf, TRUE, 0, -1, FALSE, FALSE, -1, FALSE,
                           g_cmu, g_vidRate);
        LOG("listen exit code %d", rc);
    }
    else if (!_stricmp(mode, "vidlisten"))
    {
        // self test: start the camera's video engine on <chan> and receive it
        // "auto" makes the driver *allocate* the channel - which is the only
        // thing the known-good CMU path ever does - so this is the variant
        // that must work if our receive code is correct at all.
        int ch = -1, secs = 3;
        ULONG bpf = 4096;
        if (argc > 2) ch = _stricmp(argv[2], "auto") ? atoi(argv[2]) : -1;
        if (argc > 3) secs = atoi(argv[3]);
        if (argc > 4) bpf = (ULONG)strtoul(argv[4], NULL, 0);
        DoVidListen(path, ch, secs, bpf);
    }
    else if (!_stricmp(mode, "vregs"))
    {
        LOG("");
        LOG("== video unit DCAM registers (relative offsets, driver adds 0xF00000) ==");
        const struct { ULONG off; const char *n; } vr[] = {
            { V_FRAME_RATE,   "FRAME_RATE"   },
            { V_VIDEO_MODE,   "VIDEO_MODE"   },
            { V_VIDEO_FORMAT, "VIDEO_FORMAT" },
            { V_ISO_CHANNEL,  "ISO_CHANNEL"  },
            { 0x610,          "0x610"        },
            { V_ISO_ENABLE,   "ISO_ENABLE"   },
            { 0x618,          "0x618"        },
        };
        for (int i = 0; i < 7; ++i)
        {
            ULONG v = 0;
            if (RD(path, VIDEO_ABS_BASE + vr[i].off, &v, NULL))
                LOG("  %04X  %-14s = 0x%08X", vr[i].off, vr[i].n, v);
            else
                LOG("  %04X  %-14s = <no answer>", vr[i].off, vr[i].n);
        }
    }
    else if (!_stricmp(mode, "irm"))
    {
        // Read (and optionally hand-edit) the isochronous resource manager
        // registers.  "lock <ch>" claims a channel, "unlock <ch>" gives it
        // back; with no argument it just reports what the bus looks like.
        if (argc > 2 && !_stricmp(argv[2], "lock"))
        {
            ULONG ch = (argc > 3) ? (ULONG)strtoul(argv[3], NULL, 0) : 0;
            LOG("");
            LOG("== IRM: manually acquire channel %u ==", ch);
            IrmSetChannelBit(path, ch, FALSE);
        }
        else if (argc > 2 && !_stricmp(argv[2], "unlock"))
        {
            ULONG ch = (argc > 3) ? (ULONG)strtoul(argv[3], NULL, 0) : 0;
            LOG("");
            LOG("== IRM: manually release channel %u ==", ch);
            IrmSetChannelBit(path, ch, TRUE);
        }
        else IrmDump(path);
    }
    else if (!_stricmp(mode, "poke"))
    {
        ULONG val = 0;
        if (argc < 4) { LOG("usage: isight-audio.exe poke <absOffsetHex> <valueHex>"); return 1; }
        val = (ULONG)strtoul(argv[3], NULL, 0);
        LOG("");
        LOG("== poke 0x%08X <- 0x%08X ==", from, val);
        WR(path, from, val, "poke");
        ULONG rb = 0;
        if (RD(path, from, &rb, NULL)) LOG("  readback = 0x%08X", rb);
    }
    else
    {
        LOG("usage: isight-audio.exe rom [quadlets]");
        LOG("       isight-audio.exe regs <absBaseHex>          (audio unit registers)");
        LOG("       isight-audio.exe vregs                      (video unit DCAM registers)");
        LOG("       isight-audio.exe poke <absOffsetHex> <valueHex>");
        LOG("       isight-audio.exe irm [lock|unlock <channel>]");
        LOG("       isight-audio.exe listen <chan|auto> [seconds] [bytesPerFrame]");
        LOG("       isight-audio.exe cap <absBaseHex> [seconds] [channel] [noconfig]");
        LOG("       isight-audio.exe vidlisten <chan|auto> [seconds] [bytesPerFrame]");
        LOG("       isight-audio.exe scan <fromHex> <toHex> <stepHex> [timeoutMs]");
        LOG("       isight-audio.exe sweep <fromHex> <count> [timeoutMs]  (relative)");
        LOG("");
        LOG("cap escalates through four variants: audio only, +AUDIO_ENABLE,");
        LOG("+AUDIO_ENABLE with the video engine on another channel, then finally");
        LOG("video engine FIRST and audio second (the camera-clock hypothesis).");
        LOG("vidlisten 'auto' lets the driver allocate the channel, like the");
        LOG("working CMU path does; an explicit number is a subscribe, which the");
        LOG("driver may not support at all - compare the two before believing.");
    }

    LOG("=== done === (abandoned register threads: %d)", g_leakedThreads);
    if (g_log) fclose(g_log);
    return 0;
}
