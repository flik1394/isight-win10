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
#define ISIGHTMIC_CHANNELS     1
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

// Bumped whenever the driver changes.  Kept in the same shape as the filter's
// tag so one grep over a binary answers "which build is this".
#define ISIGHTMIC_BUILD_TAG "ISIGHTMIC-BUILD-V21-20260923-PORTCLS"

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

#define ISIGHTMIC_CTL_DEVICE_NAME  L"\\Device\\IsightMicCtl"
#define ISIGHTMIC_CTL_DOS_NAME     L"\\DosDevices\\IsightMicCtl"
// What user mode actually opens (the Win32 form of the same device).  Kernel
// code uses the two names above, user-mode callers this one.
#define ISIGHTMIC_CTL_WIN32_NAME   L"\\\\.\\IsightMicCtl"
#define ISIGHTMIC_POOL_TAG         'cMsi'
