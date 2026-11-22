/*
 * Tests the line 21 slicer and the A/53 packer without a capture device:
 * builds EIA-608 waveforms (colour burst, 7-cycle run-in, start bits, 16 data
 * bits) at several sample rates with noise and timing offsets, and checks
 * that the slicer returns the same two bytes.
 *
 *   cl /Isrc tests\test_cc608.c   (or: cc -Isrc tests/test_cc608.c -lm)
 */
#include "cc608.h"

#include <stdio.h>
#include <stdlib.h>

static double frand(void) { return rand() / (double)RAND_MAX; }

static uint8_t with_parity(uint8_t c)
{
    c &= 0x7F;
    return cc608_parity_ok(c) ? c : (uint8_t)(c | 0x80);
}

/* Black at 16, 50 IRE at 16 + 0.5 * 219, as a capture chip digitises it. */
static void make_line(uint8_t *out, int n, double fs, double start_us, uint8_t b1, uint8_t b2, double noise, double gain)
{
    const double black = 16, white = 16 + 0.5 * 219 * gain, spb = fs / CC608_BIT_RATE;
    double t0 = start_us * 1e-6 * fs; /* where the run-in sine starts, at black going up */
    uint8_t bytes[2] = { b1, b2 };
    double raw[4096];
    int i, k;
    for (i = 0; i < n; i++) {
        double v = black, x = i - t0, burst;
        if (x >= 0 && x < 7 * spb) /* run-in: 7 cycles from black to 50 IRE */
            v = black + (white - black) * 0.5 * (1 - cos(2 * M_PI * x / spb));
        else if (x >= 8.75 * spb && x < 26.75 * spb) {
            /* first crossing is at 0.25 bit times; the start bit follows 8.5 bit times later */
            int bit = (int)((x - 8.75 * spb) / spb);
            int val = bit == 0 ? 1 : (bytes[(bit - 1) / 8] >> ((bit - 1) % 8)) & 1;
            v = val ? white : black;
        }
        /* colour burst before the run-in, 40 IRE peak to peak at 3.58 MHz */
        burst = (i * 1.0 / fs * 1e6 < start_us - 2.0 && i * 1.0 / fs * 1e6 > start_us - 8.0)
                    ? 0.5 * 40 * 2.19 * sin(2 * M_PI * 3579545.0 * i / fs)
                    : 0;
        raw[i] = v + burst;
    }
    for (i = 0; i < n; i++) { /* the capture chip's band limit: a box about a third of a bit wide */
        int a = i - (int)(spb / 6), b = i + (int)(spb / 6), c = 0;
        double t = 0;
        if (a < 0) a = 0;
        if (b > n - 1) b = n - 1;
        for (k = a; k <= b; k++) { t += raw[k]; c++; }
        t = t / c + (frand() - 0.5) * 2 * noise;
        out[i] = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t + 0.5);
    }
}

int main(void)
{
    static const double rates[] = { 27e6, 28636363.0, 13.5e6, 35.4e6 };
    int r, fails = 0, total = 0, trial;
    srand(12345);
    for (r = 0; r < 4; r++) {
        int ok = 0, runs = 2000;
        double fs = rates[r];
        for (trial = 0; trial < runs; trial++) {
            uint8_t line[2048], c1 = with_parity((uint8_t)(rand() & 0x7F)), c2 = with_parity((uint8_t)(rand() & 0x7F)), got[2];
            double start = 0.4 + frand() * 2.0, noise = frand() * 6.0, gain = 0.8 + frand() * 0.4;
            make_line(line, 1440 * fs / 27e6 > 2048 ? 2048 : (int)(1440 * fs / 27e6), fs, start, c1, c2, noise, gain);
            total++;
            if (cc608_slice(line, 1440 * fs / 27e6 > 2048 ? 2048 : (int)(1440 * fs / 27e6), fs, got) && got[0] == c1 && got[1] == c2)
                ok++;
        }
        printf("fs %.3f MHz: %d / %d decoded\n", fs / 1e6, ok, runs);
        if (ok < runs * 0.98) fails++;
    }
    {   /* no caption data: noise, flat black, and a line of video must give nothing */
        uint8_t line[1440], got[2];
        int i, bad = 0;
        for (trial = 0; trial < 500; trial++) {
            for (i = 0; i < 1440; i++) line[i] = (uint8_t)(16 + frand() * 8);
            bad += cc608_slice(line, 1440, 27e6, got);
            for (i = 0; i < 1440; i++) line[i] = (uint8_t)(100 + 80 * sin(i * 0.02 + trial));
            bad += cc608_slice(line, 1440, 27e6, got);
        }
        printf("false detections on %d empty lines: %d\n", 1000, bad);
        if (bad > 3) fails++;
    }
    {   /* a bad parity byte must be rejected */
        uint8_t line[1440], got[2];
        make_line(line, 1440, 27e6, 1.1, 0x14 /* wrong parity: 0x14 has two bits set */, 0x2C, 0, 1.0);
        if (cc608_slice(line, 1440, 27e6, got)) { printf("bad parity accepted\n"); fails++; }
    }
    {   /* packer / finder round trip */
        uint8_t blk[32], pairs[8][3], f1[2] = { 0x94, 0x2C }, f2[2] = { 0xC1, 0x80 };
        int n = cc608_pack_a53(blk, f1, f2), got = cc608_find_a53(blk, n, pairs, 8);
        if (n != 18 || got != 2 || pairs[0][0] != 0xFC || pairs[0][1] != 0x94 || pairs[0][2] != 0x2C || pairs[1][0] != 0xFD ||
            pairs[1][1] != 0xC1) { printf("a53 round trip failed (n=%d got=%d)\n", n, got); fails++; }
        else printf("a53 pack/find ok\n");
        /* no zero byte pair may appear in the payload (it would fake a start code) */
        n = cc608_pack_a53(blk, NULL, NULL);
        for (r = 4; r + 1 < n; r++) if (blk[r] == 0 && blk[r + 1] == 0) { printf("00 00 in payload\n"); fails++; }
    }
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails != 0;
}
