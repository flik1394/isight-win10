// isightmic.h - shared definitions between the kernel driver and the user-mode
// feeder.  Keep this file free of kernel headers: the DirectShow filter and the
// feeder service include it under the normal user-mode toolchain, so the
// includer is responsible for having CTL_CODE defined already (windows.h +
// winioctl.h in user mode, ntddk.h in kernel mode).
#pragma once

// The virtual mic exposes a single capture endpoint:
//   48 kHz / 16-bit / mono, signed little-endian PCM.
#define ISIGHTMIC_SAMPLERATE   48000
#define ISIGHTMIC_BITS         16
#define ISIGHTMIC_CHANNELS     1   // the physical feeder stream is mono
#define ISIGHTMIC_MAX_CHANNELS  2   // but we advertise up to stereo so the audio
                                    // engine's stereo capture request intersects
#define ISIGHTMIC_FRAME_BYTES  (ISIGHTMIC_BITS / 8 * ISIGHTMIC_CHANNELS)   // 2

// How much audio the driver hands out per wake-up (10 ms at 48 kHz mono).
#define ISIGHTMIC_QUANTUM_BYTES (ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES / 100)

// IOCTL the feeder uses to push PCM into the driver's ring buffer.
// Input buffer = raw PCM bytes (multiple of ISIGHTMIC_FRAME_BYTES).
// METHOD_BUFFERED: the system copies the caller's buffer into a system buffer
// for us, so we never touch user memory directly.
#define IOCTL_ISIGHTMIC_PUSH \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// Ask the driver how it is doing.  Output buffer = ISIGHTMIC_STATUS.
#define IOCTL_ISIGHTMIC_GETSTATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_READ_ACCESS)

// Ask the loaded .sys which build it is.  Output buffer = a NUL-terminated
// narrow string.  A separate IOCTL rather than a new ISIGHTMIC_STATUS field on
// purpose: the DirectShow filter includes this header too, and extending the
// struct would drag the filter into a rebuild for a diagnostic.
#define IOCTL_ISIGHTMIC_GETBUILD \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_READ_ACCESS)

// NewStream break-down, for telling "the audio engine never instantiated the
// pin" apart from "it did, and we failed inside NewStream" -- two problems
// with opposite fixes.  Output buffer = ISIGHTMIC_DIAG.  Same rule as above:
// its own IOCTL and its own struct, so adding a counter never forces the
// DirectShow filter to be rebuilt.
#define IOCTL_ISIGHTMIC_GETDIAG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED, FILE_READ_ACCESS)

// Bumped whenever the driver changes.  Kept in the same shape as the filter's
// tag so one grep over a binary answers "which build is this".
#define ISIGHTMIC_BUILD_TAG "ISIGHTMIC-BUILD-V30-20260926-RUNCHAIN"

// Old name, kept so a stale header/test build still links.
#define IOCTL_ISIGHTMIC_GETLEVEL IOCTL_ISIGHTMIC_GETSTATUS

typedef struct _ISIGHTMIC_STATUS {
    unsigned long Buffered;   // bytes waiting to be played out
    unsigned long Pushed;     // total bytes accepted from user mode
    unsigned long Played;     // total bytes handed to the audio engine
    unsigned long Starved;    // bytes of silence inserted because the ring was dry
    unsigned long Streams;    // capture streams created so far
    unsigned long State;      // KSSTATE of the last stream (0=stop .. 3=run)
    unsigned long Opens;      // how many times the control device was opened
} ISIGHTMIC_STATUS, *PISIGHTMIC_STATUS;

// Where the audio stack actually got to.  Read top to bottom: the first column
// that is still zero is the step that never happened, and that is the bug.
//
// This exists because "the endpoint is published but closes the instant you
// open it" has at least four very different causes -- the engine never opened
// the filter, it opened it but never asked about formats, it asked about
// formats but never instantiated the pin, or it instantiated the pin and we
// failed inside NewStream -- and they all look identical from WASAPI, which
// can only report AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008) for every one.
typedef struct _ISIGHTMIC_DIAG {
    // --- keep these six in this order: they are the v23 fields ---
    unsigned long NewStreamEntered;   // PortCls called IMiniportWaveCyclic::NewStream
    unsigned long NewStreamFailed;    // ... and we returned a failure
    unsigned long FailDma;            // IDmaChannel::AllocateBuffer failed
    unsigned long FailStreamInit;     // stream Init / stream object alloc failed
    unsigned long FailServiceGroup;   // PcNewServiceGroup failed
    unsigned long LastFailStatus;     // NTSTATUS of the most recent failure
    // --- v24 additions ---
    unsigned long WaveInitCalls;      // wave filter instances created
    unsigned long TopoInitCalls;      // topology filter instances created
    unsigned long WaveIntersect;      // wave DataRangeIntersection calls
    unsigned long WaveIntersectProbe; // ... of which were length-only probes
    unsigned long WaveIntersectLastPin;
    unsigned long WaveIntersectLastOutLen;  // OutputBufferLength the caller offered
    unsigned long WaveIntersectLastStatus;  // what we returned to the last call
    unsigned long WaveIntersectReqSpec;     // requested Specifier, first ULONG
    unsigned long TopoIntersect;      // topology DataRangeIntersection calls
    // --- v25 additions ---
    unsigned long WaveIntersectPhase2;   // second-stage (write-format) calls
    unsigned long ClientChannels;        // last requested channels (MatchingDataRange)
    unsigned long ClientSampleRate;      // last requested sample rate
    unsigned long ClientBits;            // last requested bits per sample
    // --- v30 additions: the RUN -> Service chain ---------------------------
    unsigned long StateLast;             // g_State when diag was read (0..3)
    unsigned long DpcFires;              // stream timer DPC actually executed
    unsigned long NotifyCalls;           // port->Notify(serviceGroup) attempts
    unsigned long ServiceCalls;          // stream Service() invocations (any kind)
} ISIGHTMIC_DIAG, *PISIGHTMIC_DIAG;

#define ISIGHTMIC_CTL_DEVICE_NAME  L"\\Device\\IsightMicCtl"
#define ISIGHTMIC_CTL_DOS_NAME     L"\\DosDevices\\IsightMicCtl"
// What user mode actually opens (the Win32 form of the same device).  Kernel
// code uses the two names above, user-mode callers this one.
#define ISIGHTMIC_CTL_WIN32_NAME   L"\\\\.\\IsightMicCtl"
#define ISIGHTMIC_POOL_TAG         'cMsi'
