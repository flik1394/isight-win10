// pcprobe.cpp -- is the Port Class (portcls) audio framework usable here?
//
// The iSight microphone has to surface as a real capture endpoint, which on
// Windows means a PortCls miniport driver.  portcls.h is NOT part of the
// plain Windows SDK -- it only ships with the WDK -- so we cannot assume it
// is present.  This file compiles (never links, never runs) against the
// PortCls surface we intend to use and thereby answers the question.
//
// Built by .github/workflows/build.yml (job "kmprobe"); failure is allowed
// and is itself the answer.

#include <ntddk.h>
#include <ks.h>
#include <ksmedia.h>
#include <portcls.h>
#include <drmk.h>

// Touch the three PortCls entry points a virtual mic actually needs.
// Taking their address proves the headers declare them.
static const void *g_probes[] = {
    (void *)&PcInitializeAdapterDriver,
    (void *)&PcNewMiniport,
    (void *)&PcNewPort,
};

// A wave-cyclic miniport is the shape MSVAD uses.  We only need the vtable
// to exist at compile time, so declare an incomplete use of it.
static IMiniportWaveCyclic *g_miniport;

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT  DriverObject,
                     _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(g_probes);
    UNREFERENCED_PARAMETER(g_miniport);
    return STATUS_SUCCESS;
}
