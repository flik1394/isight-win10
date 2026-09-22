// isightmic.h - shared definitions between the kernel driver and the user-mode
// feeder service.  Keep this file self-contained (no kernel headers) so the
// service can #include it under the normal user-mode toolchain.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The virtual mic exposes a single capture endpoint:
//   48 kHz / 16-bit / mono, signed little-endian PCM.
#define ISIGHTMIC_SAMPLERATE   48000
#define ISIGHTMIC_BITS         16
#define ISIGHTMIC_CHANNELS     1
#define ISIGHTMIC_FRAME_BYTES  (ISIGHTMIC_BITS / 8 * ISIGHTMIC_CHANNELS)   // 2

// IOCTL the feeder service uses to push PCM into the driver's ring buffer.
// Input buffer = raw PCM bytes (multiple of ISIGHTMIC_FRAME_BYTES).
// METHOD_BUFFERED: the system copies the caller's buffer into a system buffer
// for us, so we never touch user memory directly.
#define IOCTL_ISIGHTMIC_PUSH \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// Optional: ask the driver how many bytes are still buffered (for flow control).
#define IOCTL_ISIGHTMIC_GETLEVEL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_READ_ACCESS)

#define ISIGHTMIC_CTL_DEVICE_NAME  L"\\Device\\IsightMicCtl"
#define ISIGHTMIC_CTL_DOS_NAME     L"\\DosDevices\\IsightMicCtl"
#define ISIGHTMIC_POOL_TAG         'cMsi'

#ifdef __cplusplus
}
#endif
