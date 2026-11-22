/*
 * Tests of the portable parts of the TV helpers that need no tuner and no ffmpeg:
 * the North American channel plan (tvchan.hpp) and the caption injector (tvcore.hpp),
 * which must put one A/53 block after every MPEG-2 picture header however the
 * bytes arrive, and change nothing else.
 *
 *   c++ -std=c++17 -Isrc tests/test_tv.cpp -lpthread -o test_tv     (cl /EHsc /std:c++17 /Isrc tests\test_tv.cpp on Windows)
 */
#include "tvchan.hpp"
#include "tvcore.hpp"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void channels()
{
    struct { int ch; bool cable; double mhz; } t[] = {
        { 2, false, 55.25 }, { 3, false, 61.25 }, { 3, true, 61.25 }, { 4, true, 67.25 }, { 5, false, 77.25 }, { 6, true, 83.25 },
        { 7, false, 175.25 }, { 13, true, 211.25 }, { 14, false, 471.25 }, { 36, false, 603.25 }, { 83, false, 885.25 },
        { 1, true, 73.25 }, { 14, true, 121.25 }, { 22, true, 169.25 }, { 23, true, 217.25 }, { 94, true, 643.25 },
        { 95, true, 93.25 }, { 99, true, 117.25 }, { 100, true, 649.25 }, { 135, true, 859.25 },
        { 0, true, 0 }, { 1, false, 0 }, { 84, false, 0 }, { 136, true, 0 },
    };
    for (auto &x : t) {
        double got = tv_channel_mhz(x.ch, x.cable);
        CHECK(fabs(got - x.mhz) < 1e-6, "channel %d %s: %.2f, expected %.2f", x.ch, x.cable ? "cable" : "antenna", got, x.mhz);
    }
}

/* a made-up MPEG-2 elementary stream: sequence header, then pictures of header, extension, user data and slices */
static std::vector<uint8_t> make_es(int pictures, unsigned seed)
{
    std::vector<uint8_t> v;
    auto sc = [&](uint8_t code) { v.push_back(0); v.push_back(0); v.push_back(1); v.push_back(code); };
    auto payload = [&](int n) { /* never contains 00 00 */
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; v.push_back((uint8_t)(1 + (seed >> 16) % 250)); }
    };
    sc(0xB3); payload(8);                 /* sequence header */
    sc(0xB5); payload(6);                 /* sequence extension */
    sc(0xB2); payload(10);                /* sequence user data */
    for (int p = 0; p < pictures; p++) {
        if (p % 5 == 0) { sc(0xB8); payload(4); } /* GOP */
        sc(0x00); payload(4);             /* picture header */
        sc(0xB5); payload(5);             /* picture coding extension */
        if (p % 3 == 0) { sc(0xB2); payload(7); } /* a user data of its own */
        for (int s = 1; s <= 4; s++) { sc((uint8_t)s); payload(30 + (int)(seed >> 20) % 40); } /* slices */
    }
    sc(0xB7);                             /* sequence end */
    return v;
}

static void injector()
{
    const int pics = 37;
    std::vector<uint8_t> es = make_es(pics, 7);
    for (int chunk : { 1, 2, 3, 5, 17, 64, 100000 }) {
        Injector inj;
        std::vector<i64> asked;
        inj.next_block = [&](i64 idx, uint8_t *blk) {
            asked.push_back(idx);
            uint8_t f1[2] = { (uint8_t)(0x80 | (idx & 0x7F)), (uint8_t)0x80 }, f2[2] = { 0x80, 0x80 };
            cc608_pack_a53(blk, f1, f2);
        };
        std::vector<uint8_t> out;
        for (size_t i = 0; i < es.size(); i += (size_t)chunk) inj.feed(&es[i], std::min((size_t)chunk, es.size() - i), out, false);
        inj.feed(nullptr, 0, out, true);
        CHECK((int)asked.size() == pics, "chunk %d: %zu blocks asked for, expected %d", chunk, asked.size(), pics);
        for (size_t i = 0; i < asked.size(); i++) CHECK(asked[i] == (i64)i, "chunk %d: block %zu was for picture %lld", chunk, i, asked[i]);
        /* the same pairs, in order, come back out */
        uint8_t pairs[200][3];
        int n = cc608_find_a53(out.data(), (int)out.size(), pairs, 200);
        CHECK(n == pics * 2, "chunk %d: %d pairs found, expected %d", chunk, n, pics * 2);
        for (int i = 0; i < pics && 2 * i + 1 < n; i++)
            CHECK(pairs[2 * i][1] == (0x80 | (i & 0x7F)) && pairs[2 * i][0] == 0xFC && pairs[2 * i + 1][0] == 0xFD, "chunk %d: pair %d wrong", chunk, i);
        /* and with the blocks cut out, the stream is exactly what went in */
        std::vector<uint8_t> back;
        for (size_t i = 0; i < out.size();) {
            if (i + 18 <= out.size() && out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 1 && out[i + 3] == 0xB2 && out[i + 4] == 'G' && out[i + 5] == 'A') i += 18;
            else back.push_back(out[i++]);
        }
        CHECK(back == es, "chunk %d: the stream changed apart from the caption blocks (%zu vs %zu bytes)", chunk, back.size(), es.size());
        /* each block sits after its picture header and extension, before the first slice */
        int idx = -1, bad = 0;
        for (size_t i = 0; i + 4 < out.size(); i++)
            if (out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 1) {
                if (out[i + 3] == 0x00) idx++;
                if (out[i + 3] == 0xB2 && out[i + 4] == 'G') { if (idx < 0) bad++; if (i + 22 < out.size() && !(out[i + 18 + 3] >= 1 && out[i + 18 + 3] <= 0xAF)) bad++; }
            }
        CHECK(bad == 0, "chunk %d: %d misplaced blocks", chunk, bad);
    }
}

int main()
{
    channels();
    injector();
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
