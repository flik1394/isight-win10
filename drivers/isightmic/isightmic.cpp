// isightmic.cpp
//
// A kernel-mode PortCls WaveCyclic capture miniport that shows up in Windows as
// a real recording device, "iSight Microphone (FireWire)".  It does NOT talk to
// the camera.  A user-mode feeder (the DirectShow filter, or the isight-micfeed
// test tool) pushes 48 kHz / 16-bit / mono PCM in through an IOCTL; this driver
// streams it back out to the audio engine from a cyclic DMA buffer.
//
// Kernel side is deliberately tiny: one ring buffer, one 10 ms timer, one
// PortCls miniport.  All DSP lives in user mode.
//
// Build (see make.sys.bat / the vmic job in .github/workflows/build.yml):
//   cl /nologo /kernel /c /W3 /O2 /I<WDK km include> /I<WDK km\crt> /I<shared> isightmic.cpp
//   link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64
//        /LIBPATH:<WDK km x64 lib> portcls.lib ks.lib drmk.lib ntoskrnl.lib
//        hal.lib wmilib.lib wdmsec.lib /OUT:isightmic.sys isightmic.obj

// INITGUID must come before any header that uses DEFINE_GUID.  PortCls
// declares its interface IIDs (IID_IMiniportWaveCyclic, IID_IMiniport,
// IID_IServiceSink, ...) with DEFINE_GUID, which without INITGUID is a bare
// `extern const GUID` and links nowhere -- there is no library that defines
// them for a miniport that does not import them through a class factory.
// With INITGUID they are emitted here as DECLSPEC_SELECTANY definitions, so
// QueryInterface has something to compare against.
#define INITGUID

#include <ntddk.h>
#include <wdmsec.h>
#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>

#include "isightmic.h"

// ---------------------------------------------------------------------------
// Kernel new/delete (no exceptions in kernel mode: these return / accept NULL)
// ---------------------------------------------------------------------------
void* __cdecl operator new(size_t size, POOL_TYPE poolType, ULONG tag) {
    return ExAllocatePoolWithTag(poolType, size, tag);
}
void __cdecl operator delete(void* p) {
    if (p) ExFreePoolWithTag(p, ISIGHTMIC_POOL_TAG);
}
void __cdecl operator delete(void* p, size_t) {
    if (p) ExFreePoolWithTag(p, ISIGHTMIC_POOL_TAG);
}

// ---------------------------------------------------------------------------
// Ring buffer: written by the IOCTL handler, drained by the capture stream
// ---------------------------------------------------------------------------
#define RING_SECONDS   2
#define RING_BYTES     (ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES * RING_SECONDS)

typedef struct _RING {
    PUCHAR       Buffer;
    ULONG        Cap;
    ULONG        Head;
    ULONG        Tail;
    ULONG        Count;
    KSPIN_LOCK   Lock;
} RING, *PRING;

static RING  g_Ring;
static ULONG g_Pushed;
static ULONG g_Played;
static ULONG g_Starved;
static ULONG g_Streams;
static ULONG g_State;
static ULONG g_Opens;

static void RingInit(PRING r) {
    r->Buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, RING_BYTES, ISIGHTMIC_POOL_TAG);
    r->Cap = (r->Buffer != NULL) ? RING_BYTES : 0;
    r->Head = r->Tail = r->Count = 0;
    KeInitializeSpinLock(&r->Lock);
}

// Drop the oldest audio rather than block the feeder: this is a microphone,
// and a late sample is worth less than the next one.
// (src is a *mutable* pointer: we skip forward over the data we have to drop.)
static void RingPush(PRING r, PUCHAR src, ULONG n) {
    if (r->Buffer == NULL || n == 0) return;
    KIRQL irql;
    KeAcquireSpinLock(&r->Lock, &irql);
    if (n > r->Cap) { src += (n - r->Cap); n = r->Cap; }
    if (r->Count + n > r->Cap) {
        ULONG drop = (r->Count + n) - r->Cap;
        r->Head = (r->Head + drop) % r->Cap;
        r->Count -= drop;
    }
    ULONG first = r->Cap - r->Tail;
    if (first > n) first = n;
    RtlCopyMemory(r->Buffer + r->Tail, src, first);
    if (n > first) RtlCopyMemory(r->Buffer, src + first, n - first);
    r->Tail = (r->Tail + n) % r->Cap;
    r->Count += n;
    KeReleaseSpinLock(&r->Lock, irql);
}

static ULONG RingPull(PRING r, PUCHAR dst, ULONG n) {
    if (r->Buffer == NULL || n == 0) return 0;
    KIRQL irql;
    KeAcquireSpinLock(&r->Lock, &irql);
    if (n > r->Count) n = r->Count;
    ULONG first = r->Cap - r->Head;
    if (first > n) first = n;
    RtlCopyMemory(dst, r->Buffer + r->Head, first);
    if (n > first) RtlCopyMemory(dst + first, r->Buffer, n - first);
    r->Head = (r->Head + n) % r->Cap;
    r->Count -= n;
    KeReleaseSpinLock(&r->Lock, irql);
    return n;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class CMiniportWaveCyclic;
class CMiniportWaveCyclicStream;

VOID StreamTimerDpc(IN PKDPC Dpc, IN PVOID DeferredContext,
                    IN PVOID SystemArgument1, IN PVOID SystemArgument2);

#define WAVE_BUFFER_BYTES   9600    // 100 ms of 48 kHz mono 16-bit
#define TIMER_PERIOD_MS     10

// ---------------------------------------------------------------------------
// Capture stream
//
// WaveCyclic: the miniport owns one cyclic DMA buffer.  We are the "hardware",
// so we fill it ourselves on a timer and hand the port driver the position; the
// port driver copies from the DMA buffer into the KS stream.
// ---------------------------------------------------------------------------
class CMiniportWaveCyclicStream : public IMiniportWaveCyclicStream, public IServiceSink {
public:
    CMiniportWaveCyclicStream(PUNKNOWN outer);
    ~CMiniportWaveCyclicStream();

    // IUnknown
    STDMETHODIMP          QueryInterface(REFIID iid, PVOID* ppv);
    STDMETHODIMP_(ULONG)  AddRef();
    STDMETHODIMP_(ULONG)  Release();

    // IServiceSink - PortCls calls this when the port is notified
    STDMETHODIMP_(void)   RequestService();

    // IMiniportWaveCyclicStream
    STDMETHODIMP_(NTSTATUS) GetPosition(OUT PULONG Position);
    STDMETHODIMP_(NTSTATUS) NormalizePhysicalPosition(IN OUT PLONGLONG PhysicalPosition);
    STDMETHODIMP_(NTSTATUS) SetFormat(IN PKSDATAFORMAT DataFormat);
    // Note: returns ULONG (the previous interval), not NTSTATUS.
    STDMETHODIMP_(ULONG)    SetNotificationFreq(IN ULONG Interval, OUT PULONG FrameSize);
    STDMETHODIMP_(NTSTATUS) SetState(IN KSSTATE State);
    STDMETHODIMP_(void)     Silence(IN PVOID Buffer, IN ULONG ByteCount);

    // Private helper.  No port-side stream interface is handed to a WaveCyclic
    // miniport here (the port reaches the stream through the service group), so
    // this only records what the caller told us.
    NTSTATUS Init(IN ULONG Pin,
                  IN BOOLEAN Capture,
                  IN PKSDATAFORMAT DataFormat);

    void Service();

    PPORTWAVECYCLIC        m_Port;
    PSERVICEGROUP          m_ServiceGroup;
    PDMACHANNEL            m_DmaChannel;
    PVOID                  m_Buffer;
    ULONG                  m_BufferSize;
    ULONG                  m_Position;
    ULONG                  m_NotificationInterval;
    KSSTATE                m_State;
    KTIMER                 m_Timer;
    KDPC                   m_Dpc;
    BOOLEAN                m_TimerOn;

protected:
    LONG m_RefCount;
};

CMiniportWaveCyclicStream::CMiniportWaveCyclicStream(PUNKNOWN outer) {
    UNREFERENCED_PARAMETER(outer);
    m_Port = NULL;
    m_ServiceGroup = NULL;
    m_DmaChannel = NULL;
    m_Buffer = NULL;
    m_BufferSize = 0;
    m_Position = 0;
    m_NotificationInterval = 0;
    m_State = KSSTATE_STOP;
    m_TimerOn = FALSE;
    m_RefCount = 1;
    KeInitializeTimer(&m_Timer);
    KeInitializeDpc(&m_Dpc, (PKDEFERRED_ROUTINE)StreamTimerDpc, (PVOID)this);
}

CMiniportWaveCyclicStream::~CMiniportWaveCyclicStream() {
    if (m_TimerOn) { KeCancelTimer(&m_Timer); m_TimerOn = FALSE; }
    if (m_DmaChannel) {
        m_DmaChannel->FreeBuffer();
        m_DmaChannel->Release();
        m_DmaChannel = NULL;
    }
    if (m_ServiceGroup) { m_ServiceGroup->Release(); m_ServiceGroup = NULL; }
}

STDMETHODIMP CMiniportWaveCyclicStream::QueryInterface(REFIID iid, PVOID* ppv) {
    if (!ppv) return STATUS_INVALID_PARAMETER;
    *ppv = NULL;
    if (IsEqualGUIDAligned(iid, IID_IUnknown))
        *ppv = (PVOID)(IUnknown*)(IMiniportWaveCyclicStream*)this;
    else if (IsEqualGUIDAligned(iid, IID_IServiceSink))
        *ppv = (PVOID)(IServiceSink*)this;
    else if (IsEqualGUIDAligned(iid, IID_IMiniportWaveCyclicStream))
        *ppv = (PVOID)(IMiniportWaveCyclicStream*)this;
    if (*ppv) { AddRef(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::AddRef() {
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::Release() {
    ULONG c = (ULONG)InterlockedDecrement(&m_RefCount);
    if (c == 0) delete this;
    return c;
}

// The port driver asks for a position inside the cyclic buffer.  We advance it
// by one quantum per service call, which is exactly what we wrote.
STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStream::GetPosition(OUT PULONG Position) {
    if (!Position) return STATUS_INVALID_PARAMETER;
    *Position = m_Position;
    return STATUS_SUCCESS;
}

// Our "hardware position" is already byte-based and linear, so a conversion to
// 100 ns units is just a scale.
STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStream::NormalizePhysicalPosition(IN OUT PLONGLONG PhysicalPosition) {
    if (!PhysicalPosition) return STATUS_INVALID_PARAMETER;
    // bytes -> 100 ns:  * 10000000 / (sampleRate * frameBytes)
    *PhysicalPosition = (*PhysicalPosition * 10000000LL) /
                        (ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStream::SetFormat(IN PKSDATAFORMAT DataFormat) {
    UNREFERENCED_PARAMETER(DataFormat);
    if (!m_DmaChannel) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS st = m_DmaChannel->AllocateBuffer(WAVE_BUFFER_BYTES, NULL);
    if (!NT_SUCCESS(st)) return st;
    m_Buffer = m_DmaChannel->SystemAddress();
    if (!m_Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    m_BufferSize = WAVE_BUFFER_BYTES;
    if (m_BufferSize) RtlZeroMemory(m_Buffer, m_BufferSize);
    return STATUS_SUCCESS;
}

// Returns the *previous* notification interval -- that is what this method is
// specified to report, not a status code.
STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::SetNotificationFreq(IN ULONG Interval,
                                                                   OUT PULONG FrameSize) {
    ULONG previous = m_NotificationInterval;
    m_NotificationInterval = Interval;
    // We service on a fixed 10 ms timer; report the matching frame count.
    if (FrameSize) *FrameSize = ISIGHTMIC_SAMPLERATE / 100;
    return previous;
}

STDMETHODIMP_(void) CMiniportWaveCyclicStream::Silence(IN PVOID Buffer, IN ULONG ByteCount) {
    if (Buffer && ByteCount) RtlZeroMemory(Buffer, ByteCount);
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStream::SetState(IN KSSTATE State) {
    m_State = State;
    g_State = (ULONG)State;
    if (State == KSSTATE_RUN) {
        if (!m_TimerOn && m_Buffer && m_BufferSize) {
            LARGE_INTEGER due;
            due.QuadPart = -10000LL * TIMER_PERIOD_MS;   // relative
            KeSetTimerEx(&m_Timer, due, TIMER_PERIOD_MS, &m_Dpc);
            m_TimerOn = TRUE;
        }
    } else {
        if (m_TimerOn) { KeCancelTimer(&m_Timer); m_TimerOn = FALSE; }
    }
    return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveCyclicStream::Init(IN ULONG Pin,
                                         IN BOOLEAN Capture,
                                         IN PKSDATAFORMAT DataFormat) {
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(Capture);
    UNREFERENCED_PARAMETER(DataFormat);
    // The cyclic buffer is sized in SetFormat, once the port has picked the
    // format it actually wants to run.
    return STATUS_SUCCESS;
}

// One 10 ms quantum: copy from the ring into the cyclic DMA buffer, wrapping at
// the end.  Whatever the ring cannot supply becomes silence, so the endpoint
// always runs at real time even when the camera is not streaming.
void CMiniportWaveCyclicStream::Service() {
    if (m_State != KSSTATE_RUN || !m_Buffer || !m_BufferSize) return;

    const ULONG quantum = ISIGHTMIC_QUANTUM_BYTES;
    ULONG pos = m_Position % m_BufferSize;
    PUCHAR base = (PUCHAR)m_Buffer;

    ULONG first = m_BufferSize - pos;
    if (first > quantum) first = quantum;
    ULONG got = RingPull(&g_Ring, base + pos, first);
    if (got < first) { RtlZeroMemory(base + pos + got, first - got); g_Starved += first - got; }

    if (quantum > first) {
        ULONG second = quantum - first;
        ULONG got2 = RingPull(&g_Ring, base, second);
        if (got2 < second) { RtlZeroMemory(base + got2, second - got2); g_Starved += second - got2; }
    }

    m_Position = (pos + quantum) % m_BufferSize;
    g_Played += quantum;
}

STDMETHODIMP_(void) CMiniportWaveCyclicStream::RequestService() {
    Service();
}

// Stand-in for a hardware interrupt: wake the port driver, which will then
// service the stream and pull our bytes out of the DMA buffer.
VOID StreamTimerDpc(IN PKDPC Dpc, IN PVOID DeferredContext,
                    IN PVOID SystemArgument1, IN PVOID SystemArgument2) {
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    CMiniportWaveCyclicStream* s = (CMiniportWaveCyclicStream*)DeferredContext;
    if (s == NULL) return;
    if (s->m_Port != NULL && s->m_ServiceGroup != NULL)
        s->m_Port->Notify(s->m_ServiceGroup);
}

// ---------------------------------------------------------------------------
// Wave miniport
// ---------------------------------------------------------------------------
class CMiniportWaveCyclic : public IMiniportWaveCyclic {
public:
    CMiniportWaveCyclic(PUNKNOWN outer);
    ~CMiniportWaveCyclic();

    STDMETHODIMP QueryInterface(REFIID iid, PVOID* ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IMiniport
    // IMiniport -- note the extra '*': IMiniport::GetDescription takes a
    // PPCFILTER_DESCRIPTOR * (a pointer to the caller's out-pointer).
    STDMETHODIMP_(NTSTATUS) GetDescription(OUT PPCFILTER_DESCRIPTOR *Description);
    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(IN ULONG PinId,
                                                  IN PKSDATARANGE DataRange,
                                                  IN PKSDATARANGE MatchingDataRange,
                                                  IN ULONG OutputBufferLength,
                                                  OUT PVOID ResultantFormat,
                                                  OUT PULONG ResultantFormatLength);

    // IMiniportWaveCyclic
    STDMETHODIMP_(NTSTATUS) Init(IN PUNKNOWN UnknownAdapter,
                                 IN PRESOURCELIST ResourceList,
                                 IN PPORTWAVECYCLIC Port);
    STDMETHODIMP_(NTSTATUS) NewStream(OUT PMINIPORTWAVECYCLICSTREAM* Stream,
                                      IN PUNKNOWN OuterUnknown,
                                      IN POOL_TYPE PoolType,
                                      IN ULONG Pin,
                                      IN BOOLEAN Capture,
                                      IN PKSDATAFORMAT DataFormat,
                                      OUT PDMACHANNEL* DmaChannel,
                                      OUT PSERVICEGROUP* ServiceGroup);

    PPORTWAVECYCLIC  m_Port;

protected:
    LONG m_RefCount;
};

// 48 kHz / 16-bit / mono capture data range.
// KSDATARANGE_AUDIO = KSDATARANGE (FormatSize, Flags, SampleSize, Reserved,
// MajorFormat, SubFormat, Specifier) followed by exactly five ULONGs:
// MaximumChannels, MinimumBitsPerSample, MaximumBitsPerSample,
// MinimumSampleFrequency, MaximumSampleFrequency.  (There is no
// MinimumChannels member -- adding one shifts every following field.)
static KSDATARANGE_AUDIO PinDataRangesStream[] = {
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            0,                               // Flags
            0,                               // SampleSize (informational)
            0,                               // Reserved
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        ISIGHTMIC_CHANNELS,                 // MaximumChannels
        ISIGHTMIC_BITS,                     // MinimumBitsPerSample
        ISIGHTMIC_BITS,                     // MaximumBitsPerSample
        ISIGHTMIC_SAMPLERATE,               // MinimumSampleFrequency
        ISIGHTMIC_SAMPLERATE                // MaximumSampleFrequency
    }
};
static PKSDATARANGE PinDataRangePointersStream[] = {
    (PKSDATARANGE)&PinDataRangesStream[0]
};

// Bridge data ranges carry analog audio: no format, just a connection.  Both
// filters need one: the wave filter's bridge pin pairs with the topology's, and
// wdmaudio walks that pair to find out which wave pin belongs to which jack.
// (Reference layout: microsoft/Windows-driver-samples, audio/simpleaudiosample,
// Filters/*wavtable.h + *toptable.h -- a capture device is exactly a topology
// with a physical input pin plus a wave filter whose streaming pin is the
// source.  See the note on KSPIN_WAVE_BRIDGE below.)
static KSDATARANGE PinDataRangesBridge[] = {
    {
        sizeof(KSDATARANGE),
        0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};
static PKSDATARANGE PinDataRangePointersBridge[] = {
    &PinDataRangesBridge[0]
};

#define KSPIN_WAVE_BRIDGE       0
#define KSPIN_WAVE_HOST         1
#define KSNODE_WAVE_ADC         0

// A capture pin is a sink for IRPs and a source of data.
static PCPIN_DESCRIPTOR WavePins[] = {
    {   // 0 - KSPIN_WAVE_BRIDGE: the connection to the topology filter.
        // A bridge pin is a filter pin, not a stream: nothing ever opens a
        // stream on it (KSPIN_COMMUNICATION_NONE), so the instance counts are
        // zero, exactly as in the reference tables.  Without this pin the wave
        // filter has no counterpart for the topology's bridge and wdmaudio
        // cannot tell which jack this wave pin belongs to.
        0, 0, 0,
        NULL,               // AutomationTable
        {
            0, NULL,        // Interfaces
            0, NULL,        // Mediums
            1, (const PKSDATARANGE*)PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            { 0 }
        }
    },
    {   // 1 - KSPIN_WAVE_HOST: the streaming pin the audio engine opens.
        1, 1, 0,            // instance counts (global, filter, min)
        NULL,               // AutomationTable
        {
            0, NULL,        // Interfaces
            0, NULL,        // Mediums
            1, (const PKSDATARANGE*)PinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            { 0 }
        }
    }
};

// The ADC between the bridge and the streaming pin -- the reference capture
// filter has one, and it is the node wdmaudio expects to see on the capture
// path.  No automation table: this device has no volume/mute control.
static PCNODE_DESCRIPTOR WaveNodes[] = {
    {
        0,                      // Flags
        NULL,                   // AutomationTable
        &KSNODETYPE_ADC,        // Type
        NULL                    // Name
    }
};

// Node pins follow the KS convention: pin 0 is the output, pin 1 the input.
static PCCONNECTION_DESCRIPTOR WaveConnections[] = {
    { KSFILTER_NODE, KSPIN_WAVE_BRIDGE, KSNODE_WAVE_ADC, 1 },
    { KSNODE_WAVE_ADC, 0,               KSFILTER_NODE,   KSPIN_WAVE_HOST }
};

// PCFILTER_DESCRIPTOR is exactly 12 fields -- Version, AutomationTable,
// PinSize, PinCount, Pins, NodeSize, NodeCount, Nodes, ConnectionCount,
// Connections, CategoryCount, Categories.  There is no AutomationTableSize
// member; inserting one silently reinterprets the whole descriptor.
static PCFILTER_DESCRIPTOR WaveFilterDescriptor = {
    0,                                  // Version
    NULL,                               // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),           // PinSize
    2,                                  // PinCount (bridge + streaming)
    WavePins,                           // Pins
    sizeof(PCNODE_DESCRIPTOR),          // NodeSize
    1,                                  // NodeCount
    WaveNodes,                          // Nodes
    2,                                  // ConnectionCount
    WaveConnections,                    // Connections
    0,                                  // CategoryCount
    NULL                                // Categories
};

CMiniportWaveCyclic::CMiniportWaveCyclic(PUNKNOWN outer) {
    UNREFERENCED_PARAMETER(outer);
    m_Port = NULL;
    m_RefCount = 1;
}

CMiniportWaveCyclic::~CMiniportWaveCyclic() {
    if (m_Port) { m_Port->Release(); m_Port = NULL; }
}

STDMETHODIMP CMiniportWaveCyclic::QueryInterface(REFIID iid, PVOID* ppv) {
    if (!ppv) return STATUS_INVALID_PARAMETER;
    *ppv = NULL;
    if (IsEqualGUIDAligned(iid, IID_IUnknown))
        *ppv = (PVOID)(IUnknown*)(IMiniportWaveCyclic*)this;
    else if (IsEqualGUIDAligned(iid, IID_IMiniport))
        *ppv = (PVOID)(IMiniport*)(IMiniportWaveCyclic*)this;
    else if (IsEqualGUIDAligned(iid, IID_IMiniportWaveCyclic))
        *ppv = (PVOID)(IMiniportWaveCyclic*)this;
    if (*ppv) { AddRef(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclic::AddRef() {
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclic::Release() {
    ULONG c = (ULONG)InterlockedDecrement(&m_RefCount);
    if (c == 0) delete this;
    return c;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclic::Init(IN PUNKNOWN UnknownAdapter,
                                                  IN PRESOURCELIST ResourceList,
                                                  IN PPORTWAVECYCLIC Port) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    m_Port = Port;
    if (m_Port) m_Port->AddRef();
    // Virtual device: there is no real bus resource to claim.
    UNREFERENCED_PARAMETER(ResourceList);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclic::NewStream(OUT PMINIPORTWAVECYCLICSTREAM* Stream,
                                                       IN PUNKNOWN OuterUnknown,
                                                       IN POOL_TYPE PoolType,
                                                       IN ULONG Pin,
                                                       IN BOOLEAN Capture,
                                                       IN PKSDATAFORMAT DataFormat,
                                                       OUT PDMACHANNEL* DmaChannel,
                                                       OUT PSERVICEGROUP* ServiceGroup) {
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(Capture);
    if (!Stream || !DmaChannel || !ServiceGroup) return STATUS_INVALID_PARAMETER;
    *Stream = NULL; *DmaChannel = NULL; *ServiceGroup = NULL;

    CMiniportWaveCyclicStream* s =
        new(PoolType, ISIGHTMIC_POOL_TAG) CMiniportWaveCyclicStream(OuterUnknown);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS st = STATUS_SUCCESS;

    // The port driver needs a DMA channel object; ours is a software device, so
    // a master channel backed by ordinary nonpaged memory is all it takes.
    // IPortWaveCyclic::NewMasterDmaChannel takes exactly eight arguments --
    // (OutDmaChannel, OuterUnknown, ResourceList, MaximumLength,
    //  Dma32BitAddresses, Dma64BitAddresses, DmaWidth, DmaSpeed).  It does not
    // take a DEVICE_DESCRIPTION*.
    if (m_Port) {
        st = m_Port->NewMasterDmaChannel(&s->m_DmaChannel, NULL, NULL,
                                         WAVE_BUFFER_BYTES, TRUE, FALSE,
                                         (DMA_WIDTH)(-1), (DMA_SPEED)(-1));
    }
    if (!NT_SUCCESS(st)) { s->Release(); return st; }

    st = s->Init(Pin, TRUE, DataFormat);
    if (!NT_SUCCESS(st)) { s->Release(); return st; }

    // Service group: PortCls calls the stream back through it when we notify.
    st = PcNewServiceGroup(&s->m_ServiceGroup, NULL);
    if (!NT_SUCCESS(st)) { s->Release(); return st; }
    s->m_ServiceGroup->AddMember(PSERVICESINK(s));

    s->m_Port = m_Port;
    if (s->m_Port) s->m_Port->AddRef();

    *Stream = (PMINIPORTWAVECYCLICSTREAM)s;
    (*Stream)->AddRef();
    *DmaChannel = s->m_DmaChannel;
    if (*DmaChannel) (*DmaChannel)->AddRef();
    *ServiceGroup = s->m_ServiceGroup;
    if (*ServiceGroup) (*ServiceGroup)->AddRef();

    g_Streams++;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclic::GetDescription(OUT PPCFILTER_DESCRIPTOR *Description) {
    if (!Description) return STATUS_INVALID_PARAMETER;
    *Description = &WaveFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclic::DataRangeIntersection(IN ULONG PinId,
                                                                   IN PKSDATARANGE DataRange,
                                                                   IN PKSDATARANGE MatchingDataRange,
                                                                   IN ULONG OutputBufferLength,
                                                                   OUT PVOID ResultantFormat,
                                                                   OUT PULONG ResultantFormatLength) {
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        if (ResultantFormatLength) *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        return STATUS_BUFFER_TOO_SMALL;
    }
    PKSDATAFORMAT_WAVEFORMATEX fmt = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
    if (!fmt) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(fmt, sizeof(KSDATAFORMAT_WAVEFORMATEX));
    fmt->DataFormat.FormatSize  = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    fmt->DataFormat.SampleSize  = ISIGHTMIC_FRAME_BYTES;
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
    fmt->WaveFormatEx.nChannels       = ISIGHTMIC_CHANNELS;
    fmt->WaveFormatEx.nSamplesPerSec  = ISIGHTMIC_SAMPLERATE;
    fmt->WaveFormatEx.nBlockAlign     = ISIGHTMIC_FRAME_BYTES;
    fmt->WaveFormatEx.wBitsPerSample  = ISIGHTMIC_BITS;
    fmt->WaveFormatEx.cbSize          = 0;
    fmt->WaveFormatEx.nAvgBytesPerSec = ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES;
    if (ResultantFormatLength) *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Topology miniport: one microphone node between the mic jack and the wave pin
// ---------------------------------------------------------------------------
class CMiniportTopology : public IMiniportTopology {
public:
    CMiniportTopology(PUNKNOWN outer);
    ~CMiniportTopology();

    STDMETHODIMP QueryInterface(REFIID iid, PVOID* ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IMiniport -- note the extra '*': IMiniport::GetDescription takes a
    // PPCFILTER_DESCRIPTOR * (a pointer to the caller's out-pointer).
    STDMETHODIMP_(NTSTATUS) GetDescription(OUT PPCFILTER_DESCRIPTOR *Description);
    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(IN ULONG PinId,
                                                  IN PKSDATARANGE DataRange,
                                                  IN PKSDATARANGE MatchingDataRange,
                                                  IN ULONG OutputBufferLength,
                                                  OUT PVOID ResultantFormat,
                                                  OUT PULONG ResultantFormatLength);
    STDMETHODIMP_(NTSTATUS) Init(IN PUNKNOWN UnknownAdapter,
                                 IN PRESOURCELIST ResourceList,
                                 IN PPORTTOPOLOGY Port);

    PPORTTOPOLOGY m_Port;

protected:
    LONG m_RefCount;
};

// Bridge pins carry analog audio: no format, just a connection.
// (PinDataRangesBridge / PinDataRangePointersBridge are defined once, next to
// the wave pins, and shared by both filters.)
#define KSPIN_TOPO_MIC_JACK     0
#define KSPIN_TOPO_WAVE_BRIDGE  1
#define KSNODE_TOPO_MIC         0

static PCPIN_DESCRIPTOR TopologyPins[] = {
    {   // 0 - the microphone jack (signal enters the filter here)
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            1, (const PKSDATARANGE*)PinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE,
            NULL,
            { 0 }
        }
    },
    {   // 1 - bridge to the wave filter's capture pin.  The reference tables
        // use KSCATEGORY_AUDIO here (the physical jack is the pin above, which
        // is the one that carries the node type).
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            1, (const PKSDATARANGE*)PinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            { 0 }
        }
    }
};

static PCNODE_DESCRIPTOR TopologyNodes[] = {
    {
        0,                          // Flags
        NULL,                       // AutomationTable (no volume/mute node)
        &KSNODETYPE_MICROPHONE,     // Type
        NULL                        // Name
    }
};

static PCCONNECTION_DESCRIPTOR TopologyConnections[] = {
    // Node pin numbering: 0 is the output, 1 the input.
    { KSFILTER_NODE, KSPIN_TOPO_MIC_JACK,    KSNODE_TOPO_MIC, 1 },
    { KSNODE_TOPO_MIC, 0,                   KSFILTER_NODE, KSPIN_TOPO_WAVE_BRIDGE }
};

static PCFILTER_DESCRIPTOR TopologyFilterDescriptor = {
    0,                                  // Version
    NULL,                               // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),           // PinSize
    2,                                  // PinCount
    TopologyPins,                       // Pins
    sizeof(PCNODE_DESCRIPTOR),          // NodeSize
    1,                                  // NodeCount
    TopologyNodes,                      // Nodes
    2,                                  // ConnectionCount
    TopologyConnections,                // Connections
    0,                                  // CategoryCount
    NULL                                // Categories
};

CMiniportTopology::CMiniportTopology(PUNKNOWN outer) {
    UNREFERENCED_PARAMETER(outer);
    m_Port = NULL;
    m_RefCount = 1;
}

CMiniportTopology::~CMiniportTopology() {
    if (m_Port) { m_Port->Release(); m_Port = NULL; }
}

STDMETHODIMP CMiniportTopology::QueryInterface(REFIID iid, PVOID* ppv) {
    if (!ppv) return STATUS_INVALID_PARAMETER;
    *ppv = NULL;
    if (IsEqualGUIDAligned(iid, IID_IUnknown))
        *ppv = (PVOID)(IUnknown*)(IMiniportTopology*)this;
    else if (IsEqualGUIDAligned(iid, IID_IMiniport))
        *ppv = (PVOID)(IMiniport*)(IMiniportTopology*)this;
    else if (IsEqualGUIDAligned(iid, IID_IMiniportTopology))
        *ppv = (PVOID)(IMiniportTopology*)this;
    if (*ppv) { AddRef(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(ULONG) CMiniportTopology::AddRef() {
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CMiniportTopology::Release() {
    ULONG c = (ULONG)InterlockedDecrement(&m_RefCount);
    if (c == 0) delete this;
    return c;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::Init(IN PUNKNOWN UnknownAdapter,
                                                IN PRESOURCELIST ResourceList,
                                                IN PPORTTOPOLOGY Port) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    m_Port = Port;
    if (m_Port) m_Port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::GetDescription(OUT PPCFILTER_DESCRIPTOR *Description) {
    if (!Description) return STATUS_INVALID_PARAMETER;
    *Description = &TopologyFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::DataRangeIntersection(IN ULONG PinId,
                                                                 IN PKSDATARANGE DataRange,
                                                                 IN PKSDATARANGE MatchingDataRange,
                                                                 IN ULONG OutputBufferLength,
                                                                 OUT PVOID ResultantFormat,
                                                                 OUT PULONG ResultantFormatLength) {
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    if (ResultantFormat && OutputBufferLength >= sizeof(KSDATARANGE) && MatchingDataRange)
        RtlCopyMemory(ResultantFormat, MatchingDataRange, sizeof(KSDATARANGE));
    if (ResultantFormatLength) *ResultantFormatLength = sizeof(KSDATARANGE);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Adapter: register the wave + topology subdevices
// ---------------------------------------------------------------------------
static NTSTATUS StartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList) {
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(ResourceList);
    NTSTATUS st;
    PUNKNOWN wave = NULL;
    PUNKNOWN topo = NULL;

    CMiniportWaveCyclic* w = new(NonPagedPool, ISIGHTMIC_POOL_TAG) CMiniportWaveCyclic(NULL);
    if (!w) return STATUS_INSUFFICIENT_RESOURCES;
    wave = (PUNKNOWN)(IMiniportWaveCyclic*)w;

    // The subdevice name must match the KSNAME_* entries in isightmic.inf.
    // (PWSTR) keeps this compiling whether the WDK declares the parameter as
    // PWSTR or PCWSTR.
    st = PcRegisterSubdevice(DeviceObject, (PWSTR)L"Wave", wave);
    if (!NT_SUCCESS(st)) { wave->Release(); return st; }

    CMiniportTopology* t = new(NonPagedPool, ISIGHTMIC_POOL_TAG) CMiniportTopology(NULL);
    if (!t) { wave->Release(); return STATUS_INSUFFICIENT_RESOURCES; }
    topo = (PUNKNOWN)(IMiniportTopology*)t;

    st = PcRegisterSubdevice(DeviceObject, (PWSTR)L"Topology", topo);
    if (!NT_SUCCESS(st)) { topo->Release(); wave->Release(); return st; }

    topo->Release();
    wave->Release();
    return STATUS_SUCCESS;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, StartDevice, 2, 0);
}

// ---------------------------------------------------------------------------
// Control device: the IOCTL feeder talks to this, not to the audio device
//
// PortCls fills in DriverObject->MajorFunction when it initialises the adapter
// driver, and those entries serve the KS filters.  We keep every original
// pointer and forward anything that is not aimed at our control device, so the
// audio side keeps working.
// ---------------------------------------------------------------------------
static PDEVICE_OBJECT   g_CtlDevice = NULL;
static PDRIVER_DISPATCH g_PortClsDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
static PDRIVER_UNLOAD   g_PortClsUnload = NULL;

static NTSTATUS CtlDispatch(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    UCHAR major = irpSp->MajorFunction;

    if (DeviceObject != g_CtlDevice) {
        if (major <= IRP_MJ_MAXIMUM_FUNCTION && g_PortClsDispatch[major] != NULL)
            return g_PortClsDispatch[major](DeviceObject, Irp);
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;

    switch (major) {
    case IRP_MJ_CREATE:
        g_Opens++;
        break;

    case IRP_MJ_CLOSE:
        break;

    case IRP_MJ_DEVICE_CONTROL: {
        ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
        ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
        PVOID buf = Irp->AssociatedIrp.SystemBuffer;
        if (code == IOCTL_ISIGHTMIC_PUSH) {
            if (!buf || inLen == 0 || (inLen % ISIGHTMIC_FRAME_BYTES) != 0) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                RingPush(&g_Ring, (PUCHAR)buf, inLen);
                g_Pushed += inLen;
                info = inLen;
            }
        } else if (code == IOCTL_ISIGHTMIC_GETSTATUS) {
            ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
            if (buf && outLen >= sizeof(ISIGHTMIC_STATUS)) {
                PISIGHTMIC_STATUS st = (PISIGHTMIC_STATUS)buf;
                RtlZeroMemory(st, sizeof(ISIGHTMIC_STATUS));
                st->Buffered = g_Ring.Count;
                st->Pushed = g_Pushed;
                st->Played = g_Played;
                st->Starved = g_Starved;
                st->Streams = g_Streams;
                st->State = g_State;
                st->Opens = g_Opens;
                info = sizeof(ISIGHTMIC_STATUS);
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
        } else {
            status = STATUS_INVALID_DEVICE_REQUEST;
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID CtlUnload(PDRIVER_OBJECT DriverObject) {
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
        g_Ring.Cap = 0;
    }
    if (g_PortClsUnload) g_PortClsUnload(DriverObject);
}

extern "C" NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject,
                                IN PUNICODE_STRING RegistryPath) {
    RingInit(&g_Ring);
    if (g_Ring.Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice);
    if (!NT_SUCCESS(status)) return status;

    // Keep what PortCls installed, then sit in front of it.
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        g_PortClsDispatch[i] = DriverObject->MajorFunction[i];
        if (DriverObject->MajorFunction[i] != NULL)
            DriverObject->MajorFunction[i] = CtlDispatch;
    }
    g_PortClsUnload = DriverObject->DriverUnload;

    // A feeder runs inside WeChat / OBS, i.e. as an ordinary user, so the
    // control device has to be openable without elevation.
    UNICODE_STRING devName, dosName, sddl;
    RtlInitUnicodeString(&devName, ISIGHTMIC_CTL_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, ISIGHTMIC_CTL_DOS_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;WD)(A;;GA;;;BU)");

    status = IoCreateDeviceSecure(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN,
                                  FILE_DEVICE_SECURE_OPEN, FALSE, &sddl, NULL,
                                  &g_CtlDevice);
    if (!NT_SUCCESS(status)) {
        g_CtlDevice = NULL;
        return status;   // audio endpoint still works; the feeder just cannot reach us
    }
    status = IoCreateSymbolicLink(&dosName, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_CtlDevice);
        g_CtlDevice = NULL;
        return status;
    }

    g_CtlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    DriverObject->DriverUnload = CtlUnload;
    return STATUS_SUCCESS;
}
