/* Host tests: gcc -m32 -Idrv -Iinc test/tgeode.c drv/convert.c test/stub.c -lm */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audiont.h"
#include "geode_dma.h"
void OsLog(ULONG level, const char *fmt, ...) { (void)level; (void)fmt; }

static void Dma(void)
{
    GEODE_PRD prd[18];
    ULONG pos;
    memset(prd, 0xcc, sizeof(prd));
    assert(sizeof(ULONG) == 4 && sizeof(GEODE_PRD) == 8);
    assert(GeodeBuildPrd(prd, 17, 0x100000, 0x200000, 16, 2048));
    assert(prd[0].Address == 0x200000 && prd[0].Bytes == 2048 && prd[0].Flags == 0x4000);
    assert(prd[15].Address == 0x207800 && prd[15].Flags == 0x4000);
    assert(prd[16].Address == 0x100000 && !prd[16].Bytes && prd[16].Flags == 0x2000);
    assert(prd[17].Address == 0xcccccccc); /* sentinel after the jump */
    assert(!GeodeBuildPrd(prd, 16, 0x100000, 0x200000, 16, 2048));
    assert(!GeodeBuildPrd(prd, 17, 0x100004, 0x200000, 16, 2048));
    assert(!GeodeBuildPrd(prd, 17, 0xffffff80, 0x200000, 16, 2048));
    assert(!GeodeBuildPrd(prd, 17, 0x100000, 0xfffff000, 16, 2048));
    assert(!GeodeBuildPrd(prd, 17, 0x100000, 0x200000, 16, 65536));
    assert(!GeodeBuildPrd(prd, 17, 0x100000, 0x200000, 0, 2048));
    assert(GeodeDmaPosition(0x200000, 0x200000, 32768, &pos) && pos == 0);
    assert(GeodeDmaPosition(0x207ffc, 0x200000, 32768, &pos) && pos == 8191);
    assert(GeodeDmaPosition(0x208000, 0x200000, 32768, &pos) && pos == 0);
    assert(!GeodeDmaPosition(0x1ffffc, 0x200000, 32768, &pos));
    assert(!GeodeDmaPosition(0x208004, 0x200000, 32768, &pos));
    assert(!GeodeDmaPosition(0x200001, 0x200000, 32768, &pos));
    puts("PASS PRD layout, jump, overflow/alignment rejection and DMA position wrap");
}
static void Rate(ULONG input, ULONG output)
{
    CONVERTER cv;
    PCM_FORMAT f = { input, 2, 16, 4, 0, 3 };
    SHORT *samples = malloc(input * 4);
    LONG *mix = calloc((output + 512) * 2, sizeof(LONG));
    ULONG i, consumed = 0, frames = 0, guard = 0, crossings = 0;
    double rms = 0;
    assert(samples && mix);
    for (i = 0; i < input; i++) {
        samples[i*2] = (SHORT)lrint(sin(2 * M_PI * 1000 * i / input) * 12000);
        samples[i*2+1] = -samples[i*2];
    }
    memset(&cv, 0, sizeof(cv));
    assert(ConvSetupRate(&cv, &f, FALSE, output));
    assert(cv.Rs.Passthrough == (input == output));
    while (consumed < input * 4) {
        ULONG used = 0, n = MIN(317, input * 4 - consumed), got;
        assert(++guard < 20000 && frames < output + 128);
        got = ConvPlay(&cv, (UCHAR *)samples + consumed, n, &used, mix + frames * 2, 73, 32768, 32768);
        assert(got || used);
        consumed += used; frames += got;
    }
    assert(frames > output - 256 && frames <= output);
    for (i = 1000; i < frames; i++) {
        LONG l = mix[i*2], r = mix[i*2+1];
        assert(l + r >= -2 && l + r <= 2);
        if (input == output) assert(l == samples[i*2]);
        rms += (double)l * l;
        if (i > 1000 && l >= 0 && mix[(i-1)*2] < 0) crossings++;
    }
    rms = sqrt(rms / (frames - 1000));
    assert(rms > 8300 && rms < 8650);
    assert(fabs((double)crossings * output / (frames - 1000) - 1000) < 3);
    ConvReset(&cv);
    assert(cv.CarryLen == 0 && cv.PendLen == 0);
    ConvFree(&cv);
    free(samples); free(mix);
    printf("PASS %lu -> %lu Hz: %lu frames, stereo polarity, tone, level, reset%s\n",
           input, output, frames, input == output ? ", bit-exact passthrough" : "");
}
int main(void)
{
    Dma();
    Rate(44100, 44100); Rate(48000, 44100); Rate(22050, 44100);
    Rate(44100, 48000); Rate(48000, 48000);
    puts("RESULT tgeode PASS");
    return 0;
}
