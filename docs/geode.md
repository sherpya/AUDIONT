# Geode audio for Windows XP

This branch adds CS5530 and CS5536 playback backends to AUDIONT, using its
WinMM driver, mixer and cross-build. The motherboard BIOS and VSA firmware
are not replaced.

## Status

| Controller | Status | Configuration |
| --- | --- | --- |
| CS5536, PCI `1022:2093` | **Working: playback** | Linutop 2 / FIC ION603, subsystem `1509:801b`, revision 01, Realtek ALC203, Windows XP SP3 x86 |
| CS5530, PCI `1078:0103` and BIOS PnP `*CRX0002` | **Partially tested** | Backend present; no successful audible-output validation |

On 2026-11-17, CS5536 installation from a clean XP clone completed
successfully. Windows system sounds, WAV and MP3 playback were audible
through headphones connected to line out. Playback worked immediately,
without rebooting. The notification-area volume control appeared only after
a reboot; its absence did not prevent audio playback.

This status covers playback on the configuration above. Recording, MIDI,
OPL synthesis, MPU-401 and native CS5530 PCI audio are not implemented by
these backends. Suspend/hibernate requests are vetoed; seamless resume is
not supported. Driver Verifier, other boards and other OS versions remain
untested.

## Design

- **CS5530:** BIOS VSA1 SB16 at 44,100 Hz, signed 16-bit stereo. The
  `CRX0002` node owns the Windows-assigned ports, IRQ and ISA DMA channels.
  The PCI companion installs a null service, avoiding duplicate claims.
  DMA uses `IoGetDmaAdapter`, a HAL common buffer and map registers covering
  the full ring, without direct 8237 programming. The ring must fit below
  16 MiB and within a 128 KiB boundary.
- **CS5536:** native AC'97 at 48,000 Hz, signed 16-bit stereo, with a
  circular PRD table and a 32-bit-addressable DMA ring. BAR0 and the
  translated IRQ are validated before startup. PCI `DevicePrivate`
  descriptors are ignored as metadata; invalid hardware resources still
  fail startup. The controller has its own codec transaction implementation
  and reuses AUDIONT's AC'97 codec initialization.
- Both use a 32 KiB playback ring, sixteen 2 KiB periods, master/PCM controls
  and software sample conversion. The new paths are WDM x86 only and
  require the XP `KeFlushQueuedDpcs` export.
- Removal drains queued requests and DPCs before freeing state. Failed
  playback starts return queued buffers with an error. Forced power
  transitions recreate devices on return; applications must reopen handles.
  This power recovery path has not been validated on hardware.

## Build and installation

Requirements: MinGW-w64 GCC/DDK headers, Make, Python 3 and `pefile`.

```sh
bash tools/geodebuild.sh
```

Copy the whole `build/geode/package/` folder to XP and run `setup.exe` as
administrator. It contains the dedicated INF, x86 kernel driver, WinMM DLL,
mixer and diagnostic tools. No proprietary driver or firmware is included.
An existing installation can use Device Manager's Update Driver / Have Disk
with `2kxp/audiont.inf`. Use `setup.exe /u` to uninstall.

On CS5530, both the PCI companion and `CRX0002` need the INF. Leave BIOS
audio/VSA enabled and do not disable `CRX0002`. Clean installation has
been validated only on CS5536.

The kernel uses `-march=i586 -mgeneral-regs-only`, without SSE/FXSR.
`manifest.json` records the upstream base, source revision and dirty state,
compiler, build epoch, source/package hashes, and whether the kernel matches
the validated R3 binary. Keep the matching `audiont.map` and
`obj/audiont.symbols` in `build/geode/2kxp/i386/` for debugging.

`SOURCE_DATE_EPOCH` defaults to the current commit timestamp. To retain the
timestamp used for R3:

```sh
SOURCE_DATE_EPOCH=1793882612 bash tools/geodebuild.sh
```

Byte identity also depends on the sources and toolchain; check the hash.
Existing NT and amd64 backends remain available through the usual Makefile
and `tools/wdmbuild.sh`.

## Diagnostics and tests

Run without RDP audio redirection to test the physical output:

```bat
audtest info
audtest startlog
audtest dump
audtest stats
audtest tone 440 5000 48000 16 2
audtest log
```

`startlog` reads the saved START_DEVICE result even with Code 10 and no live
audio device. It is a snapshot, not current playback state. Other commands
read live diagnostics. Check master/PCM volume and mute with `audmix.exe`
or the Windows mixer.

```sh
bash tools/geodetest.sh
```

Four user-mode tests run under Wine in an isolated prefix, without loading
the driver or touching hardware:

- `tgeode_res`: resource ordering, IDs, translation consistency,
  malformed/duplicate resources, BAR bounds and PCI private descriptors.
  Also cross-compiled against x86 DDK structures.
- `tcs5530`: modeled HAL/DSP allocation, mapping, playback, IRQ, mixer,
  restart, timeout and cleanup. This is not hardware validation.
- `tgeode`: 44.1/48 kHz conversion, passthrough, channel polarity, PRD
  layout, bounds and position wrap.
- `tfmt`: existing PCM, float and multichannel conversion regression tests.

All four passed for XP x86, XP x64 and legacy NT cross-builds and the WDM
PE import checks also passed during development. QEMU does not emulate the
CS5536 controller or the GX1 VSA firmware.

## References

- [Linux CS5530](https://github.com/torvalds/linux/blob/v6.18/sound/pci/cs5530.c)
  and [SB16 PCM](https://github.com/torvalds/linux/blob/v6.18/sound/isa/sb/sb16_main.c).
- [Linux CS5535/6](https://github.com/torvalds/linux/blob/v6.18/sound/pci/cs5535audio/cs5535audio.c)
  and [PCM](https://github.com/torvalds/linux/blob/v6.18/sound/pci/cs5535audio/cs5535audio_pcm.c).
- [AMD CS5536 databook, 33238G](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/datasheets/33238G_cs5536_db.pdf):
  register offsets and PRD format. The GPL-2.0-only Linux header was not copied.
- [Microsoft common-buffer DMA](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-common-buffer-system-dma),
  [adapter allocation](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/allocating-an-adapter-channel-for-common-buffer-system-dma)
  and [resource descriptors](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_cm_partial_resource_descriptor).
