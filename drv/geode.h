/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AUDIONT_GEODE_H
#define AUDIONT_GEODE_H

/* The CS5530 VSA1 device belongs to the BIOS PnP node, not the PCI node. */
#ifdef AUDIONT_WDM
typedef struct _GEODE_RES {
    USHORT SbBase, FmBase;
    ULONG Irq, Dma8, Dma16, BusNumber;
    CARD_INT Int;
} GEODE_RES;
NTSTATUS GeodeResources(PCM_RESOURCE_LIST raw, PCM_RESOURCE_LIST translated, GEODE_RES *r);
BOOLEAN GeodePciResources(PCM_RESOURCE_LIST list, ULONG bar);
BOOLEAN GeodeHardwareId(const WCHAR *ids, ULONG chars);
BOOLEAN GeodePciId(const WCHAR *ids, ULONG chars);
NTSTATUS CardProbeCs5530(PDEVICE_OBJECT pdo, PDEVICE_OBJECT fdo, const GEODE_RES *r, CARD **out);
NTSTATUS Cs5530Attach(CARD *c, PDEVICE_OBJECT pdo, PDEVICE_OBJECT fdo, const GEODE_RES *r);
#endif
BOOLEAN Cs5536Probe(CARD *c);
extern const HWOPS g_Cs5530Ops;

#endif
