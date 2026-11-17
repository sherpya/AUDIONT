/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hardware-format helpers, shared with host tests. ULONG must be 32 bits.
 * AMD CS5536 databook 33238G, Audio Codec Controller PRD format.
 */
#ifndef AUDIONT_GEODE_DMA_H
#define AUDIONT_GEODE_DMA_H
typedef struct { ULONG Address; USHORT Bytes, Flags; } GEODE_PRD;

static BOOLEAN GeodeBuildPrd(GEODE_PRD *p, ULONG entries, ULONG table,
                            ULONG buffer, ULONG periods, ULONG bytes)
{
    ULONG i;
    if (!p || !periods || periods > 128 || entries <= periods ||
        !bytes || bytes > 65520 || (bytes & 3) || (table & 7) || (buffer & 3) ||
        table > 0xffffffffUL - (periods + 1) * 8 + 1 ||
        buffer > 0xffffffffUL - periods * bytes + 1) return FALSE;
    for (i = 0; i < periods; i++) {
        p[i].Address = buffer + i * bytes;
        p[i].Bytes = (USHORT)bytes;
        p[i].Flags = 0x4000; /* end of period: interrupt, continue */
    }
    p[periods].Address = table;
    p[periods].Bytes = 0;
    p[periods].Flags = 0x2000; /* jump, NOT end of table */
    return TRUE;
}
static BOOLEAN GeodeDmaPosition(ULONG address, ULONG base, ULONG bytes, ULONG *position)
{
    ULONG offset = address - base;
    if (!bytes || offset > bytes || (offset & 3)) return FALSE;
    *position = (offset == bytes ? 0 : offset) / 4;
    return TRUE;
}
#endif
