/* SPDX-License-Identifier: GPL-3.0-or-later
 * BIOS PnP resources: no fixed descriptor order and no compulsory MPU port.
 */
#include "audiont.h"
#include "geode.h"
#ifdef AUDIONT_WDM

typedef struct {
    ULONG Ports, Irqs, Dmas, SeenDma;
    GEODE_RES R;
} RESOURCE_SCAN;

static NTSTATUS Scan(PCM_RESOURCE_LIST list, BOOLEAN translated, RESOURCE_SCAN *s)
{
    ULONG i, j;
    PUCHAR p;
    if (!list || !list->Count || list->Count > 8) return STATUS_DEVICE_CONFIGURATION_ERROR;
    memset(s, 0, sizeof(*s));
    s->R.Dma8 = s->R.Dma16 = 0xffffffff;
    p = (PUCHAR)list + FIELD_OFFSET(CM_RESOURCE_LIST, List);
    for (i = 0; i < list->Count; i++) {
        PCM_FULL_RESOURCE_DESCRIPTOR f = (PCM_FULL_RESOURCE_DESCRIPTOR)p;
        ULONG n = f->PartialResourceList.Count;
        PUCHAR dp = (PUCHAR)&f->PartialResourceList + FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
        if (!n || n > 64) return STATUS_DEVICE_CONFIGURATION_ERROR;
        s->R.BusNumber = f->BusNumber;
        for (j = 0; j < n; j++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)(dp + j * sizeof(*d));
            LOGV("geode: %s resource %u.%u type %u flags %x", translated ? "translated" : "raw", i, j, d->Type, d->Flags);
            switch (d->Type) {
            case CmResourceTypePort:
                if (!(d->Flags & CM_RESOURCE_PORT_IO) || d->u.Port.Start.QuadPart < 0 || d->u.Port.Start.QuadPart > 0xffff ||
                    !d->u.Port.Length || d->u.Port.Length > 0x10000 - d->u.Port.Start.LowPart)
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                if (d->u.Port.Length == 16) {
                    if (s->R.SbBase || d->u.Port.Start.LowPart < 0x200) return STATUS_DEVICE_CONFIGURATION_ERROR;
                    s->R.SbBase = (USHORT)d->u.Port.Start.LowPart;
                } else if (d->u.Port.Length == 4) {
                    if (s->R.FmBase) return STATUS_DEVICE_CONFIGURATION_ERROR;
                    s->R.FmBase = (USHORT)d->u.Port.Start.LowPart;
                } else if (d->u.Port.Length != 2) return STATUS_DEVICE_CONFIGURATION_ERROR;
                s->Ports++;
                break;
            case CmResourceTypeInterrupt:
                if (++s->Irqs != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
                s->R.Irq = d->u.Interrupt.Level;
                s->R.Int.Valid = TRUE;
                s->R.Int.Vector = d->u.Interrupt.Vector;
                s->R.Int.Irql = (KIRQL)d->u.Interrupt.Level;
                s->R.Int.Affinity = d->u.Interrupt.Affinity;
                s->R.Int.Shared = d->ShareDisposition == CmResourceShareShared;
                s->R.Int.Mode = (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
                break;
            case CmResourceTypeDma: {
                ULONG ch = d->u.Dma.Channel;
                if (ch > 7 || ch == 2 || ch == 4 || (s->SeenDma & (1UL << ch)))
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                s->SeenDma |= 1UL << ch;
                s->Dmas++;
                if (ch < 4) {
                    if (s->R.Dma8 != 0xffffffff) return STATUS_DEVICE_CONFIGURATION_ERROR;
                    s->R.Dma8 = ch;
                } else {
                    if (s->R.Dma16 != 0xffffffff) return STATUS_DEVICE_CONFIGURATION_ERROR;
                    s->R.Dma16 = ch;
                }
                break;
            }
            /* Device-specific trailing data needs a variable-sized walk; this device
             * has none. Fail instead of indexing beyond the OS's descriptor list. */
            default: return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        }
        p = dp + n * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    }
    if (!s->R.SbBase || s->Irqs != 1 || s->Dmas != 2 || s->Ports > 3 ||
        s->R.Dma8 == 0xffffffff || s->R.Dma16 == 0xffffffff)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

NTSTATUS GeodeResources(PCM_RESOURCE_LIST raw, PCM_RESOURCE_LIST translated, GEODE_RES *r)
{
    RESOURCE_SCAN a, b;
    NTSTATUS st;
    memset(r, 0, sizeof(*r));
    st = Scan(raw, FALSE, &a);
    if (!NT_SUCCESS(st)) return st;
    st = Scan(translated, TRUE, &b);
    if (!NT_SUCCESS(st)) return st;
    if (a.R.SbBase != b.R.SbBase || a.R.FmBase != b.R.FmBase ||
        a.R.Dma8 != b.R.Dma8 || a.R.Dma16 != b.R.Dma16 ||
        !(a.R.Irq == 5 || a.R.Irq == 7 || a.R.Irq == 9 || a.R.Irq == 10) ||
        !b.R.Int.Vector || !b.R.Int.Affinity)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    *r = a.R;
    r->Int = b.R.Int;
    LOGI("geode: SB %x FM %x IRQ %u DMA8 %u DMA16 %u vector %u", r->SbBase, r->FmBase,
         r->Irq, r->Dma8, r->Dma16, r->Int.Vector);
    return STATUS_SUCCESS;
}

/* Bound the entire REG_MULTI_SZ, including truncated/malformed properties.
 * ACPI/ISAPNP enumerators may prefix the EISA identifier differently. */
static BOOLEAN MatchId(const WCHAR *ids, ULONG chars, const char *want, BOOLEAN prefix)
{
    ULONG off = 0, j, n;
    while (off < chars && ids[off]) {
        for (n = 0; off + n < chars && ids[off + n]; n++) ;
        if (off + n == chars) return FALSE;
        for (j = 0; j < n && want[j]; j++) {
            WCHAR ch = ids[off + j];
            if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
            if (ch != (WCHAR)want[j]) break;
        }
        if (!want[j] && (j == n || (prefix && ids[off + j] == '&'))) return TRUE;
        off += n + 1;
    }
    return FALSE;
}
BOOLEAN GeodeHardwareId(const WCHAR *ids, ULONG chars)
{
    return MatchId(ids, chars, "*CRX0002", FALSE) ||
           MatchId(ids, chars, "ACPI\\CRX0002", FALSE) ||
           MatchId(ids, chars, "ISAPNP\\CRX0002", TRUE);
}
BOOLEAN GeodePciId(const WCHAR *ids, ULONG chars)
{ return MatchId(ids, chars, "PCI\\VEN_1022&DEV_2093", TRUE); }

BOOLEAN GeodePciResources(PCM_RESOURCE_LIST list, ULONG bar)
{
    ULONG i, j, ports = 0, irqs = 0;
    PUCHAR p;
    if (!list || !list->Count || list->Count > 8 || !(bar & 1) ||
        (bar & ~3UL) < 0x100 || (bar & ~3UL) > 0xff80) return FALSE;
    p = (PUCHAR)list + FIELD_OFFSET(CM_RESOURCE_LIST, List);
    for (i = 0; i < list->Count; i++) {
        PCM_FULL_RESOURCE_DESCRIPTOR f = (PCM_FULL_RESOURCE_DESCRIPTOR)p;
        ULONG n = f->PartialResourceList.Count;
        PUCHAR dp = (PUCHAR)&f->PartialResourceList + FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
        if (!n || n > 64) return FALSE;
        for (j = 0; j < n; j++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)(dp + j * sizeof(*d));
            if (d->Type == CmResourceTypePort) {
                if (!(d->Flags & CM_RESOURCE_PORT_IO) || d->u.Port.Start.QuadPart != (bar & ~3UL) ||
                    d->u.Port.Length < 0x80 || d->u.Port.Length > 0x10000 - (bar & ~3UL)) return FALSE;
                ports++;
            } else if (d->Type == CmResourceTypeInterrupt) {
                if (!d->u.Interrupt.Vector || !d->u.Interrupt.Affinity) return FALSE;
                irqs++;
            } else if (d->Type == CmResourceTypeDevicePrivate) {
                /* Bus-private metadata is not an additional hardware resource. */
                LOGV("geode: PCI private resource %u.%u ignored", i, j);
            } else return FALSE;
        }
        p = dp + n * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    }
    return ports == 1 && irqs == 1;
}
#endif
