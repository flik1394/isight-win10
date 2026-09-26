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

// NewStream break-down.  NewStream increments g_Streams on its *last* line, so
// g_Streams == 0 cannot distinguish "PortCls never called us because it never
// instantiated the pin" from "we were called and failed half way through".
// Those two need opposite fixes (pin descriptor vs. DMA/service-group code), so
// count the entry and each failure site separately.  Reported through
// IOCTL_ISIGHTMIC_GETDIAG.
static ULONG g_NewStreamEntered;
static ULONG g_NewStreamFailed;
static ULONG g_FailDma;
static ULONG g_FailStreamInit;
static ULONG g_FailServiceGroup;
static ULONG g_LastFailStatus;

// v30: RUN -> Service chain counters.  The engine only sees a working endpoint
// if data flows, and data flows only if the whole chain runs:
// SetState(RUN) -> KeSetTimerEx -> timer DPC -> port->Notify(serviceGroup)
// -> IServiceSink::RequestService -> Service().  played=0 with everything
// green above it used to be undiagnosable; now each hop counts itself.
static ULONG g_DpcFires;
static ULONG g_NotifyCalls;
static ULONG g_ServiceCalls;

// v24 call-trace counters.  Plain ULONGs, incremented from arbitrary threads
// and read from an IOCTL; a torn read only ever makes the snapshot
// self-inconsistent, never wrong in a way that changes a diagnosis.
static ULONG g_WaveInitCalls;
static ULONG g_TopoInitCalls;
static ULONG g_WaveIntersect;
static ULONG g_WaveIntersectProbe;
static ULONG g_WaveIntersectLastPin;
static ULONG g_WaveIntersectLastOutLen;
static ULONG g_WaveIntersectLastStatus;
static ULONG g_WaveIntersectReqSpec;
static ULONG g_TopoIntersect;

// v25: how many times we actually wrote a format (not just reported its size),
// and what the audio engine last asked for, to distinguish "range check failed
// before we were even asked" from "we were asked and still said no".
static ULONG g_WaveIntersectPhase2;
static ULONG g_ClientChannels;
static ULONG g_ClientSampleRate;
static ULONG g_ClientBits;

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

#define WAVE_BUFFER_BYTES   19200   // 100 ms of 48 kHz stereo 16-bit (covers mono too)
#define TIMER_PERIOD_MS     10

// ---------------------------------------------------------------------------
// Capture stream
//
// WaveCyclic: the miniport owns one cyclic DMA buffer.  We are the "hardware",
// so we fill it ourselves on a timer and hand the port driver the position; the
// port driver copies from the DMA buffer into the KS stream.
// ---------------------------------------------------------------------------
class CMiniportWaveCyclicStream : public IMiniportWaveCyclicStream, public IServiceSink, public IDmaChannel {
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

    // IDmaChannel - the stream is its own DMA channel, MSVAD style.  A software
    // device has no bus-master DMA: IPortWaveCyclic::NewMasterDmaChannel ends in
    // IoGetDmaAdapter, which fails on a root-enumerated device with
    // STATUS_DEVICE_CONFIGURATION_ERROR (0xC0000182) -- the V27 NewStream
    // failure.  MSVAD avoids the call entirely by handing the port the stream
    // object itself as the IDmaChannel; we do the same.
    STDMETHODIMP_(NTSTATUS) AllocateBuffer(IN ULONG BufferSize, IN PPHYSICAL_ADDRESS PhysicalAddressConstraint);
    STDMETHODIMP_(void)     FreeBuffer(void);
    STDMETHODIMP_(ULONG)    MaximumBufferSize(void);
    STDMETHODIMP_(ULONG)    AllocatedBufferSize(void);
    STDMETHODIMP_(ULONG)    BufferSize(void);
    STDMETHODIMP_(void)     SetBufferSize(IN ULONG BufferSize);
    STDMETHODIMP_(PHYSICAL_ADDRESS) PhysicalAddress(void);
    STDMETHODIMP_(ULONG)    TransferCount(void);
    STDMETHODIMP_(PVOID)    SystemAddress(void);
    STDMETHODIMP_(PADAPTER_OBJECT) GetAdapterObject(void);
    STDMETHODIMP_(void)     CopyTo(IN PVOID Destination, IN PVOID Source, IN ULONG RequestedLength);
    STDMETHODIMP_(void)     CopyFrom(IN PVOID Destination, IN PVOID Source, IN ULONG RequestedLength);

    // Private helper.  No port-side stream interface is handed to a WaveCyclic
    // miniport here (the port reaches the stream through the service group), so
    // this only records what the caller told us.
    NTSTATUS Init(IN ULONG Pin,
                  IN BOOLEAN Capture,
                  IN PKSDATAFORMAT DataFormat);

    void Service();

    PPORTWAVECYCLIC        m_Port;
    PSERVICEGROUP          m_ServiceGroup;
    // IDmaChannel storage (also aliased through m_Buffer/m_BufferSize for the
    // feeder service loop).
    PVOID                  m_DmaBuffer;
    ULONG                  m_DmaAllocated;  // bytes actually allocated
    ULONG                  m_DmaSize;       // logical buffer size
    PVOID                  m_Buffer;
    ULONG                  m_BufferSize;
    ULONG                  m_Position;
    ULONG                  m_NotificationInterval;
    ULONG                  m_Channels;       // negotiated channel count (1 or 2)
    ULONG                  m_FrameBytes;     // bytes per frame (channels * 2)
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
    m_DmaBuffer = NULL;
    m_DmaAllocated = 0;
    m_DmaSize = 0;
    m_Buffer = NULL;
    m_BufferSize = 0;
    m_Position = 0;
    m_NotificationInterval = 0;
    m_Channels = 1;
    m_FrameBytes = ISIGHTMIC_BITS / 8;
    m_State = KSSTATE_STOP;
    m_TimerOn = FALSE;
    m_RefCount = 1;
    KeInitializeTimer(&m_Timer);
    KeInitializeDpc(&m_Dpc, (PKDEFERRED_ROUTINE)StreamTimerDpc, (PVOID)this);
}

CMiniportWaveCyclicStream::~CMiniportWaveCyclicStream() {
    if (m_TimerOn) { KeCancelTimer(&m_Timer); m_TimerOn = FALSE; }
    FreeBuffer();
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
    else if (IsEqualGUIDAligned(iid, IID_IDmaChannel))
        *ppv = (PVOID)(IDmaChannel*)this;
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
    ULONG fb = m_FrameBytes ? m_FrameBytes : (ISIGHTMIC_BITS / 8);
    *PhysicalPosition = (*PhysicalPosition * 10000000LL) /
                        (ISIGHTMIC_SAMPLERATE * fb);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStream::SetFormat(IN PKSDATAFORMAT DataFormat) {
    UNREFERENCED_PARAMETER(DataFormat);
    // The buffer is allocated in Init (at creation); this call just re-zeroes
    // it for the format the port has settled on.
    m_Buffer = m_DmaBuffer;
    m_BufferSize = m_DmaSize;
    if (m_BufferSize) RtlZeroMemory(m_Buffer, m_BufferSize);
    return STATUS_SUCCESS;
}

// --- IDmaChannel: the stream is its own DMA channel (MSVAD style) ----------

STDMETHODIMP_(NTSTATUS) CMiniportWaveCyclicStream::AllocateBuffer(
    IN ULONG BufferSize, IN PPHYSICAL_ADDRESS PhysicalAddressConstraint) {
    UNREFERENCED_PARAMETER(PhysicalAddressConstraint);
    if (BufferSize == 0) return STATUS_INVALID_PARAMETER;
    if (m_DmaBuffer) {
        if (m_DmaAllocated >= BufferSize) { m_DmaSize = BufferSize; return STATUS_SUCCESS; }
        FreeBuffer();
    }
    m_DmaBuffer = ExAllocatePoolWithTag(NonPagedPool, BufferSize, ISIGHTMIC_POOL_TAG);
    if (!m_DmaBuffer) { g_FailDma++; return STATUS_INSUFFICIENT_RESOURCES; }
    m_DmaAllocated = BufferSize;
    m_DmaSize = BufferSize;
    RtlZeroMemory(m_DmaBuffer, BufferSize);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void) CMiniportWaveCyclicStream::FreeBuffer(void) {
    if (m_DmaBuffer) {
        ExFreePoolWithTag(m_DmaBuffer, ISIGHTMIC_POOL_TAG);
        m_DmaBuffer = NULL;
    }
    m_DmaAllocated = 0;
    m_DmaSize = 0;
    m_Buffer = NULL;
    m_BufferSize = 0;
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::MaximumBufferSize(void) {
    return WAVE_BUFFER_BYTES;
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::AllocatedBufferSize(void) {
    return m_DmaAllocated;
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::BufferSize(void) {
    return m_DmaSize;
}

STDMETHODIMP_(void) CMiniportWaveCyclicStream::SetBufferSize(IN ULONG BufferSize) {
    if (BufferSize <= m_DmaAllocated) m_DmaSize = BufferSize;
}

STDMETHODIMP_(PHYSICAL_ADDRESS) CMiniportWaveCyclicStream::PhysicalAddress(void) {
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = 0;
    if (m_DmaBuffer) pa = MmGetPhysicalAddress(m_DmaBuffer);
    return pa;
}

STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::TransferCount(void) {
    return m_DmaSize;
}

STDMETHODIMP_(PVOID) CMiniportWaveCyclicStream::SystemAddress(void) {
    return m_DmaBuffer;
}

// A software device has no DMA adapter; the port only needs one for hardware
// transfers, and our buffer is ordinary nonpaged pool.
STDMETHODIMP_(PADAPTER_OBJECT) CMiniportWaveCyclicStream::GetAdapterObject(void) {
    return NULL;
}

STDMETHODIMP_(void) CMiniportWaveCyclicStream::CopyTo(IN PVOID Destination,
                                                      IN PVOID Source,
                                                      IN ULONG RequestedLength) {
    if (Destination && Source && RequestedLength)
        RtlCopyMemory(Destination, Source, RequestedLength);
}

STDMETHODIMP_(void) CMiniportWaveCyclicStream::CopyFrom(IN PVOID Destination,
                                                        IN PVOID Source,
                                                        IN ULONG RequestedLength) {
    if (Destination && Source && RequestedLength)
        RtlCopyMemory(Destination, Source, RequestedLength);
}

// Returns the *previous* notification interval -- that is what this method is
// specified to report, not a status code.
STDMETHODIMP_(ULONG) CMiniportWaveCyclicStream::SetNotificationFreq(IN ULONG Interval,
                                                                   OUT PULONG FrameSize) {
    ULONG previous = m_NotificationInterval;
    m_NotificationInterval = Interval;
    // We service on a fixed 10 ms timer; report the frame size in bytes for the
    // channel count we are actually running.
    if (FrameSize) *FrameSize = m_FrameBytes ? m_FrameBytes : (ISIGHTMIC_BITS / 8);
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
    // The cyclic buffer is sized in SetFormat, once the port has picked the
    // format it actually wants to run.  Here we record the channel count so
    // Service() can upmix the mono feeder stream to what the engine asked for.
    m_Channels = 1;
    m_FrameBytes = ISIGHTMIC_BITS / 8;   // 2 for mono
    if (DataFormat && DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        PKSDATAFORMAT_WAVEFORMATEX wf = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;
        ULONG ch = wf->WaveFormatEx.nChannels;
        if (ch < 1) ch = 1;
        if (ch > (ULONG)ISIGHTMIC_MAX_CHANNELS) ch = (ULONG)ISIGHTMIC_MAX_CHANNELS;
        m_Channels = ch;
        m_FrameBytes = (ISIGHTMIC_BITS / 8) * ch;
    }
    // Allocate the cyclic buffer HERE, at stream creation (MSVAD does the
    // same).  PortCls does NOT call SetFormat when a pin is created with a
    // format in the connect block -- SetFormat only runs when the engine later
    // sends KSPROPERTY_CONNECTION_DATAFORMAT.  A buffer-less stream silently
    // no-ops in RUN (the timer never starts, Service never fires) and the
    // port's position handler fails with STATUS_UNSUCCESSFUL, which is exactly
    // what turned the engine's endpoint build into AUDCLNT_E_ENDPOINT_CREATE_
    // FAILED even though the pin itself was healthy.
    NTSTATUS st = AllocateBuffer(WAVE_BUFFER_BYTES, NULL);
    if (!NT_SUCCESS(st)) return st;
    m_Buffer = m_DmaBuffer;
    m_BufferSize = m_DmaSize;
    RtlZeroMemory(m_Buffer, m_BufferSize);
    return STATUS_SUCCESS;
}

// One 10 ms quantum: copy from the ring into the cyclic DMA buffer, wrapping at
// the end.  Whatever the ring cannot supply becomes silence, so the endpoint
// always runs at real time even when the camera is not streaming.
void CMiniportWaveCyclicStream::Service() {
    g_ServiceCalls++;
    if (m_State != KSSTATE_RUN || !m_Buffer || !m_BufferSize || m_FrameBytes == 0) return;

    const ULONG frames  = ISIGHTMIC_SAMPLERATE / 100;   // 480 frames per 10 ms tick
    const ULONG fb      = m_FrameBytes;                 // 2 (mono) or 4 (stereo)
    const ULONG quantum = fb * frames;                  // bytes to write this tick
    ULONG pos = m_Position % m_BufferSize;
    PUCHAR base = (PUCHAR)m_Buffer;

    // The feeder ring is mono 16-bit.  For each frame we pull one mono sample
    // and write it to every channel the engine asked for (upmix).  Positions
    // wrap modulo the cyclic buffer.
    for (ULONG f = 0; f < frames; f++) {
        INT16 s = 0;
        ULONG got = RingPull(&g_Ring, (PUCHAR)&s, 2);
        if (got < 2) g_Starved += fb;
        for (ULONG c = 0; c < m_Channels; c++) {
            *(PINT16)(base + pos) = s;   // s == 0 when the ring was dry -> silence
            pos = (pos + 2) % m_BufferSize;
        }
    }

    m_Position = (m_Position + quantum) % m_BufferSize;
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
    g_DpcFires++;
    if (s->m_Port != NULL && s->m_ServiceGroup != NULL) {
        g_NotifyCalls++;
        s->m_Port->Notify(s->m_ServiceGroup);
    }
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
        ISIGHTMIC_MAX_CHANNELS,               // MaximumChannels (now stereo-capable)
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

// Pin category of the streaming pin on a *capture* wave filter.
//
// This is not decoration.  Two independent working capture filters on the
// machine this was measured on -- Realtek's rtmicinwave and rtstereomixwave --
// both report KSPROPERTY_PIN_CATEGORY = {FB6C4281-...} for their host pin,
// while the render sibling on the same codec (rearlineoutwave3) reports
// KSCATEGORY_AUDIO {6994AD04-...}.  This table used to say KSCATEGORY_AUDIO,
// i.e. the render value, on a capture pin.  The consequence was measurable:
// through a full round of GetMixFormat / IsFormatSupported / Initialize
// (shared *and* exclusive) KSPROPERTY_PIN_GLOBALCINSTANCES stayed {1,0} and
// this driver's own stream counter stayed 0 -- the audio engine never
// instantiated the host pin, so NewStream was never reached and WASAPI could
// only report AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008).
//
// ksmedia.h spells these bytes PINNAME_CAPTURE (== STATIC_PINNAME_VIDEO_CAPTURE);
// uuids.h calls the same value PIN_CATEGORY_CAPTURE.  It is written out
// literally here rather than referenced symbolically because the WDK header
// only ships the alias macro and the exact spelling moves between SDK versions.
static const GUID ISIGHTMIC_PIN_CATEGORY_CAPTURE = {
    0xFB6C4281, 0x0353, 0x11D1, { 0x90, 0x5F, 0x00, 0x00, 0xC0, 0xCC, 0x16, 0xBA }
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
        //
        // Two fields in this entry are load-bearing, and both were wrong.
        //
        // (a) The third number is MinFilterInstanceCount.  portcls.h maps it to
        // KSPROPERTY_PIN_NECESSARYINSTANCES, which MSDN defines as "a definite
        // lower bound on the number of instances of a pin that must exist in
        // order for a filter to be able to function".  It used to be 0 here.
        // Realtek's rtmicinwave (a working capture filter on the same machine)
        // reports 1 for exactly this pin role.
        //
        // (b) The pin category ("Category" below) is what tells the audio
        // engine whether this host pin carries render or capture audio.  It
        // used to be KSCATEGORY_AUDIO -- the *render* value -- on a capture
        // pin.  See ISIGHTMIC_PIN_CATEGORY_CAPTURE above for the measurements.
        //
        // The measurement that pinned this down: while WASAPI ran its whole
        // round of GetMixFormat / IsFormatSupported / Initialize (shared and
        // exclusive), KSPROPERTY_PIN_GLOBALCINSTANCES -- the kernel's own
        // global instance counter, not the per-filter-instance one -- stayed
        // {1,0}.  A working render pin on this machine reads {1,1} in the same
        // poll, so the metric is meaningful: the audio engine never
        // instantiated our host pin at all, which is why NewStream was never
        // reached and WASAPI could only report
        // AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008).
        //
        // (The bridge pin above stays 0,0,0 and KSCATEGORY_AUDIO -- that is
        // what the documentation prescribes for bridge pins, and both working
        // reference filters do the same.)
        1, 1, 1,            // instance counts (global, filter, min)
        NULL,               // AutomationTable
        {
            0, NULL,        // Interfaces
            0, NULL,        // Mediums
            1, (const PKSDATARANGE*)PinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &ISIGHTMIC_PIN_CATEGORY_CAPTURE,
            NULL,
            { 0 }
        }
    }
};

// ---- V31: channel-config automation ---------------------------------------
// wdmaud/audioses computes the endpoint mix format by asking the topology for
// KSPROPERTY_AUDIO_CHANNEL_CONFIG.  Every node/filter below had a NULL
// AutomationTable, so that query answered STATUS_NOT_FOUND (1168) and the
// engine bailed out before ever creating a pin (the 0x88890008 wall).
static ULONG g_ChannelConfig = 0x3;   // KSAUDIO_SPEAKER_STEREO (FL|FR)

static NTSTATUS PropertyHandlerChannelConfig(IN PPCPROPERTY_REQUEST PropertyRequest) {
    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (PropertyRequest->ValueSize < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        *(PULONG)PropertyRequest->Value =
            KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
        PropertyRequest->Irp->IoStatus.Information = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET) {
        if (PropertyRequest->ValueSize < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        *(PULONG)PropertyRequest->Value = g_ChannelConfig;
        PropertyRequest->Irp->IoStatus.Information = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET) {
        if (PropertyRequest->ValueSize < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        g_ChannelConfig = *(PULONG)PropertyRequest->Value;
        PropertyRequest->Irp->IoStatus.Information = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

static const PCPROPERTY_ITEM ChannelConfigProperties[] = {
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CHANNEL_CONFIG,
        PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET |
            PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
        PropertyHandlerChannelConfig
    }
};
DEFINE_PCAUTOMATION_TABLE_PROP(ChannelConfigAutomation, ChannelConfigProperties);

// The ADC between the bridge and the streaming pin -- the reference capture
// filter has one, and it is the node wdmaudio expects to see on the capture
// path.  It serves the channel-config property (V31); there is still no
// volume/mute control.
static PCNODE_DESCRIPTOR WaveNodes[] = {
    {
        0,                      // Flags
        &ChannelConfigAutomation,   // AutomationTable
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
    g_WaveInitCalls++;
    return STATUS_SUCCESS;
}

// Record one NewStream failure: bump the site counter, and remember the last
// NTSTATUS so a user-mode caller can tell STATUS_INSUFFICIENT_RESOURCES from
// STATUS_INVALID_DEVICE_STATE without a debugger attached.
static void NoteNewStreamFailure(PULONG Site, NTSTATUS st) {
    (*Site)++;
    g_NewStreamFailed++;
    g_LastFailStatus = (ULONG)st;
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
    g_NewStreamEntered++;

    CMiniportWaveCyclicStream* s =
        new(PoolType, ISIGHTMIC_POOL_TAG) CMiniportWaveCyclicStream(OuterUnknown);
    if (!s) { NoteNewStreamFailure(&g_FailStreamInit, STATUS_INSUFFICIENT_RESOURCES);
              return STATUS_INSUFFICIENT_RESOURCES; }

    NTSTATUS st = STATUS_SUCCESS;

    st = s->Init(Pin, TRUE, DataFormat);
    if (!NT_SUCCESS(st)) {
        NoteNewStreamFailure(&g_FailStreamInit, st);
        s->Release(); return st;
    }

    // Service group: PortCls calls the stream back through it when we notify.
    st = PcNewServiceGroup(&s->m_ServiceGroup, NULL);
    if (!NT_SUCCESS(st)) {
        NoteNewStreamFailure(&g_FailServiceGroup, st);
        s->Release(); return st;
    }
    s->m_ServiceGroup->AddMember(PSERVICESINK(s));

    s->m_Port = m_Port;
    if (s->m_Port) s->m_Port->AddRef();

    *Stream = (PMINIPORTWAVECYCLICSTREAM)s;
    (*Stream)->AddRef();
    // The stream is its own DMA channel (MSVAD style) -- see the class comment.
    *DmaChannel = (PDMACHANNEL)(IDmaChannel*)s;
    (*DmaChannel)->AddRef();
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
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);

    // v27 dual-phase, driven by hard data from v25 and v26:
    //   v25 (always BUFFER_TOO_SMALL): PortCls called 6982 times, EVERY one
    //          with OutputBufferLength==0, and never came back with a buffer.
    //   v26 (always NOT_IMPLEMENTED): PortCls suddenly called with
    //          OutputBufferLength==82 == sizeof(KSDATAFORMAT_WAVEFORMATEX),
    //          i.e. the buffer we asked for all along -- but we had nothing
    //          to write, so the format still never materialised.
    // So: if the buffer is big enough, WRITE the format and succeed (that is
    // the only path that ever produces a format).  If it is not, return
    // NOT_IMPLEMENTED, which provably pushes PortCls into the buffered call.
    g_WaveIntersect++;
    g_WaveIntersectLastPin = PinId;
    g_WaveIntersectLastOutLen = OutputBufferLength;
    if (MatchingDataRange) {
        g_WaveIntersectReqSpec = MatchingDataRange->Specifier.Data1;
        if (MatchingDataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
            PKSDATARANGE_AUDIO a = (PKSDATARANGE_AUDIO)MatchingDataRange;
            g_ClientChannels   = a->MaximumChannels;
            g_ClientSampleRate = a->MaximumSampleFrequency;
            g_ClientBits       = a->MaximumBitsPerSample;
        }
    }

    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX) || !ResultantFormat) {
        g_WaveIntersectLastStatus = STATUS_NOT_IMPLEMENTED;
        return STATUS_NOT_IMPLEMENTED;
    }
    g_WaveIntersectPhase2++;

    // We accept the channel count the engine asked for (1 or 2) but
    // physically produce 48 kHz / 16-bit mono and upmix to the requested
    // channel count, so the only variable is nChannels.
    ULONG channels = 1;
    if (MatchingDataRange && MatchingDataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
        PKSDATARANGE_AUDIO a = (PKSDATARANGE_AUDIO)MatchingDataRange;
        if (a->MaximumChannels >= 1) {
            channels = a->MaximumChannels;
            if (channels > (ULONG)ISIGHTMIC_MAX_CHANNELS) channels = (ULONG)ISIGHTMIC_MAX_CHANNELS;
        }
    }
    PKSDATAFORMAT_WAVEFORMATEX fmt = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
    RtlZeroMemory(fmt, sizeof(KSDATAFORMAT_WAVEFORMATEX));
    fmt->DataFormat.FormatSize  = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    fmt->DataFormat.SampleSize  = (ULONG)(ISIGHTMIC_BITS / 8 * channels);
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
    fmt->WaveFormatEx.nChannels       = (WORD)channels;
    fmt->WaveFormatEx.nSamplesPerSec  = ISIGHTMIC_SAMPLERATE;
    fmt->WaveFormatEx.nBlockAlign     = (WORD)(ISIGHTMIC_BITS / 8 * channels);
    fmt->WaveFormatEx.wBitsPerSample  = ISIGHTMIC_BITS;
    fmt->WaveFormatEx.cbSize          = 0;
    fmt->WaveFormatEx.nAvgBytesPerSec = ISIGHTMIC_SAMPLERATE * (ISIGHTMIC_BITS / 8 * channels);
    if (ResultantFormatLength) *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    g_WaveIntersectLastStatus = STATUS_SUCCESS;
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
        &ChannelConfigAutomation,   // AutomationTable: serves CHANNEL_CONFIG (V31)
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
    g_TopoInitCalls++;
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
    g_TopoIntersect++;
    if (ResultantFormat && OutputBufferLength >= sizeof(KSDATARANGE) && MatchingDataRange)
        RtlCopyMemory(ResultantFormat, MatchingDataRange, sizeof(KSDATARANGE));
    if (ResultantFormatLength) *ResultantFormatLength = sizeof(KSDATARANGE);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Adapter: bind a port + miniport pair and register it as a subdevice
//
// A subdevice is the binding of FOUR things: a port object, a miniport object,
// a resource list, and a reference string.  PcRegisterSubdevice takes the
// PORT -- its signature is (DeviceObject, Name, PUNKNOWN Unknown) and the docs
// say Unknown is "the IPort interface of the port driver object that is bound
// to the subdevice".
//
// Handing it the *miniport* is what broke the first two installs: PortCls
// queried the object for IPort, the miniport's QueryInterface answered
// STATUS_INVALID_PARAMETER (the PortCls convention for "no such interface"),
// and PcRegisterSubdevice propagated it.  PnP recorded that as problem 0x0a
// CM_PROB_FAILED_START with problem status 0xc000000d, StartDevice failed, no
// subdevice was ever enumerated, and "iSight Microphone (FireWire)" never
// appeared -- while the control device kept working, because DriverEntry
// creates that one and DriverEntry does not care about any of this.
//
// The sequence is the one in the "Subdevice Creation" topic:
//     PcNewPort -> IPort::Init(DeviceObject, Irp, miniport, adapter, resources)
//     -> PcRegisterSubdevice(DeviceObject, name, port)
//     -> drop both references (the port holds its own on the miniport, and
//        PcRegisterSubdevice holds its own on the port).
// UnknownAdapter may be NULL -- the docs say so explicitly, and we have no
// adapter object to pass.
// ---------------------------------------------------------------------------
static NTSTATUS InstallSubdevice(PDEVICE_OBJECT DeviceObject,
                                 PIRP Irp,
                                 PRESOURCELIST ResourceList,
                                 PWSTR Name,
                                 REFCLSID PortClassId,
                                 PUNKNOWN Miniport,
                                 PPORT* OutPort) {
    PPORT port = NULL;
    NTSTATUS st = PcNewPort(&port, PortClassId);
    if (!NT_SUCCESS(st)) return st;

    st = port->Init(DeviceObject, Irp, Miniport, NULL, ResourceList);
    if (NT_SUCCESS(st))
        st = PcRegisterSubdevice(DeviceObject, Name, port);
    // No DbgPrint here on purpose: it needs a kernel debugger to be seen at all,
    // while the installer now writes the PnP problem code into its report --
    // `pnputil /enum-devices /problem` names CM_PROB_FAILED_START in plain
    // English, which is what actually localised this bug.

    if (!NT_SUCCESS(st)) {
        port->Release();
        return st;
    }
    if (OutPort) *OutPort = port;       // caller releases it
    else         port->Release();
    return STATUS_SUCCESS;
}

static NTSTATUS StartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList) {
    NTSTATUS st;
    PPORT wavePort = NULL;
    PPORT topoPort = NULL;

    CMiniportWaveCyclic* w = new(NonPagedPool, ISIGHTMIC_POOL_TAG) CMiniportWaveCyclic(NULL);
    if (!w) return STATUS_INSUFFICIENT_RESOURCES;
    PUNKNOWN wave = (PUNKNOWN)(IMiniportWaveCyclic*)w;

    // The name must match the KSNAME_* reference strings in isightmic.inf, and
    // the buffer has to stay valid for the device object's lifetime -- a string
    // literal in the driver's .rdata outlives it.
    st = InstallSubdevice(DeviceObject, Irp, ResourceList, (PWSTR)L"Wave",
                          CLSID_PortWaveCyclic, wave, &wavePort);
    wave->Release();
    if (!NT_SUCCESS(st)) return st;

    CMiniportTopology* t = new(NonPagedPool, ISIGHTMIC_POOL_TAG) CMiniportTopology(NULL);
    if (!t) { wavePort->Release(); return STATUS_INSUFFICIENT_RESOURCES; }
    PUNKNOWN topo = (PUNKNOWN)(IMiniportTopology*)t;

    st = InstallSubdevice(DeviceObject, Irp, ResourceList, (PWSTR)L"Topology",
                          CLSID_PortTopology, topo, &topoPort);
    topo->Release();
    if (!NT_SUCCESS(st)) { wavePort->Release(); return st; }

    // ---------------------------------------------------------------------
    // THE STEP THAT WAS MISSING (2026-09-24).  Registering the two subdevices
    // is not enough: PortCls also has to be told that the wave filter's bridge
    // pin is hard-wired to the topology filter's bridge pin.  That registration
    // is the ONLY thing that gives KSPROPERTY_PIN_PHYSICALCONNECTION any
    // content, and SysAudio / AudioEndpointBuilder walk exactly that property
    // to pair a wave pin with a topology pin and build the audio graph.
    //
    // Without it the driver looks healthy from every angle: the device starts,
    // all four KSCATEGORY_* interfaces appear under DeviceClasses, and
    // isight-micdev.exe (which enumerates KSCATEGORY_CAPTURE) proudly lists
    // "iSight Microphone (FireWire)" -- yet HKLM\...\MMDevices\Audio\Capture
    // stays empty, so Sound, WeChat and everything else see no microphone.
    //
    // Proven on the box with isight-check/ksprobe.py: the Realtek wave filter
    // answers KSPROPERTY_PIN_PHYSICALCONNECTION with 270 bytes naming its
    // topology filter; every iSight pin answered nothing.
    //
    // Direction matters.  FromUnknown is the port whose subdevice *supplies*
    // the data (its pin is the output side); ToUnknown is the one that *sinks*
    // it.  On a capture path the signal leaves the topology's bridge pin and
    // enters the wave filter's bridge pin, so topology is From, wave is To.
    // ---------------------------------------------------------------------
    st = PcRegisterPhysicalConnection(DeviceObject,
                                      topoPort, KSPIN_TOPO_WAVE_BRIDGE,
                                      wavePort, KSPIN_WAVE_BRIDGE);

    topoPort->Release();
    wavePort->Release();
    return st;
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
        } else if (code == IOCTL_ISIGHTMIC_GETBUILD) {
            ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
            if (buf && outLen >= 1) {
                ULONG n = (ULONG)sizeof(ISIGHTMIC_BUILD_TAG);   // includes the NUL
                RtlZeroMemory(buf, outLen);
                if (n > outLen) n = outLen;
                RtlCopyMemory(buf, ISIGHTMIC_BUILD_TAG, n - 1);
                info = n;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
        } else if (code == IOCTL_ISIGHTMIC_GETDIAG) {
            ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
            if (buf && outLen >= sizeof(ISIGHTMIC_DIAG)) {
                PISIGHTMIC_DIAG dg = (PISIGHTMIC_DIAG)buf;
                RtlZeroMemory(dg, sizeof(ISIGHTMIC_DIAG));
                dg->NewStreamEntered = g_NewStreamEntered;
                dg->NewStreamFailed  = g_NewStreamFailed;
                dg->FailDma          = g_FailDma;
                dg->FailStreamInit   = g_FailStreamInit;
                dg->FailServiceGroup = g_FailServiceGroup;
                dg->LastFailStatus   = g_LastFailStatus;
                dg->WaveInitCalls    = g_WaveInitCalls;
                dg->TopoInitCalls    = g_TopoInitCalls;
                dg->WaveIntersect    = g_WaveIntersect;
                dg->WaveIntersectProbe = g_WaveIntersectProbe;
                dg->WaveIntersectLastPin = g_WaveIntersectLastPin;
                dg->WaveIntersectLastOutLen = g_WaveIntersectLastOutLen;
                dg->WaveIntersectLastStatus = g_WaveIntersectLastStatus;
                dg->WaveIntersectReqSpec = g_WaveIntersectReqSpec;
                dg->TopoIntersect    = g_TopoIntersect;
    dg->WaveIntersectPhase2 = g_WaveIntersectPhase2;
    dg->ClientChannels  = g_ClientChannels;
    dg->ClientSampleRate = g_ClientSampleRate;
    dg->ClientBits      = g_ClientBits;
    dg->StateLast       = g_State;
    dg->DpcFires        = g_DpcFires;
    dg->NotifyCalls     = g_NotifyCalls;
    dg->ServiceCalls    = g_ServiceCalls;
                info = sizeof(ISIGHTMIC_DIAG);
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
