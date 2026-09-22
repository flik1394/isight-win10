// isightmic.cpp
//
// A kernel-mode PortCls WaveCyclic capture miniport that appears in Windows as a
// real recording device named "iSight Microphone (FireWire)".  It does NOT talk
// to the camera.  Instead a user-mode service (isight-micsvc.exe) pushes already
// processed 48 kHz / 16-bit / mono PCM into the driver through an IOCTL, and this
// driver streams that PCM out to the audio engine through a cyclic buffer.
//
// The kernel side is deliberately tiny: one ring buffer + one timer that copies
// from the ring into the PortCls cyclic buffer.  All DSP (PLC, denoise, gate,
// EQ, exciter, ...) lives in user mode.
//
// Build (see make.sys.bat):
//   cl /nologo /kernel /c /W3 /O2 /I<WDK include> isightmic.cpp
//   link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64
//        /LIBPATH:<WDK lib> portcls.lib ks.lib drmk.lib ntoskrnl.lib hal.lib
//        wmilib.lib /OUT:isightmic.sys isightmic.obj

#include <ntddk.h>
#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>

#include "isightmic.h"

// ---------------------------------------------------------------------------
// Kernel new/delete
// ---------------------------------------------------------------------------
void* __cdecl operator new(size_t size, POOL_TYPE poolType, ULONG tag) {
    return ExAllocatePoolWithTag(poolType, size, tag);
}
void __cdecl operator delete(void* p, ULONG tag) {
    UNREFERENCED_PARAMETER(tag);
    if (p) ExFreePoolWithTag(p, ISIGHTMIC_POOL_TAG);
}
void __cdecl operator delete(void* p) {
    if (p) ExFreePoolWithTag(p, ISIGHTMIC_POOL_TAG);
}

// ---------------------------------------------------------------------------
// Global ring buffer (fed by the IOCTL, drained by the capture timer)
// ---------------------------------------------------------------------------
#define RING_SECONDS   2
#define RING_BYTES     (ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES * RING_SECONDS)

typedef struct _RING {
    PUCHAR       Buffer;
    ULONG        Cap;        // total capacity in bytes
    ULONG        Head;       // driver read index
    ULONG        Tail;       // service write index
    ULONG        Count;      // bytes currently available
    KSPIN_LOCK   Lock;
} RING, *PRING;

static RING g_Ring;

static void RingInit(PRING r) {
    r->Buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, RING_BYTES, ISIGHTMIC_POOL_TAG);
    r->Cap = (r->Buffer != NULL) ? RING_BYTES : 0;
    r->Head = r->Tail = r->Count = 0;
    KeInitializeSpinLock(&r->Lock);
}

// Push 'n' bytes from 'src' into the ring.  Drop oldest if full.
static void RingPush(PRING r, const PUCHAR src, ULONG n) {
    if (r->Buffer == NULL || n == 0) return;
    KIRQL irql;
    KeAcquireSpinLock(&r->Lock, &irql);
    for (ULONG i = 0; i < n; i++) {
        r->Buffer[r->Tail] = src[i];
        r->Tail = (r->Tail + 1) % r->Cap;
        if (r->Count < r->Cap) r->Count++;
        else r->Head = (r->Head + 1) % r->Cap;  // full: discard oldest
    }
    KeReleaseSpinLock(&r->Lock, irql);
}

// Read up to 'n' bytes from the ring into 'dst'.  Returns bytes actually read.
static ULONG RingPull(PRING r, PUCHAR dst, ULONG n) {
    KIRQL irql;
    ULONG got = 0;
    KeAcquireSpinLock(&r->Lock, &irql);
    while (got < n && r->Count > 0) {
        dst[got++] = r->Buffer[r->Head];
        r->Head = (r->Head + 1) % r->Cap;
        r->Count--;
    }
    KeReleaseSpinLock(&r->Lock, irql);
    return got;
}

// ---------------------------------------------------------------------------
// COM helpers (single-interface objects)
// ---------------------------------------------------------------------------
#define IMP_IUNKNOWN(className)                                           \
public:                                                                  \
    STDMETHODIMP_(ULONG) AddRef() {                                       \
        return InterlockedIncrement(&m_RefCount);                         \
    }                                                                     \
    STDMETHODIMP_(ULONG) Release() {                                      \
        ULONG c = InterlockedDecrement(&m_RefCount);                      \
        if (c == 0) { delete this; }                                     \
        return c;                                                         \
    }                                                                     \
    STDMETHODIMP QueryInterface(REFIID iid, PVOID* ppv) {                \
        if (!ppv) return STATUS_INVALID_PARAMETER;                        \
        *ppv = NULL;                                                      \
        if (IsEqualGUIDAligned(iid, IID_IUnknown)) {                     \
            *ppv = (PVOID)(IUnknown*)this;                               \
        } else if (IsEqualGUIDAligned(iid, __uuidof(NTNAME))) {          \
            *ppv = (PVOID)(NTNAME*)this;                                 \
        }                                                                 \
        if (*ppv) { AddRef(); return STATUS_SUCCESS; }                    \
        return STATUS_INVALID_PARAMETER_1;                                \
    }                                                                     \
protected:                                                               \
    LONG m_RefCount = 1;

// ---------------------------------------------------------------------------
// Capture stream
// ---------------------------------------------------------------------------
class CMiniportWaveCyclicStream;
typedef CMiniportWaveCyclicStream* PCMiniportWaveCyclicStream;

class CMiniportWaveCyclicStream : public IMiniportWaveCyclicStream {
    IMP_IUNKNOWN(IMiniportWaveCyclicStream)
public:
    CMiniportWaveCyclicStream(PUNKNOWN outer);
    ~CMiniportWaveCyclicStream();

    // IMiniportWaveCyclicStream
    STDMETHODIMP GetPosition(OUT ULONG* Position);
    STDMETHODIMP NormalRead(OUT PVOID Buffer, IN ULONG BytesToRead, OUT PULONG BytesRead);
    STDMETHODIMP NormalWrite(IN PVOID Buffer, IN ULONG BytesToWrite, OUT PULONG BytesWritten);
    STDMETHODIMP Silence(IN PVOID Buffer, IN ULONG BytesToSilence);
    STDMETHODIMP SetFormat(IN PKSDATAFORMAT DataFormat);
    STDMETHODIMP SetState(IN KSSTATE State);
    STDMETHODIMP GetSegmentSize(OUT ULONG* SegmentSize);

    NTSTATUS Init(IN PCMiniportWaveCyclic Miniport,
                  IN PPORTWAVECYCLICSTREAM PortStream,
                  IN ULONG Pin,
                  IN BOOLEAN Capture,
                  IN PKSDATAFORMAT DataFormat);

    void Service();   // called by the timer DPC

    PCMiniportWaveCyclic    m_Miniport;
    PPORTWAVECYCLICSTREAM   m_PortStream;
    PVOID                   m_Buffer;       // cyclic buffer (from AllocateBuffer)
    ULONG                   m_BufferSize;
    ULONG                   m_Position;     // bytes produced (== GetPosition)
    KSSTATE                 m_State;
    KTIMER                  m_Timer;
    KDPC                    m_Dpc;
    BOOLEAN                 m_TimerOn;
};

CMiniportWaveCyclicStream::CMiniportWaveCyclicStream(PUNKNOWN outer) {
    m_Miniport = NULL;
    m_PortStream = NULL;
    m_Buffer = NULL;
    m_BufferSize = 0;
    m_Position = 0;
    m_State = KSSTATE_STOP;
    m_TimerOn = FALSE;
}

CMiniportWaveCyclicStream::~CMiniportWaveCyclicStream() {
    if (m_TimerOn) {
        KeCancelTimer(&m_Timer);
        m_TimerOn = FALSE;
    }
}

NTSTATUS CMiniportWaveCyclicStream::Init(IN PCMiniportWaveCyclic Miniport,
                                         IN PPORTWAVECYCLICSTREAM PortStream,
                                         IN ULONG Pin,
                                         IN BOOLEAN Capture,
                                         IN PKSDATAFORMAT DataFormat) {
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(Capture);
    UNREFERENCED_PARAMETER(DataFormat);
    PAGED_CODE();
    m_Miniport = Miniport;
    m_PortStream = PortStream;
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclicStream::GetPosition(OUT ULONG* Position) {
    if (!Position) return STATUS_INVALID_PARAMETER;
    *Position = m_Position;
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclicStream::NormalRead(OUT PVOID Buffer,
                                                  IN ULONG BytesToRead,
                                                  OUT PULONG BytesRead) {
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BytesToRead);
    UNREFERENCED_PARAMETER(BytesRead);
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclicStream::NormalWrite(IN PVOID Buffer,
                                                    IN ULONG BytesToWrite,
                                                    OUT PULONG BytesWritten) {
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BytesToWrite);
    UNREFERENCED_PARAMETER(BytesWritten);
    // capture: the driver fills the buffer, the port never writes to us
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclicStream::Silence(IN PVOID Buffer,
                                                IN ULONG BytesToSilence) {
    if (Buffer && BytesToSilence) {
        RtlZeroMemory(Buffer, BytesToSilence);   // 16-bit PCM silence == 0
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclicStream::SetFormat(IN PKSDATAFORMAT DataFormat) {
    UNREFERENCED_PARAMETER(DataFormat);
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclicStream::GetSegmentSize(OUT ULONG* SegmentSize) {
    if (!SegmentSize) return STATUS_INVALID_PARAMETER;
    *SegmentSize = 0;
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclicStream::SetState(IN KSSTATE State) {
    KIRQL irql;
    UNREFERENCED_PARAMETER(irql);
    m_State = State;
    if (State == KSSTATE_RUN) {
        if (!m_TimerOn && m_Buffer && m_BufferSize) {
            LARGE_INTEGER due;
            due.QuadPart = -100000;   // 10 ms relative
            KeInitializeDpc(&m_Dpc, (PKDEFERRED_ROUTINE)StreamTimerDpc, (PVOID)this);
            KeSetTimerEx(&m_Timer, due, 10, &m_Dpc);   // 10 ms periodic
            m_TimerOn = TRUE;
        }
    } else {
        if (m_TimerOn) {
            KeCancelTimer(&m_Timer);
            m_TimerOn = FALSE;
        }
    }
    return STATUS_SUCCESS;
}

// Called every 10 ms: push one 10 ms quantum (960 bytes) of PCM from the ring
// into the cyclic buffer at the current position.  Pad with silence if the ring
// is short so the timeline stays real-time.
void CMiniportWaveCyclicStream::Service() {
    if (m_State != KSSTATE_RUN || !m_Buffer || !m_BufferSize) return;

    const ULONG quantum = ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES / 100; // 10 ms
    ULONG writePos = m_Position % m_BufferSize;
    PUCHAR dst = (PUCHAR)m_Buffer + writePos;

    // bytes we can write before wrapping
    ULONG toEnd = m_BufferSize - writePos;
    ULONG first = (quantum < toEnd) ? quantum : toEnd;
    ULONG got = RingPull(&g_Ring, dst, first);
    if (got < first) {
        RtlZeroMemory(dst + got, first - got);   // silence pad
    }
    if (quantum > first) {
        ULONG second = quantum - first;
        ULONG got2 = RingPull(&g_Ring, (PUCHAR)m_Buffer, second);
        if (got2 < second) RtlZeroMemory((PUCHAR)m_Buffer + got2, second - got2);
    }

    m_Position += quantum;
    if (m_PortStream) {
        m_PortStream->Notify(m_Position);
    }
}

extern "C" VOID StreamTimerDpc(IN PKDPC Dpc,
                               IN PVOID DeferredContext,
                               IN PVOID SystemArgument1,
                               IN PVOID SystemArgument2) {
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    CMiniportWaveCyclicStream* s = (CMiniportWaveCyclicStream*)DeferredContext;
    s->Service();
}

// ---------------------------------------------------------------------------
// Wave miniport (capture)
// ---------------------------------------------------------------------------
class CMiniportWaveCyclic : public IMiniportWaveCyclic {
    IMP_IUNKNOWN(IMiniportWaveCyclic)
public:
    CMiniportWaveCyclic(PUNKNOWN outer);
    ~CMiniportWaveCyclic();

    STDMETHODIMP Init(IN PUNKNOWN UnknownAdapter,
                      IN PRESOURCELIST ResourceList,
                      IN PPORTWAVECYCLIC Port);
    STDMETHODIMP NewStream(OUT PMINIPORTWAVECYCLICSTREAM* Stream,
                           IN PUNKNOWN OuterUnknown,
                           IN POOL_TYPE PoolType,
                           IN ULONG Pin,
                           IN BOOLEAN Capture,
                           IN PKSDATAFORMAT DataFormat,
                           OUT PDMACHANNEL* DmaChannel,
                           OUT PSERVICEGROUP* ServiceGroup);
    STDMETHODIMP GetDescription(OUT PPCFILTER_DESCRIPTOR Description);
    STDMETHODIMP DataRangeIntersection(IN ULONG PinId,
                                       IN PKSDATARANGE DataRange,
                                       IN PKSDATARANGE MatchingDataRange,
                                       IN ULONG OutputBufferLength,
                                       OUT PVOID ResultantFormat,
                                       OUT PULONG ResultantFormatLength);

    PPORTWAVECYCLIC   m_Port;
    PCMiniportWaveCyclicStream m_Stream;
};

// 48 kHz / 16-bit / mono capture data range
static KSDATARANGE_AUDIO PinDataRangesStream[] = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0, 0, 0,
        STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX,
        KSAUDIO_SPEAKER_MONO,
        16, 16,
        ISIGHTMIC_SAMPLERATE, ISIGHTMIC_SAMPLERATE,
        sizeof(WAVEFORMATEX)
    }
};
static PKSDATARANGE PinDataRangePointers[] = {
    (PKSDATARANGE)&PinDataRangesStream[0]
};

static KSPIN_DESCRIPTOR_EX CapturePinDesc = {
    KSPIN_COMMUNICATION_SINK,
    KSPIN_DIRECTION_IN,
    KSPIN_FLAG_HOLDING,
    NULL, NULL,
    { STATIC_KSCATEGORY_AUDIO, 0, 0, 0 },   // filled by PORTCLS via category
    0, 0, 0
};

static KSPIN_DESCRIPTOR CapturePin = {
    NULL,                          // AutomationTable
    &CapturePinDesc,
    0, 0
};
static PKSPIN_DESCRIPTOR CapturePins[] = { &CapturePin };

static PCFILTER_DESCRIPTOR WaveFilterDescriptor = {
    0,                               // Version
    &CapturePin,                     // Pins
    1,                               // PinCount
    NULL,                            // Nodes
    0,                               // NodeCount
    NULL,                            // Connections
    0,                               // ConnectionCount
    NULL                             // AutomationTable
};

CMiniportWaveCyclic::CMiniportWaveCyclic(PUNKNOWN outer) {
    m_Port = NULL;
    m_Stream = NULL;
}

CMiniportWaveCyclic::~CMiniportWaveCyclic() {}

STDMETHODIMP CMiniportWaveCyclic::Init(IN PUNKNOWN UnknownAdapter,
                                       IN PRESOURCELIST ResourceList,
                                       IN PPORTWAVECYCLIC Port) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    PAGED_CODE();
    m_Port = Port;
    Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclic::NewStream(OUT PMINIPORTWAVECYCLICSTREAM* Stream,
                                            IN PUNKNOWN OuterUnknown,
                                            IN POOL_TYPE PoolType,
                                            IN ULONG Pin,
                                            IN BOOLEAN Capture,
                                            IN PKSDATAFORMAT DataFormat,
                                            OUT PDMACHANNEL* DmaChannel,
                                            OUT PSERVICEGROUP* ServiceGroup) {
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(Capture);
    UNREFERENCED_PARAMETER(DataFormat);
    PAGED_CODE();
    if (!Stream || !DmaChannel || !ServiceGroup) return STATUS_INVALID_PARAMETER;

    CMiniportWaveCyclicStream* s =
        new(PoolType, ISIGHTMIC_POOL_TAG) CMiniportWaveCyclicStream(OuterUnknown);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS st = s->Init(this, NULL, Pin, TRUE, DataFormat);
    if (!NT_SUCCESS(st)) { s->Release(); return st; }

    *Stream = s;
    s->AddRef();
    m_Stream = s;
    *DmaChannel = NULL;     // no real DMA
    *ServiceGroup = NULL;
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclic::GetDescription(OUT PPCFILTER_DESCRIPTOR Description) {
    if (!Description) return STATUS_INVALID_PARAMETER;
    *Description = &WaveFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportWaveCyclic::DataRangeIntersection(IN ULONG PinId,
                                                       IN PKSDATARANGE DataRange,
                                                       IN PKSDATARANGE MatchingDataRange,
                                                       IN ULONG OutputBufferLength,
                                                       OUT PVOID ResultantFormat,
                                                       OUT PULONG ResultantFormatLength) {
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(PinId);
    // We only support one format; if the caller gave us room, hand it back.
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        if (ResultantFormatLength) *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        return STATUS_BUFFER_TOO_SMALL;
    }
    PKSDATAFORMAT_WAVEFORMATEX fmt = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
    RtlZeroMemory(fmt, sizeof(KSDATAFORMAT_WAVEFORMATEX));
    fmt->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    fmt->DataFormat.Flags = 0;
    fmt->DataFormat.SampleSize = ISIGHTMIC_FRAME_BYTES;
    fmt->DataFormat.Reserved = 0;
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    fmt->WaveFormatEx.nChannels = ISIGHTMIC_CHANNELS;
    fmt->WaveFormatEx.nSamplesPerSec = ISIGHTMIC_SAMPLERATE;
    fmt->WaveFormatEx.nBlockAlign = ISIGHTMIC_FRAME_BYTES;
    fmt->WaveFormatEx.wBitsPerSample = ISIGHTMIC_BITS;
    fmt->WaveFormatEx.cbSize = 0;
    fmt->WaveFormatEx.nAvgBytesPerSec = ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES;
    if (ResultantFormatLength) *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Topology miniport (one microphone bridge node + one capture pin)
// ---------------------------------------------------------------------------
class CMiniportTopology : public IMiniportTopology {
    IMP_IUNKNOWN(IMiniportTopology)
public:
    CMiniportTopology(PUNKNOWN outer);
    ~CMiniportTopology();
    STDMETHODIMP Init(IN PUNKNOWN UnknownAdapter,
                      IN PRESOURCELIST ResourceList,
                      IN PPORTTOPOLOGY Port);
    STDMETHODIMP GetDescription(OUT PPCFILTER_DESCRIPTOR Description);
    STDMETHODIMP DataRangeIntersection(IN ULONG PinId,
                                       IN PKSDATARANGE DataRange,
                                       IN PKSDATARANGE MatchingDataRange,
                                       IN ULONG OutputBufferLength,
                                       OUT PVOID ResultantFormat,
                                       OUT PULONG ResultantFormatLength);
    PPORTTOPOLOGY m_Port;
};

#define KSNODETYPE_ISIGHT_MIC  KSNODETYPE_MICROPHONE

static KSNODE_DESCRIPTOR TopologyNodes[] = {
    {
        NULL,                              // AutomationTable
        &KSNODETYPE_MICROPHONE,           // Type
        NULL                               // Name
    }
};

// connection: filter capture pin (0) <- microphone node output (1)
static KSTOPOLOGY_CONNECTION TopologyConnections[] = {
    { KSFILTER_NODE, 0, 0, 1 }
};

static KSPIN_DESCRIPTOR_EX TopologyPinDescEx = {
    KSPIN_COMMUNICATION_SINK,
    KSPIN_DIRECTION_IN,
    KSPIN_FLAG_HOLDING,
    NULL, NULL,
    { STATIC_KSCATEGORY_CAPTURE, 0, 0, 0 },
    0, 0, 0
};
static KSPIN_DESCRIPTOR TopologyPin = { NULL, &TopologyPinDescEx, 0, 0 };
static PKSPIN_DESCRIPTOR TopologyPins[] = { &TopologyPin };

static PCFILTER_DESCRIPTOR TopologyFilterDescriptor = {
    0,
    TopologyPins,
    1,
    TopologyNodes,
    1,
    TopologyConnections,
    1,
    NULL
};

CMiniportTopology::CMiniportTopology(PUNKNOWN outer) { m_Port = NULL; UNREFERENCED_PARAMETER(outer); }
CMiniportTopology::~CMiniportTopology() {}

STDMETHODIMP CMiniportTopology::Init(IN PUNKNOWN UnknownAdapter,
                                     IN PRESOURCELIST ResourceList,
                                     IN PPORTTOPOLOGY Port) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    PAGED_CODE();
    m_Port = Port;
    Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportTopology::GetDescription(OUT PPCFILTER_DESCRIPTOR Description) {
    if (!Description) return STATUS_INVALID_PARAMETER;
    *Description = &TopologyFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP CMiniportTopology::DataRangeIntersection(IN ULONG PinId,
                                                      IN PKSDATARANGE DataRange,
                                                      IN PKSDATARANGE MatchingDataRange,
                                                      IN ULONG OutputBufferLength,
                                                      OUT PVOID ResultantFormat,
                                                      OUT PULONG ResultantFormatLength) {
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    if (ResultantFormatLength) *ResultantFormatLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
// Miniport factories
// ---------------------------------------------------------------------------
NTSTATUS CreateMiniportWave(IN PUNKNOWN* Unknown,
                            IN PUNKNOWN UnknownOuter,
                            IN POOL_TYPE PoolType,
                            IN PUNKNOWN UnknownAdapter,
                            IN PIRP Irp) {
    PAGED_CODE();
    UNREFERENCED_PARAMETER(UnknownOuter);
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(Irp);
    CMiniportWaveCyclic* p = new(PoolType, ISIGHTMIC_POOL_TAG) CMiniportWaveCyclic(NULL);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = (PUNKNOWN)(IMiniportWaveCyclic*)p;
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

NTSTATUS CreateMiniportTopology(IN PUNKNOWN* Unknown,
                                IN PUNKNOWN UnknownOuter,
                                IN POOL_TYPE PoolType,
                                IN PUNKNOWN UnknownAdapter,
                                IN PIRP Irp) {
    PAGED_CODE();
    UNREFERENCED_PARAMETER(UnknownOuter);
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(Irp);
    CMiniportTopology* p = new(PoolType, ISIGHTMIC_POOL_TAG) CMiniportTopology(NULL);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = (PUNKNOWN)(IMiniportTopology*)p;
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Adapter: register Wave + Topology subdevices
// ---------------------------------------------------------------------------
NTSTATUS InstallDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Irp);
    NTSTATUS status;
    PUNKNOWN pUnknownWave = NULL;
    PUNKNOWN pUnknownTopo = NULL;

    status = CreateMiniportWave(&pUnknownWave, NULL, NonPagedPool, NULL, Irp);
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"Wave", pUnknownWave);
    }
    if (NT_SUCCESS(status)) {
        status = CreateMiniportTopology(&pUnknownTopo, NULL, NonPagedPool, NULL, Irp);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"Topology", pUnknownTopo);
    }
    if (pUnknownWave) pUnknownWave->Release();
    if (pUnknownTopo) pUnknownTopo->Release();
    return status;
}

NTSTATUS AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    PAGED_CODE();
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
                              InstallDevice, 1, 0);
}

// ---------------------------------------------------------------------------
// Control device for the IOCTL (separate from the PortCls adapter device)
// ---------------------------------------------------------------------------
PDEVICE_OBJECT g_CtlDevice = NULL;

NTSTATUS CtlDispatch(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;

    switch (irpSp->MajorFunction) {
    case IRP_MJ_DEVICE_CONTROL: {
        ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
        ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
        PVOID buf = Irp->AssociatedIrp.SystemBuffer;
        if (code == IOCTL_ISIGHTMIC_PUSH) {
            if (!buf || inLen == 0 || (inLen % ISIGHTMIC_FRAME_BYTES) != 0) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                RingPush(&g_Ring, (PUCHAR)buf, inLen);
                info = inLen;
            }
        } else if (code == IOCTL_ISIGHTMIC_GETLEVEL) {
            if (buf && irpSp->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG)) {
                *(PULONG)buf = g_Ring.Count;
                info = sizeof(ULONG);
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
        } else {
            status = STATUS_INVALID_DEVICE_REQUEST;
        }
        break;
    }
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

VOID CtlUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_CtlDevice) {
        UNICODE_STRING dos;
        RtlInitUnicodeString(&dos, ISIGHTMIC_CTL_DOS_NAME);
        IoDeleteSymbolicLink(&dos);
        IoDeleteDevice(g_CtlDevice);
        g_CtlDevice = NULL;
    }
    if (g_Ring.Buffer) {
        ExFreePoolWithTag(g_Ring.Buffer, ISIGHTMIC_POOL_TAG);
        g_Ring.Buffer = NULL;
    }
}

extern "C" NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject,
                                IN PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);
    NTSTATUS status;

    RingInit(&g_Ring);
    if (g_Ring.Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    // PortCls adapter device (the audio endpoint)
    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(g_Ring.Buffer, ISIGHTMIC_POOL_TAG);
        g_Ring.Buffer = NULL;
        return status;
    }

    // Control device for the feeder IOCTL
    UNICODE_STRING devName, dosName;
    RtlInitUnicodeString(&devName, ISIGHTMIC_CTL_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, ISIGHTMIC_CTL_DOS_NAME);
    status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &g_CtlDevice);
    if (!NT_SUCCESS(status)) {
        return status;   // adapter still loaded; IOCTL just won't work
    }
    status = IoCreateSymbolicLink(&dosName, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_CtlDevice);
        g_CtlDevice = NULL;
        return status;
    }
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CtlDispatch;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = CtlDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CtlDispatch;
    DriverObject->DriverUnload = CtlUnload;

    return STATUS_SUCCESS;
}
