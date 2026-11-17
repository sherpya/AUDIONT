/* Runs as a user-mode test under Wine; uses the real DDK resource structures
 * and the production parser, with no mocked layouts and no port I/O. */
#include "audiont.h"
#include "geode.h"
#include <stdio.h>
#include <assert.h>

void OsLog(ULONG level, const char *fmt, ...) { (void)level; (void)fmt; }
typedef union { ULONGLONG Align; UCHAR Bytes[1024]; } RESBUF;
static PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor(RESBUF *buf, ULONG i)
{
    return (PCM_PARTIAL_RESOURCE_DESCRIPTOR)(buf->Bytes +
        FIELD_OFFSET(CM_RESOURCE_LIST, List) +
        FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR, PartialResourceList) +
        FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors) + i * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
}
static void Sample(RESBUF *buf, BOOLEAN translated)
{
    PCM_RESOURCE_LIST l = (PCM_RESOURCE_LIST)buf;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;
    memset(buf, 0, sizeof(*buf));
    l->Count = 1;
    l->List[0].InterfaceType = Isa;
    l->List[0].PartialResourceList.Count = 5;
    d = Descriptor(buf, 0); d->Type = CmResourceTypeDma; d->u.Dma.Channel = 5;
    d = Descriptor(buf, 1); d->Type = CmResourceTypePort; d->Flags = CM_RESOURCE_PORT_IO;
    d->u.Port.Start.QuadPart = 0x388; d->u.Port.Length = 4;
    d = Descriptor(buf, 2); d->Type = CmResourceTypeInterrupt; d->Flags = CM_RESOURCE_INTERRUPT_LATCHED;
    d->u.Interrupt.Level = translated ? 24 : 5;
    d->u.Interrupt.Vector = translated ? 0x35 : 5; d->u.Interrupt.Affinity = 1;
    d = Descriptor(buf, 3); d->Type = CmResourceTypePort; d->Flags = CM_RESOURCE_PORT_IO;
    d->u.Port.Start.QuadPart = 0x220; d->u.Port.Length = 16;
    d = Descriptor(buf, 4); d->Type = CmResourceTypeDma; d->u.Dma.Channel = 1;
}
int main(void)
{
    RESBUF a, b;
    GEODE_RES r;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d, e;
    CM_PARTIAL_RESOURCE_DESCRIPTOR tmp;
    PCM_RESOURCE_LIST ra = (PCM_RESOURCE_LIST)&a, rb = (PCM_RESOURCE_LIST)&b;
    Sample(&a, FALSE); Sample(&b, TRUE);
    /* Reordered descriptors in translated resources must still match. */
    d = Descriptor(&b, 0); e = Descriptor(&b, 3); tmp = *d; *d = *e; *e = tmp;
    assert(NT_SUCCESS(GeodeResources(ra, rb, &r)));
    assert(r.SbBase == 0x220 && r.FmBase == 0x388 && r.Dma8 == 1 && r.Dma16 == 5 &&
           r.Irq == 5 && r.Int.Vector == 0x35 && r.Int.Irql == 24 && r.Int.Mode == Latched);
    /* Real PCM has two ports, NOT the three expected by the crashing NSC driver. */
    Sample(&b, TRUE);
    Descriptor(&b, 4)->u.Dma.Channel = 5;
    assert(!NT_SUCCESS(GeodeResources(ra, rb, &r))); /* duplicate DMA */
    Sample(&b, TRUE); rb->List[0].PartialResourceList.Count = 4;
    assert(!NT_SUCCESS(GeodeResources(ra, rb, &r))); /* missing DMA */
    Sample(&b, TRUE); Descriptor(&b, 2)->Type = CmResourceTypeNull;
    assert(!NT_SUCCESS(GeodeResources(ra, rb, &r))); /* missing interrupt */
    Sample(&b, TRUE); Descriptor(&b, 3)->u.Port.Start.QuadPart = 0x240;
    assert(!NT_SUCCESS(GeodeResources(ra, rb, &r))); /* mismatched translation */
    Sample(&b, TRUE); Descriptor(&b, 3)->u.Port.Start.QuadPart = -1;
    assert(!NT_SUCCESS(GeodeResources(ra, rb, &r)));
    Sample(&b, TRUE); rb->List[0].PartialResourceList.Count = 65;
    assert(!NT_SUCCESS(GeodeResources(ra, rb, &r)));
    assert(!NT_SUCCESS(GeodeResources(NULL, rb, &r)));
    Sample(&b, TRUE); Descriptor(&b, 4)->u.Dma.Channel = 4;
    assert(!NT_SUCCESS(GeodeResources(ra, rb, &r))); /* cascade is never a DMA device */
    Sample(&a, FALSE); Sample(&b, TRUE);
    /* Optional MPU port is accepted but neither required nor accessed. */
    ra->List[0].PartialResourceList.Count = rb->List[0].PartialResourceList.Count = 6;
    d = Descriptor(&a, 5); d->Type = CmResourceTypePort; d->Flags = CM_RESOURCE_PORT_IO;
    d->u.Port.Start.QuadPart = 0x330; d->u.Port.Length = 2; *Descriptor(&b, 5) = *d;
    assert(NT_SUCCESS(GeodeResources(ra, rb, &r)));
    assert(GeodeHardwareId(L"UNKNOWN\0*crx0002\0", 19));
    assert(GeodeHardwareId(L"ISAPNP\\CRX0002_DEV0000\0", 23) == FALSE);
    assert(!GeodeHardwareId(L"*CRX0002BAD\0", 13));
    assert(!GeodeHardwareId(L"*CRX0002", 8)); /* missing NUL within bound */
    assert(GeodePciId(L"PCI\\VEN_1022&DEV_2093&SUBSYS_12345678\0", 38));
    assert(!GeodePciId(L"PCI\\VEN_1022&DEV_20930\0", 23));
    Sample(&b, TRUE);
    rb->List[0].PartialResourceList.Count = 2;
    *Descriptor(&b, 0) = *Descriptor(&b, 3);
    *Descriptor(&b, 1) = *Descriptor(&b, 2);
    Descriptor(&b, 0)->u.Port.Start.QuadPart = 0x4000;
    Descriptor(&b, 0)->u.Port.Length = 0x80;
    assert(GeodePciResources(rb, 0x4001));
    assert(!GeodePciResources(rb, 0x4081));
    Descriptor(&b, 0)->u.Port.Length = 0x40;
    assert(!GeodePciResources(rb, 0x4001));
    /* Linutop 2 / XP: I/O, opaque bus-private data, shared interrupt.
     * R2 rejected type 129 before the CS5536 backend could start. */
    Descriptor(&b, 0)->u.Port.Length = 0x80;
    Descriptor(&b, 0)->Flags = CM_RESOURCE_PORT_IO |
        CM_RESOURCE_PORT_16_BIT_DECODE | CM_RESOURCE_PORT_POSITIVE_DECODE;
    rb->List[0].InterfaceType = PCIBus;
    rb->List[0].PartialResourceList.Count = 3;
    *Descriptor(&b, 2) = *Descriptor(&b, 1);
    Descriptor(&b, 2)->u.Interrupt.Vector = 59;
    Descriptor(&b, 2)->u.Interrupt.Level = 16;
    Descriptor(&b, 2)->Flags = 0;
    Descriptor(&b, 2)->ShareDisposition = CmResourceShareShared;
    d = Descriptor(&b, 1); memset(d, 0, sizeof(*d));
    d->Type = CmResourceTypeDevicePrivate;
    d->u.DevicePrivate.Data[0] = 0x12345678;
    d->u.DevicePrivate.Data[1] = 0xffffffff;
    d->u.DevicePrivate.Data[2] = 4;
    assert(GeodePciResources(rb, 0x4001));
    /* Private data may appear before or after the actual resources. */
    tmp = *Descriptor(&b, 0); *Descriptor(&b, 0) = *d; *d = tmp;
    assert(GeodePciResources(rb, 0x4001));
    tmp = *Descriptor(&b, 2); *Descriptor(&b, 2) = *Descriptor(&b, 0);
    *Descriptor(&b, 0) = tmp;
    assert(GeodePciResources(rb, 0x4001));
    /* Opaque data must not substitute for missing ports or interrupts. */
    tmp = *Descriptor(&b, 0); *Descriptor(&b, 0) = *Descriptor(&b, 2);
    assert(!GeodePciResources(rb, 0x4001));
    *Descriptor(&b, 0) = tmp;
    tmp = *Descriptor(&b, 1); *Descriptor(&b, 1) = *Descriptor(&b, 2);
    assert(!GeodePciResources(rb, 0x4001));
    *Descriptor(&b, 1) = tmp;
    assert(!GeodePciResources(rb, 0x4081));
    /* Duplicate hardware resources and other descriptor types still fail. */
    rb->List[0].PartialResourceList.Count = 4;
    *Descriptor(&b, 3) = *Descriptor(&b, 0);
    assert(!GeodePciResources(rb, 0x4001));
    *Descriptor(&b, 3) = *Descriptor(&b, 1);
    assert(!GeodePciResources(rb, 0x4001));
    Descriptor(&b, 3)->Type = CmResourceTypeMemory;
    assert(!GeodePciResources(rb, 0x4001));
    Descriptor(&b, 3)->Type = CmResourceTypeDeviceSpecific;
    assert(!GeodePciResources(rb, 0x4001));
    puts("RESULT tgeode_res PASS (WDM resources, hardware IDs, CS5536 bus-private descriptors)");
    return 0;
}
