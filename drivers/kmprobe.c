// kmprobe.c -- the smallest possible kernel-mode driver.
//
// It exists for one reason: to answer "can this build agent produce a .sys
// at all?"  The iSight virtual microphone has to be a kernel driver, and
// GitHub's windows-2022 image ships a WDK of unknown completeness, so before
// writing a single line of PortCls code we prove that
//
//     cl /kernel   +   link /DRIVER
//
// actually work here.  Nothing in this file is ever installed anywhere.
//
// Built by .github/workflows/build.yml (job "kmprobe"), which tolerates
// failure -- a red result here is information, not a broken build.

#include <ntddk.h>

static VOID KmProbeUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject,
                     _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = KmProbeUnload;
    return STATUS_SUCCESS;
}
