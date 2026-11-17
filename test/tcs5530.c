/* White-box failure tests of production CS5530 code. The HAL and port I/O are
 * modeled here; this does not emulate VSA timing or validate analog audio. */
#include "audiont.h"
#include "geode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

ULONG g_NtVersion = 0x0501;
static UCHAR mixer[256], selected, response[4], nr, rp;
static UCHAR commands[64];
static ULONG ncmd, acks, syncs, resetCount, maps, flushes;
static ULONG residual, shortMap, availableMaps = 8, requestedMaps;
static int mapToken, nullMapBase;
static int poolLive, commonLive, mdlLive, adapterLive, channelLive;
static int failPool, failAdapter, failCommon, failMdl, failChannel, busy, noResponse;
static DMA_ADAPTER adapter;
static DMA_OPERATIONS dma;

PVOID OsAlloc(ULONG bytes) { if (failPool) return NULL; poolLive++; return calloc(1, bytes); }
void OsFree(PVOID p) { if (p) { poolLive--; free(p); } }
void OsStallUs(ULONG us) { (void)us; }
void OsLog(ULONG level, const char *fmt, ...) { (void)level; (void)fmt; }
int OsSprintf(char *buf, int n, const char *fmt, ...)
{ int rc; va_list ap; va_start(ap, fmt); rc = vsnprintf(buf, n, fmt, ap); va_end(ap); return rc; }
PVOID OsResolve(int hal, const char *name) { (void)hal; (void)name; return (PVOID)1; }

static UCHAR PortRead(PUCHAR p)
{
    ULONG off = (ULONG)(ULONG_PTR)p - 0x220;
    if (off == 0xc) return busy ? 0x80 : 0;
    if (off == 0xe) return noResponse ? 0 : 0x80;
    if (off == 0xf) { acks++; return 0; }
    if (off == 0xa) { assert(rp < nr); return response[rp++]; }
    if (off == 5) return mixer[selected];
    assert(0); return 0;
}
static void PortWrite(PUCHAR p, UCHAR v)
{
    ULONG off = (ULONG)(ULONG_PTR)p - 0x220;
    if (off == 6) {
        if (!v) { response[0] = 0xaa; nr = 1; rp = 0; resetCount++; }
    } else if (off == 4) selected = v;
    else if (off == 5) mixer[selected] = v;
    else if (off == 0xc) {
        assert(ncmd < sizeof(commands)); commands[ncmd++] = v;
        if (v == 0xe1) { response[0] = 4; response[1] = 12; nr = 2; rp = 0; }
    } else assert(0);
}
static PDMA_ADAPTER GetAdapter(PDEVICE_OBJECT p, PDEVICE_DESCRIPTION d, PULONG n)
{
    (void)p;
    assert(d->InterfaceType == Isa && d->DmaChannel == 5 && d->DmaWidth == Width16Bits);
    assert(d->AutoInitialize && !d->Master && !d->Dma32BitAddresses && d->MaximumLength == RING_BYTES);
    if (failAdapter) return NULL;
    adapterLive++; *n = availableMaps; return &adapter;
}
static void NTAPI PutAdapter(PDMA_ADAPTER a) { assert(a == &adapter); adapterLive--; }
static PVOID NTAPI Common(PDMA_ADAPTER a, ULONG n, PPHYSICAL_ADDRESS pa, BOOLEAN cached)
{
    assert(a == &adapter && !cached && n == RING_BYTES);
    if (failCommon) return NULL;
    commonLive++; pa->QuadPart = 0x80000;
    PVOID p = _aligned_malloc(n, PAGE_SIZE); assert(p); memset(p, 0, n); return p;
}
static void NTAPI FreeCommon(PDMA_ADAPTER a, ULONG n, PHYSICAL_ADDRESS pa, PVOID p, BOOLEAN cached)
{ (void)a; (void)n; (void)pa; (void)cached; assert(!channelLive); commonLive--; _aligned_free(p); }
static PMDL Mdl(PVOID p, ULONG n, BOOLEAN sec, BOOLEAN quota, PIRP irp)
{ (void)p; (void)n; (void)sec; (void)quota; (void)irp; if (failMdl) return NULL; mdlLive++; return calloc(1, sizeof(MDL)); }
static void FreeMdl(PMDL p) { mdlLive--; free(p); }
static void BuildMdl(PMDL p) { assert(p); }
static NTSTATUS NTAPI Channel(PDMA_ADAPTER a, PDEVICE_OBJECT dev, ULONG n, PDRIVER_CONTROL cb, PVOID ctx)
{
    assert(a == &adapter && n <= availableMaps);
    requestedMaps = n;
    if (failChannel) return STATUS_INSUFFICIENT_RESOURCES;
    channelLive++;
    assert(cb(dev, NULL, (n && !nullMapBase) ? &mapToken : NULL, ctx) == KeepObject);
    return STATUS_SUCCESS;
}
static void NTAPI FreeChannel(PDMA_ADAPTER a) { assert(a == &adapter); channelLive--; }
static PHYSICAL_ADDRESS NTAPI Map(PDMA_ADAPTER a, PMDL mdl, PVOID base, PVOID va, PULONG n, BOOLEAN write)
{
    PHYSICAL_ADDRESS pa;
    assert(a == &adapter && mdl && va && write && *n == RING_BYTES && channelLive);
    maps++; pa.QuadPart = 0x80000;
    /* HAL's NULL-map-register fast path only translates the buffer. It must
       not accidentally make a zero-register system-DMA request look active. */
    if (!base) return pa;
    assert(base == &mapToken);
    if (shortMap) *n = shortMap;
    residual = *n;
    return pa;
}
static BOOLEAN NTAPI Flush(PDMA_ADAPTER a, PMDL mdl, PVOID base, PVOID va, ULONG n, BOOLEAN write)
{ (void)a; (void)mdl; (void)base; (void)va; assert(n == RING_BYTES && write); flushes++; return TRUE; }
static ULONG NTAPI Counter(PDMA_ADAPTER a) { assert(a == &adapter); return residual; }
static void EventInit(PRKEVENT ev, EVENT_TYPE type, BOOLEAN set) { (void)type; ev->Header.SignalState = set; }
static LONG EventSet(PRKEVENT ev, KPRIORITY inc, BOOLEAN wait) { (void)inc; (void)wait; ev->Header.SignalState = 1; return 0; }
static NTSTATUS Wait(PVOID ev, KWAIT_REASON why, KPROCESSOR_MODE mode, BOOLEAN alert, PLARGE_INTEGER time)
{ (void)why; (void)mode; (void)alert; (void)time; assert(((PKEVENT)ev)->Header.SignalState == 1); return STATUS_SUCCESS; }
static void Raise(KIRQL q, PKIRQL old) { assert(q == DISPATCH_LEVEL); *old = PASSIVE_LEVEL; }
static void Lower(KIRQL old) { assert(old == PASSIVE_LEVEL); }
static BOOLEAN Sync(PKINTERRUPT i, PKSYNCHRONIZE_ROUTINE f, PVOID p) { assert(i); syncs++; return f(p); }

/* Interpose only OS/hardware boundaries; backend functions are unchanged. */
#define READ_PORT_UCHAR PortRead
#define WRITE_PORT_UCHAR PortWrite
#define IoGetDmaAdapter GetAdapter
#define IoAllocateMdl Mdl
#define IoFreeMdl FreeMdl
#define MmBuildMdlForNonPagedPool BuildMdl
#define KeInitializeEvent EventInit
#define KeSetEvent EventSet
#define KeWaitForSingleObject Wait
#undef KeRaiseIrql
#define KeRaiseIrql Raise
#define KeLowerIrql Lower
#define KeSynchronizeExecution Sync
#ifndef _X86_
#define _X86_ 1 /* select the backend after including the host's DDK declarations */
#endif
#include "../drv/cs5530.c"

static void Clean(void)
{
    assert(!poolLive && !adapterLive && !commonLive && !mdlLive && !channelLive);
}
static void Fresh(CARD *c)
{
    GEODE_RES r;
    Clean(); memset(c, 0, sizeof(*c)); memset(&r, 0, sizeof(r));
    r.SbBase = 0x220; r.FmBase = 0x388; r.Dma8 = 1; r.Dma16 = 5; r.Irq = 5;
    assert(NT_SUCCESS(Cs5530Attach(c, (PDEVICE_OBJECT)1, (PDEVICE_OBJECT)2, &r)));
    memset(mixer, 0, sizeof(mixer)); mixer[0x80] = 2; mixer[0x81] = 0x22;
    ncmd = resetCount = maps = flushes = acks = syncs = shortMap = 0;
    residual = 2; /* unprogrammed channel: raw count 0, 16-bit words */
    requestedMaps = 0;
}
int main(void)
{
    CARD c;
    int *faults[] = { &failAdapter, &failCommon, &failMdl, &failChannel };
    unsigned i;
    static const UCHAR start[] = { 0x41, 0xac, 0x44, 0x42, 0xac, 0x44, 0xd1, 0xb6, 0x30, 0xff, 3 };
    memset(&dma, 0, sizeof(dma)); adapter.DmaOperations = &dma;
    dma.PutDmaAdapter = PutAdapter; dma.AllocateCommonBuffer = Common; dma.FreeCommonBuffer = FreeCommon;
    dma.AllocateAdapterChannel = Channel; dma.FreeAdapterChannel = FreeChannel;
    dma.MapTransfer = Map; dma.FlushAdapterBuffers = Flush; dma.ReadDmaCounter = Counter;
    Fresh(&c); assert(NT_SUCCESS(Init(&c)));
    assert(requestedMaps == 8 && ((CS5530 *)c.Geode)->MapBase == &mapToken);
    assert(c.EngineRate == 44100 && !c.CanRecord && c.NoSynth && c.BackendBuffers);
    ncmd = 0; assert(Start(&c)); assert(ncmd == sizeof(start) && !memcmp(commands, start, sizeof(start)));
    assert(((CS5530 *)c.Geode)->DmaBeforeMap == 2);
    assert(((CS5530 *)c.Geode)->DmaAfterMap == RING_BYTES);
    assert(Position(&c) == 0); residual = RING_BYTES - 2048; assert(Position(&c) == 512);
    residual = 0; assert(Position(&c) == 0);
    c.Interrupt = (PKINTERRUPT)1; c.Lines[LINE_MASTER].Left = 65535; c.Lines[LINE_MASTER].Right = 0;
    ApplyLine(&c, LINE_MASTER); assert(syncs == 1 && mixer[0x30] == 248 && mixer[0x31] == 0);
    c.Lines[LINE_MASTER].Mute = TRUE; ApplyLine(&c, LINE_MASTER); assert(mixer[0x30] == 0);
    mixer[0x82] = 0; assert(!Isr(&c)); mixer[0x82] = 2; assert(Isr(&c) && acks == 1);
    Stop(&c); assert(flushes == 1 && !((CS5530 *)c.Geode)->Mapped);
    ncmd = 0; assert(Start(&c)); Stop(&c); assert(maps == 2 && flushes == 2);
    Shutdown(&c); assert(!c.Geode && !c.PlayRing.Va); Clean();
    for (i = 0; i < ARRAYSIZE(faults); i++) {
        Fresh(&c); *faults[i] = 1; assert(!NT_SUCCESS(Init(&c)));
        Shutdown(&c); *faults[i] = 0; Clean();
    }
    Fresh(&c); availableMaps = 7;
    assert(Init(&c) == STATUS_INSUFFICIENT_RESOURCES && !channelLive);
    Shutdown(&c); availableMaps = 8; Clean();
    Fresh(&c); nullMapBase = 1;
    assert(Init(&c) == STATUS_DEVICE_CONFIGURATION_ERROR && channelLive);
    Shutdown(&c); nullMapBase = 0; Clean();
    Fresh(&c); mixer[0x81] = 0x42; assert(!NT_SUCCESS(Init(&c))); assert(mixer[0x81] == 0x42);
    Shutdown(&c); Clean(); /* resource mismatch is refused, never reprogrammed */
    Fresh(&c); noResponse = 1; assert(!NT_SUCCESS(Init(&c))); noResponse = 0; Shutdown(&c); Clean();
    Fresh(&c); assert(NT_SUCCESS(Init(&c))); shortMap = 4096;
    assert(!Start(&c)); assert(flushes == 1); Shutdown(&c); Clean();
    Fresh(&c); assert(NT_SUCCESS(Init(&c))); busy = 1;
    assert(!Start(&c)); busy = 0; assert(flushes == 1); Shutdown(&c); Clean();
    failPool = 1; assert(!NT_SUCCESS(Cs5530Attach(&c, NULL, NULL, NULL))); failPool = 0; Clean();
    puts("RESULT tcs5530 PASS (modeled HAL/DSP: playback, restart, IRQ, volume, timeout and allocation unwind)");
    return 0;
}
