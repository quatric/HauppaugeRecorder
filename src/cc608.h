/*
 * CEA-608 (line 21) closed captions: a slicer that turns one raw VBI scan line
 * into the two caption bytes, and a packer that writes those bytes as ATSC A/53
 * "GA94" user data, which is how MPEG-2 video carries 608 captions (the form
 * ffmpeg, VLC, CCExtractor, DVD players and TVs read).
 *
 * Header only, plain C, no dependencies, so tests/test_cc608.c can run it
 * without a capture device.
 */
#ifndef CC608_H
#define CC608_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define CC608_BIT_RATE 503496.5 /* 32 x the horizontal line frequency, Hz */

/* The caption byte carries odd parity in bit 7. */
static inline int cc608_parity_ok(uint8_t b)
{
    b ^= (uint8_t)(b >> 4);
    b ^= (uint8_t)(b >> 2);
    b ^= (uint8_t)(b >> 1);
    return b & 1;
}

/* Smoothed value of the line around x (in samples), a window of `half` either side. */
static inline double cc608_level(const float *s, int n, double x, double half)
{
    int a = (int)floor(x - half), b = (int)ceil(x + half), i;
    double sum = 0;
    int cnt = 0;
    if (a < 0) a = 0;
    if (b > n - 1) b = n - 1;
    for (i = a; i <= b; i++) { sum += s[i]; cnt++; }
    return cnt ? sum / cnt : 0;
}

/*
 * line:    8-bit luma samples of one scan line (as the capture driver gives them)
 * fs:      sampling rate in Hz
 * out[2]:  the two caption bytes, parity bit included, when this returns 1
 * Returns 1 if a caption pair with correct parity was found, else 0. A line
 * with no caption data (no run-in) returns 0.
 */
static inline int cc608_slice(const uint8_t *line, int n, double fs, uint8_t out[2])
{
    enum { MAXW = 4096, MAXX = 128 };
    float s[MAXW];
    double cross[MAXX];
    int hist[256], w, bw, i, k, nx = 0, armed = 0, lo = 0, hi = 255, acc;
    double spb, thr, amp, c0, sum;
    int j;
    if (!line || n < 64 || fs < 8e6 || fs > 60e6) return 0;
    spb = fs / CC608_BIT_RATE;
    w = (int)(66e-6 * fs);
    if (w > n) w = n;
    if (w > MAXW) w = MAXW;
    if (w < (int)(26 * spb)) return 0;

    /* A box filter one colour-burst period wide removes the 3.58 MHz burst and noise. */
    bw = (int)floor(fs / 3579545.0 + 0.5);
    if (bw < 1) bw = 1;
    for (i = 0; i < w; i++) {
        int a = i - bw / 2, b = a + bw - 1, c = 0;
        double t = 0;
        if (a < 0) a = 0;
        if (b > w - 1) b = w - 1;
        for (k = a; k <= b; k++) { t += line[k]; c++; }
        s[i] = (float)(t / c);
    }

    /* Black and the 50 IRE level, from the 10th and 97th percentile of the line. */
    memset(hist, 0, sizeof hist);
    for (i = 0; i < w; i++) hist[(int)(s[i] + 0.5f) & 255]++;
    acc = 0;
    for (i = 0; i < 256; i++) { acc += hist[i]; if (acc >= w / 10) { lo = i; break; } }
    acc = 0;
    for (i = 255; i >= 0; i--) { acc += hist[i]; if (acc >= w * 3 / 100) { hi = i; break; } }
    amp = hi - lo;
    if (amp < 28) return 0; /* nothing there but noise */
    thr = (lo + hi) / 2.0;

    /* Rising edges through the middle, one per clock run-in cycle. */
    for (i = 1; i < w && nx < MAXX; i++) {
        if (s[i] < thr - amp * 0.2) armed = 1;
        else if (armed && s[i] >= thr && s[i - 1] < thr) {
            cross[nx++] = (i - 1) + (thr - s[i - 1]) / (s[i] - s[i - 1]);
            armed = 0;
        }
    }

    /* The run-in is 7 cycles one bit period apart; take the first run of 7. */
    for (j = 0; j + 6 < nx; j++) {
        int ok = 1;
        double base = cross[j], tol = 0.3 * spb;
        int pos = j;
        sum = base;
        for (k = 1; k <= 6 && ok; k++) {
            int found = 0, m;
            for (m = pos + 1; m < nx; m++) {
                double d = cross[m] - (base + k * spb);
                if (d > tol) break;
                if (d >= -tol) { found = 1; pos = m; sum += cross[m] - k * spb; break; }
            }
            ok = found;
        }
        if (!ok) continue;
        c0 = sum / 7.0;
        {
            double half = spb * 0.2;
            int bits[16], q;
            /* EIA-608: the start bit's leading edge is 8.5 bit times after the first run-in
             * crossing (27.38 us against 10.5 us after sync), preceded by two zero bits;
             * data bit q is centred 10 + q bit times after that crossing. */
            if (cc608_level(s, w, c0 + 7.5 * spb, half) > thr) continue;
            if (cc608_level(s, w, c0 + 8.0 * spb, half) > thr) continue;
            if (cc608_level(s, w, c0 + 9.0 * spb, half) <= thr) continue;
            if (c0 + 25.3 * spb >= w) continue;
            for (q = 0; q < 16; q++) bits[q] = cc608_level(s, w, c0 + (10.0 + q) * spb, half) > thr;
            out[0] = out[1] = 0;
            for (q = 0; q < 8; q++) {
                out[0] |= (uint8_t)(bits[q] << q);
                out[1] |= (uint8_t)(bits[8 + q] << q);
            }
            if (cc608_parity_ok(out[0]) && cc608_parity_ok(out[1])) return 1;
        }
    }
    return 0;
}

/*
 * One A/53 user data block for a picture: the two caption pairs of a frame,
 * field 1 (cc_type 0) and field 2 (cc_type 1). A field with no data gets the
 * null pair 0x80 0x80 (0x00 with its parity bit), which decoders ignore.
 * Writes 4+4+2+1+6+1 = 18 bytes to out; returns 18.
 */
static inline int cc608_pack_a53(uint8_t *out, const uint8_t *f1, const uint8_t *f2)
{
    static const uint8_t null_pair[2] = { 0x80, 0x80 };
    int n = 0;
    if (!f1) f1 = null_pair;
    if (!f2) f2 = null_pair;
    out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x01; out[n++] = 0xB2; /* user_data_start_code */
    out[n++] = 'G'; out[n++] = 'A'; out[n++] = '9'; out[n++] = '4';
    out[n++] = 0x03;        /* user_data_type_code: cc_data */
    out[n++] = 0x40 | 2;    /* process_cc_data_flag, cc_count = 2 */
    out[n++] = 0xFF;        /* em_data */
    out[n++] = 0xFC; out[n++] = f1[0]; out[n++] = f1[1]; /* cc_valid, NTSC line 21 field 1 */
    out[n++] = 0xFD; out[n++] = f2[0]; out[n++] = f2[1]; /* field 2 */
    out[n++] = 0xFF;        /* marker_bits */
    return n;
}

/* Reads A/53 caption pairs back out of an MPEG-2 video elementary stream, for tests and diagnostics. */
static inline int cc608_find_a53(const uint8_t *p, int n, uint8_t pairs[][3], int maxpairs)
{
    int i, got = 0;
    for (i = 0; i + 11 < n && got < maxpairs; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && p[i + 3] == 0xB2 && p[i + 4] == 'G' && p[i + 5] == 'A' &&
            p[i + 6] == '9' && p[i + 7] == '4' && p[i + 8] == 3) {
            int cnt = p[i + 9] & 0x1F, k, o = i + 11;
            for (k = 0; k < cnt && o + 2 < n && got < maxpairs; k++, o += 3) {
                pairs[got][0] = p[o];
                pairs[got][1] = p[o + 1];
                pairs[got][2] = p[o + 2];
                got++;
            }
        }
    }
    return got;
}

#endif
