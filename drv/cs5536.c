/* SPDX-License-Identifier: GPL-3.0-or-later
 * CS5536 native PCI AC'97 playback. Controller/PRD format: AMD 33238G.
 * No ISA emulation, no VSA virtual-register writes, no proprietary code.
 */
#include "audiont.h"
#include "geode.h"
#if defined(AUDIONT_WDM) && defined(_X86_)
#include "ac97.h"
#include "geode_dma.h"

typedef struct { ULONG Errors, BadPos; UCHAR LastStatus; BOOLEAN Ready; } CS5536;
static UCHAR R8(CARD *c, ULONG r) { return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(c->IoBase0 + r)); }
static USHORT R16(CARD *c, ULONG r) { return READ_PORT_USHORT((PUSHORT)(ULONG_PTR)(c->IoBase0 + r)); }
static ULONG R32(CARD *c, ULONG r) { return READ_PORT_ULONG((PULONG)(ULONG_PTR)(c->IoBase0 + r)); }
static void W8(CARD *c, ULONG r, UCHAR v) { WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)(c->IoBase0 + r), v); }
static void W32(CARD *c, ULONG r, ULONG v) { WRITE_PORT_ULONG((PULONG)(ULONG_PTR)(c->IoBase0 + r), v); }

static BOOLEAN CodecWait(CARD *c)
{
    ULONG i;
    for (i = 0; i < 100; i++) {
        if (!(R32(c, 0x0c) & 0x00010000)) return TRUE;
        OsStallUs(1);
    }
    ((CS5536 *)c->Geode)->Errors++;
    return FALSE;
}
static USHORT CodecRead(CARD *c, ULONG reg)
{
    ULONG i, v;
    if (!CodecWait(c)) return 0xffff;
    (void)R32(c, 0x08); /* discard stale status before requesting a new value */
    W32(c, 0x0c, (reg << 24) | 0x80010000);
    if (!CodecWait(c)) return 0xffff;
    for (i = 0; i < 100; i++) {
        v = R32(c, 0x08);
        if ((v & 0x00020000) && (v >> 24) == reg) return (USHORT)v;
        OsStallUs(1);
    }
    ((CS5536 *)c->Geode)->Errors++;
    return 0xffff;
}
static void CodecWrite(CARD *c, ULONG reg, USHORT v)
{
    if (!CodecWait(c)) return;
    W32(c, 0x0c, (reg << 24) | 0x00010000 | v);
    CodecWait(c);
}
static void Stop(CARD *c)
{
    CS5536 *s = c->Geode;
    if (!s || !s->Ready) return;
    W8(c, 0x20, 0);
    (void)R8(c, 0x20); /* posted I/O completion before buffer reuse/free */
}
static void Shutdown(CARD *c)
{
    CS5536 *s = c->Geode;
    if (!s) return;
    Stop(c);
    if (s->Ready) {
        W8(c, 0x28, 0); /* capture is never exposed */
        (void)R8(c, 0x21); (void)R8(c, 0x29);
        CodecWrite(c, AC97_MASTER, 0x8000);
        W32(c, 0x24, 0);
    }
    OsFree(s); c->Geode = NULL;
}
static NTSTATUS Init(CARD *c)
{
    CS5536 *s;
    NTSTATUS st;
    ULONG i;
    if (!c->Int.Valid || (g_NtVersion < 0x0501 || !OsResolve(0, "KeFlushQueuedDpcs")) || !(c->Bar[0] & 1) ||
        (c->Bar[0] & ~3UL) < 0x100 || (c->Bar[0] & ~3UL) > 0xff80)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    s = OsAlloc(sizeof(*s)); c->Geode = s;
    c->Ac97 = OsAlloc(sizeof(*c->Ac97));
    if (!s || !c->Ac97) return STATUS_INSUFFICIENT_RESOURCES;
    c->IoBase0 = (USHORT)(c->Bar[0] & ~3UL);
    c->Ac97->CodecRead = CodecRead; c->Ac97->CodecWrite = CodecWrite;
    s->Ready = TRUE;
    W8(c, 0x20, 0); W8(c, 0x28, 0);
    (void)R8(c, 0x21); (void)R8(c, 0x29);
    /* The BIOS brings up AC-link. Try a warm link reset if it is not ready. */
    if (!(R32(c, 0x08) & 0x00800000)) {
        W32(c, 0x0c, 0x00020000);
        for (i = 0; i < 100; i++) {
            if (R32(c, 0x08) & 0x00800000) break;
            OsSleepMs(1);
        }
        if (i == 100) return STATUS_IO_TIMEOUT;
    }
    st = Ac97CodecInit(c);
    if (!NT_SUCCESS(st) || s->Errors || !c->Ac97->VendorId || c->Ac97->VendorId == 0xffffffff)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!OsAllocDma(&c->Aux, 4096, c->DmaUncached)) return STATUS_INSUFFICIENT_RESOURCES;
    if (!GeodeBuildPrd(c->Aux.Va, c->Aux.Size / sizeof(GEODE_PRD), c->Aux.Pa,
                       c->PlayRing.Pa, RING_PERIODS, PERIOD_BYTES)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    /* Only the implemented playback controls are advertised. Mute unused inputs. */
    for (i = 0; i < LINE_COUNT; i++) c->Lines[i].Present = FALSE;
    c->Lines[LINE_MASTER].Present = c->Lines[LINE_WAVE].Present = TRUE;
    c->RecSrc = c->RecSrcMask = 0;
    CodecWrite(c, AC97_CD, 0x8808); CodecWrite(c, AC97_LINE, 0x8808);
    CodecWrite(c, AC97_MIC, 0x8000); CodecWrite(c, AC97_AUX, 0x8808);
    CodecWrite(c, AC97_PC_BEEP, 0x8000);
    LOGI("cs5536: IO %x PRD %08x ring %08x, 48000 Hz S16 stereo", c->IoBase0, c->Aux.Pa, c->PlayRing.Pa);
    return s->Errors ? STATUS_DEVICE_CONFIGURATION_ERROR : STATUS_SUCCESS;
}
static BOOLEAN Start(CARD *c)
{
    CS5536 *s = c->Geode;
    if (!s || !s->Ready || s->Errors) return FALSE;
    Stop(c);
    (void)R8(c, 0x21);
    /* Start at the jump descriptor, as specified for a circular PRD list. */
    W32(c, 0x24, c->Aux.Pa + RING_PERIODS * sizeof(GEODE_PRD));
    W8(c, 0x20, 1);
    return (R8(c, 0x20) & 1) != 0;
}
static ULONG Position(CARD *c)
{
    CS5536 *s = c->Geode;
    ULONG pos;
    if (!GeodeDmaPosition(R32(c, 0x60), c->PlayRing.Pa, RING_BYTES, &pos)) {
        s->BadPos++; return c->LastPlayPos;
    }
    return pos;
}
static BOOLEAN Isr(CARD *c)
{
    CS5536 *s = c->Geode;
    USHORT irq = R16(c, 0x12);
    if (!(irq & 0xf)) return FALSE;
    if (irq & 4) {
        s->LastStatus = R8(c, 0x21); /* read clears period IRQ */
        if (s->LastStatus & 2) s->Errors++;
    }
    if (irq & 8) (void)R8(c, 0x29);
    if (irq & 3) (void)R32(c, 0);
    return TRUE;
}
static ULONG Dump(CARD *c, char *b, ULONG n)
{
    CS5536 *s = c->Geode;
    /* Do not read read-to-clear IRQ/status registers from diagnostics. */
    return OsSprintf(b, n, "CS5536: IO=%x PRD=%08x ring=%08x rate=%u pos=%u\r\n"
        "irq_count=%u last_bm_status=%02x errors=%u invalid_positions=%u codec=%08x\r\n",
        c->IoBase0, c->Aux.Pa, c->PlayRing.Pa, c->EngineRate, Position(c),
        c->IntCount, s->LastStatus, s->Errors, s->BadPos, c->Ac97->VendorId);
}
static const HWOPS Ops = {
    .Name = "CS5536 AC97", .Init = Init, .Shutdown = Shutdown,
    .StartPlay = Start, .StopPlay = Stop, .PlayPos = Position,
    .Isr = Isr, .ApplyLine = Ac97ApplyLine, .Dump = Dump
};
BOOLEAN Cs5536Probe(CARD *c)
{
    if (c->VendorId != 0x1022 || c->DeviceId != 0x2093) return FALSE;
    c->Type = HW_CS5536; c->Ops = &Ops; c->CanRecord = FALSE;
    c->NoSynth = TRUE; c->RequireInterrupt = TRUE; c->EngineRate = 48000;
    OsSprintf(c->Desc, sizeof(c->Desc), "AMD Geode CS5536 AC'97");
    return TRUE;
}
#else
BOOLEAN Cs5536Probe(CARD *c) { (void)c; return FALSE; }
#endif
