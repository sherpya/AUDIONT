/* SPDX-License-Identifier: GPL-3.0-or-later
 * CS5530/GX1: playback through the BIOS VSA1 SB16 interface, owned by CRX0002.
 * This is NOT the PCI-native NSC interface and does not install/patch VSA code.
 * See docs/geode.md for provenance, resource ownership and test limitations.
 */
#include "audiont.h"
#include "geode.h"

#if defined(AUDIONT_WDM) && defined(_X86_)
typedef struct {
    GEODE_RES R;
    PDEVICE_OBJECT Pdo, Fdo;
    PDMA_ADAPTER Adapter;
    PVOID MapBase, Buffer;
    PHYSICAL_ADDRESS Logical;
    PMDL Mdl;
    KEVENT Allocated;
    BOOLEAN Channel, Mapped, DspReady;
    UCHAR Major, Minor, IrqMask, DmaMask;
    ULONG Errors, MapRegisters, AvailableMaps;
    ULONG DmaBeforeMap, DmaAfterMap, DmaRemaining;
} CS5530;

static UCHAR Rd(CS5530 *s, ULONG off) { return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(s->R.SbBase + off)); }
static void Wr(CS5530 *s, ULONG off, UCHAR v) { WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)(s->R.SbBase + off), v); }
static UCHAR MixRead(CS5530 *s, UCHAR reg)
{
    UCHAR v;
    Wr(s, 4, reg); OsStallUs(20);
    v = Rd(s, 5); OsStallUs(20);
    return v;
}
static void MixWrite(CS5530 *s, UCHAR reg, UCHAR val)
{
    Wr(s, 4, reg); OsStallUs(20); Wr(s, 5, val); OsStallUs(20);
}
static BOOLEAN Command(CS5530 *s, UCHAR cmd)
{
    ULONG i;
    for (i = 0; i < 1000; i++) {
        if (!(Rd(s, 0xc) & 0x80)) { Wr(s, 0xc, cmd); return TRUE; }
        OsStallUs(1);
    }
    s->Errors++;
    return FALSE;
}
static BOOLEAN Response(CS5530 *s, UCHAR *v)
{
    ULONG i;
    for (i = 0; i < 1000; i++) {
        if (Rd(s, 0xe) & 0x80) { *v = Rd(s, 0xa); return TRUE; }
        OsStallUs(1);
    }
    s->Errors++;
    return FALSE;
}
static BOOLEAN Reset(CS5530 *s)
{
    UCHAR v = 0;
    Wr(s, 6, 1); OsStallUs(10); Wr(s, 6, 0);
    return Response(s, &v) && v == 0xaa;
}

/* The START IRP waits for this callback, so neither its context nor the FDO
 * can disappear while HAL has the request queued. No hardware polling here.
 * This exclusively assigned ISA channel is retained until STOP_DEVICE; the
 * DMA transfer itself is unmapped whenever playback stops. */
static IO_ALLOCATION_ACTION NTAPI Allocated(PDEVICE_OBJECT dev, PIRP irp, PVOID map, PVOID ctx)
{
    CS5530 *s = ctx;
    (void)dev; (void)irp;
    s->MapBase = map;
    s->Channel = TRUE;
    KeSetEvent(&s->Allocated, IO_NO_INCREMENT, FALSE);
    return KeepObject;
}

static void Stop(CARD *c)
{
    CS5530 *s = c->Geode;
    if (!s) return;
    if (s->Mapped) {
        /* Reset terminates auto-init immediately, unlike D9 (end of block). */
        if (!Reset(s)) LOGE("cs5530: DSP did not acknowledge stop/reset");
        s->Adapter->DmaOperations->FlushAdapterBuffers(s->Adapter, s->Mdl, s->MapBase,
                                                      s->Buffer, RING_BYTES, TRUE);
        s->Mapped = FALSE;
    }
}

static void Shutdown(CARD *c)
{
    CS5530 *s = c->Geode;
    KIRQL old;
    if (!s) return;
    Stop(c);
    if (s->DspReady) {
        MixWrite(s, 0x30, 0); MixWrite(s, 0x31, 0);
        Command(s, 0xd3);
    }
    if (s->Channel) {
        KeRaiseIrql(DISPATCH_LEVEL, &old);
        s->Adapter->DmaOperations->FreeAdapterChannel(s->Adapter);
        KeLowerIrql(old);
    }
    if (s->Mdl) IoFreeMdl(s->Mdl);
    if (s->Buffer) s->Adapter->DmaOperations->FreeCommonBuffer(s->Adapter, RING_BYTES,
                                                             s->Logical, s->Buffer, FALSE);
    if (s->Adapter) s->Adapter->DmaOperations->PutDmaAdapter(s->Adapter);
    memset(&c->PlayRing, 0, sizeof(c->PlayRing)); /* not an OsAllocDma allocation */
    OsFree(s); c->Geode = NULL;
}

static NTSTATUS Init(CARD *c)
{
    CS5530 *s = c->Geode;
    DEVICE_DESCRIPTION dd;
    ULONG maps, i;
    KIRQL old;
    NTSTATUS st;
    if (!s || (g_NtVersion < 0x0501 || !OsResolve(0, "KeFlushQueuedDpcs"))) return STATUS_NOT_SUPPORTED;
    if (!Reset(s) || !Command(s, 0xe1) || !Response(s, &s->Major) ||
        !Response(s, &s->Minor) || s->Major != 4) return STATUS_DEVICE_CONFIGURATION_ERROR;
    s->DspReady = TRUE;
    s->IrqMask = MixRead(s, 0x80);
    s->DmaMask = MixRead(s, 0x81);
    i = s->R.Irq == 9 ? 1 : s->R.Irq == 5 ? 2 : s->R.Irq == 7 ? 4 : 8;
    if ((s->IrqMask & 15) != i || (s->DmaMask & 0xeb) != ((1U << s->R.Dma8) | (1U << s->R.Dma16))) {
        LOGE("cs5530: firmware/PnP mismatch: mixer IRQ=%02x DMA=%02x", s->IrqMask, s->DmaMask);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    memset(&dd, 0, sizeof(dd));
    dd.Version = DEVICE_DESCRIPTION_VERSION;
    dd.InterfaceType = Isa;
    dd.BusNumber = s->R.BusNumber;
    dd.DmaChannel = s->R.Dma16;
    dd.DmaWidth = Width16Bits;
    dd.DmaSpeed = Compatible;
    dd.AutoInitialize = TRUE;
    dd.MaximumLength = RING_BYTES;
    s->Adapter = IoGetDmaAdapter(s->Pdo, &dd, &maps);
    if (!s->Adapter) return STATUS_INSUFFICIENT_RESOURCES;
    s->AvailableMaps = maps;
    s->Buffer = s->Adapter->DmaOperations->AllocateCommonBuffer(s->Adapter, RING_BYTES, &s->Logical, FALSE);
    if (!s->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    /* 16-bit ISA: below 16 MiB, word aligned, no 128 KiB boundary crossing. */
    if (s->Logical.QuadPart < 0 || s->Logical.QuadPart > 0x1000000 - RING_BYTES ||
        (s->Logical.LowPart & 1) || (s->Logical.LowPart & 0x1ffff) + RING_BYTES > 0x20000)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    s->Mdl = IoAllocateMdl(s->Buffer, RING_BYTES, FALSE, FALSE, NULL);
    if (!s->Mdl) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(s->Mdl);
    s->MapRegisters = ADDRESS_AND_SIZE_TO_SPAN_PAGES(s->Buffer, RING_BYTES);
    if (s->MapRegisters > s->AvailableMaps) {
        LOGE("cs5530: ring needs %u map registers, HAL offers %u", s->MapRegisters, s->AvailableMaps);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&s->Allocated, NotificationEvent, FALSE);
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    /* System DMA needs map registers even with a physically contiguous common
       buffer. A NULL MapRegisterBase can select HAL's address-only fast path,
       leaving the ISA controller unprogrammed. Retain the channel and its
       registers together until shutdown; FlushAdapterBuffers resets each use. */
    st = s->Adapter->DmaOperations->AllocateAdapterChannel(s->Adapter, s->Fdo,
                                                          s->MapRegisters, Allocated, s);
    KeLowerIrql(old);
    if (!NT_SUCCESS(st)) return st;
    KeWaitForSingleObject(&s->Allocated, Executive, KernelMode, FALSE, NULL);
    if (!s->MapBase) {
        LOGE("cs5530: HAL returned no map-register handle for system DMA");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    LOGI("cs5530: DMA setup revision 2, map registers %u/%u, base=%p",
         s->MapRegisters, s->AvailableMaps, s->MapBase);
    c->PlayRing.Va = s->Buffer;
    c->PlayRing.Pa = s->Logical.LowPart;
    c->PlayRing.Size = RING_BYTES;
    memset(s->Buffer, 0, RING_BYTES);
    for (i = 0; i < LINE_COUNT; i++) c->Lines[i].Present = FALSE;
    c->Lines[LINE_MASTER].Present = c->Lines[LINE_WAVE].Present = TRUE;
    c->Lines[LINE_MASTER].HwVolume = c->Lines[LINE_WAVE].HwVolume = TRUE;
    /* Disable analog input monitoring; PCM is mixed into the output internally. */
    MixWrite(s, 0x3c, 0);
    MixWrite(s, 0x3d, 0); MixWrite(s, 0x3e, 0);
    OsSprintf(c->Codec, sizeof(c->Codec), "VSA1 SB16 DSP %u.%02u", s->Major, s->Minor);
    LOGI("cs5530: %s, ring %08x/%u, IRQ %u DMA %u, 44100 Hz S16 stereo", c->Codec,
         c->PlayRing.Pa, RING_BYTES, s->R.Irq, s->R.Dma16);
    return STATUS_SUCCESS;
}

static BOOLEAN Start(CARD *c)
{
    CS5530 *s = c->Geode;
    ULONG n = RING_BYTES, count = PERIOD_BYTES / 2 - 1;
    if (!s || !s->Channel || s->Mapped) return FALSE;
    s->DmaBeforeMap = s->Adapter->DmaOperations->ReadDmaCounter(s->Adapter);
    s->Adapter->DmaOperations->MapTransfer(s->Adapter, s->Mdl, s->MapBase, s->Buffer, &n, TRUE);
    s->Mapped = TRUE;
    s->DmaAfterMap = s->Adapter->DmaOperations->ReadDmaCounter(s->Adapter);
    LOGI("cs5530: DMA map bytes=%u count_before=%u count_after=%u",
         n, s->DmaBeforeMap, s->DmaAfterMap);
    if (n != RING_BYTES ||
        !Command(s, 0x41) || !Command(s, 44100 >> 8) || !Command(s, 44100 & 255) ||
        !Command(s, 0x42) || !Command(s, 44100 >> 8) || !Command(s, 44100 & 255) ||
        !Command(s, 0xd1) || !Command(s, 0xb6) || !Command(s, 0x30) ||
        !Command(s, count & 255) || !Command(s, count >> 8)) {
        LOGE("cs5530: start failed (mapped %u bytes, DSP errors %u)", n, s->Errors);
        Stop(c); return FALSE;
    }
    return TRUE;
}
static ULONG Position(CARD *c)
{
    CS5530 *s = c->Geode;
    ULONG n;
    if (!s || !s->Mapped) return 0;
    n = s->Adapter->DmaOperations->ReadDmaCounter(s->Adapter);
    s->DmaRemaining = n;
    if (n > RING_BYTES) { s->Errors++; return c->LastPlayPos; }
    return ((RING_BYTES - n) % RING_BYTES) / ENG_FRAME_BYTES;
}
static BOOLEAN Isr(CARD *c)
{
    CS5530 *s = c->Geode;
    UCHAR v = MixRead(s, 0x82);
    if (!(v & 3)) return FALSE;
    if (v & 1) (void)Rd(s, 0xe);
    if (v & 2) (void)Rd(s, 0xf);
    return TRUE;
}
typedef struct { CS5530 *S; UCHAR Reg, L, R; } VOLUME;
static BOOLEAN NTAPI Volume(PVOID ctx)
{
    VOLUME *v = ctx;
    MixWrite(v->S, v->Reg, v->L); MixWrite(v->S, v->Reg + 1, v->R);
    return TRUE;
}
static void ApplyLine(CARD *c, ULONG line)
{
    LINE_STATE *l;
    VOLUME v;
    if (line != LINE_MASTER && line != LINE_WAVE) return;
    l = &c->Lines[line];
    v.S = c->Geode; v.Reg = line == LINE_MASTER ? 0x30 : 0x32;
    v.L = l->Mute ? 0 : (UCHAR)((l->Left >> 11) << 3);
    v.R = l->Mute ? 0 : (UCHAR)((l->Right >> 11) << 3);
    /* Serialize indexed mixer writes with ISR's mixer status read. */
    if (c->Interrupt) KeSynchronizeExecution(c->Interrupt, Volume, &v);
    else Volume(&v);
}
static ULONG Dump(CARD *c, char *b, ULONG n)
{
    CS5530 *s = c->Geode;
    ULONG pos = Position(c);
    return OsSprintf(b, n, "CS5530 VSA1: SB=%x FM=%x IRQ=%u DMA8=%u DMA16=%u DSP=%u.%02u\r\n"
        "ring=%08x bytes=%u rate=%u mapped=%u pos=%u irq_count=%u errors=%u\r\n"
        "dma_setup=2 maps=%u/%u map_base=%p count_before=%u count_after=%u remaining=%u\r\n",
        s->R.SbBase, s->R.FmBase, s->R.Irq, s->R.Dma8, s->R.Dma16, s->Major, s->Minor,
        c->PlayRing.Pa, RING_BYTES, c->EngineRate, s->Mapped, pos, c->IntCount, s->Errors,
        s->MapRegisters, s->AvailableMaps, s->MapBase, s->DmaBeforeMap, s->DmaAfterMap, s->DmaRemaining);
}
const HWOPS g_Cs5530Ops = {
    .Name = "CS5530 VSA1", .Init = Init, .Shutdown = Shutdown,
    .StartPlay = Start, .StopPlay = Stop, .PlayPos = Position,
    .Isr = Isr, .ApplyLine = ApplyLine, .Dump = Dump
};
NTSTATUS Cs5530Attach(CARD *c, PDEVICE_OBJECT pdo, PDEVICE_OBJECT fdo, const GEODE_RES *r)
{
    CS5530 *s = OsAlloc(sizeof(*s));
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    s->R = *r; s->Pdo = pdo; s->Fdo = fdo;
    c->Geode = s; c->Type = HW_CS5530; c->Ops = &g_Cs5530Ops;
    c->EngineRate = 44100; c->NoSynth = TRUE; c->RequireInterrupt = TRUE; c->BackendBuffers = TRUE;
    c->CanRecord = FALSE; c->Int = r->Int; c->Irq = (UCHAR)r->Irq;
    OsSprintf(c->Desc, sizeof(c->Desc), "Geode CS5530 XpressAudio (VSA1)");
    return STATUS_SUCCESS;
}
#elif defined(AUDIONT_WDM)
NTSTATUS Cs5530Attach(CARD *c, PDEVICE_OBJECT pdo, PDEVICE_OBJECT fdo, const GEODE_RES *r)
{ (void)c; (void)pdo; (void)fdo; (void)r; return STATUS_NOT_SUPPORTED; }
#endif
