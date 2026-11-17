#!/bin/bash
# Pure code tests, no kernel installation or hardware I/O. Real WDM layouts.
set -euo pipefail
cd "$(dirname "$0")/.."
T="$PWD/build/geode-tests"
mkdir -p "$T"
export WINEPREFIX="$T/wine" WINEDEBUG=-all
# Run 64-bit test executables on hosts without a 32-bit libc; Windows ULONG
# remains 32 bits. Also compile the resource parser tests with the x86 DDK.
CF=(-O2 -Wall -Wno-unused-function -DAUDIONT_WDM=1 -D_AMD64_=1
    -D_WIN32_WINNT=0x0500 -DNTDDI_VERSION=0x05000000
    -I/usr/share/mingw-w64/include/ddk -Iinc -Idrv)
x86_64-w64-mingw32-gcc "${CF[@]}" test/tgeode_res.c drv/geode_res.c -o "$T/tgeode_res.exe"
x86_64-w64-mingw32-gcc "${CF[@]}" test/tcs5530.c -o "$T/tcs5530.exe"
for t in tgeode tfmt; do
    x86_64-w64-mingw32-gcc "${CF[@]}" "test/$t.c" drv/convert.c test/stub.c -lm -o "$T/$t.exe"
done
i686-w64-mingw32-gcc -O2 -DAUDIONT_WDM=1 -D_X86_=1 -D_WIN32_WINNT=0x0500 -DNTDDI_VERSION=0x05000000 \
    -I/usr/share/mingw-w64/include/ddk -Iinc -Idrv test/tgeode_res.c drv/geode_res.c -o "$T/tgeode_res32.exe"
for t in tgeode_res tcs5530 tgeode tfmt; do wine "$T/$t.exe"; done
