/*
 * Portable core of the TV tuner helpers (wintv-capture on Windows, tv-capture-linux
 * on Linux): records analog TV from a USB tuner such as the Hauppauge WinTV
 * HVR-950Q to an MPEG-2 file with the line 21 closed captions (CEA-608) encoded
 * into the video.
 *
 * Why not the vendor's software: the raw VBI scan lines are read from the
 * capture driver and sliced here (cc608.h), so captions never depend on the
 * vendor application. Every frame carries one caption pair per field (the null
 * pair when the broadcast sent nothing) as A/53 "GA94" user data in the MPEG-2
 * video, which ffmpeg, VLC, CCExtractor, DVD players and TVs read.
 *
 * What is here and what is not:
 *   here          options, the Engine (preview, levels, caption queues), Session (one
 *                 recording: ffmpeg + caption injector + muxing), the --serve protocol
 *                 for HauppaugeRecorder (docs/TVHELPER.md), the command line
 *   a Backend     opens the tuner, delivers frames, audio and VBI lines to the Engine
 *                 (DirectShow in wintv.cpp, V4L2 and ALSA in tv_linux.cpp)
 *   tvplat.hpp    time, files, child processes, stdio
 *
 * A Session is:
 *   video -> constant frame rate -> ffmpeg #1 (MPEG-2, no B frames) -> caption injector -> temp .m2v
 *   audio -> temp .wav (gaps filled with silence)
 *   at the end ffmpeg #2 copies the video and encodes the audio into the final file
 */
#ifndef TVCORE_HPP
#define TVCORE_HPP

#include "tvplat.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "cc608.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static std::atomic<bool> g_stop(false);
static bool g_quiet = false; /* serve mode: nothing on stdout, little on stderr */

static void logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
static std::string fmt_str(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}
static bool icontains(const std::string &hay, const std::string &needle)
{
    auto lower = [](std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/* ---- options ------------------------------------------------------------------------------------ */
struct Options {
    std::string out, dir = ".";
    double seconds = 0;           /* 0 = until Ctrl-C */
    int start_h = -1, start_m = 0, start_s = 0;
    std::string quality = "high";
    double mbps = 0, peak = 0;    /* 0 = from the preset */
    bool cbr = false;
    std::string source = "tuner"; /* tuner | composite | svideo */
    int channel = 0;
    bool cable = true;
    bool pal = false;
    int audio_kbps = 224;
    bool ac3 = false;
    bool wide = false;            /* 16:9 flag */
    bool bff = true, bff_set = false; /* field order of the picture: bottom first for 525-line, top first for 625-line unless given */
    bool no_cc = false, keep_temp = false, list = false, help = false, serve = false, scan = false;
    double probe = 0, selftest = 0;
    double vbi_offset_ms = 0;
    std::string device = "WinTV", audio_device = "950", ffmpeg;
};

struct Preset { const char *name; double mbps; };
static const Preset PRESETS[] = { { "low", 3.0 }, { "medium", 5.0 }, { "high", 7.0 }, { "best", 9.0 } };
static double preset_mbps(const std::string &q)
{
    for (auto &p : PRESETS) if (q == p.name) return p.mbps;
    return 7;
}

/* "90", "90s", "45m", "2h", "1:30" (1 h 30 min), "1:30:15" -> seconds; -1 on error */
static double parse_duration(const std::string &t)
{
    if (t.empty()) return -1;
    int a, b, c;
    char u;
    if (sscanf(t.c_str(), "%d:%d:%d", &a, &b, &c) == 3) return a * 3600.0 + b * 60.0 + c;
    if (sscanf(t.c_str(), "%d:%d", &a, &b) == 2) return a * 3600.0 + b * 60.0;
    double v;
    if (sscanf(t.c_str(), "%lf%c", &v, &u) == 2) {
        switch (tolower(u)) {
        case 's': return v;
        case 'm': return v * 60;
        case 'h': return v * 3600;
        default: return -1;
        }
    }
    if (sscanf(t.c_str(), "%lf", &v) == 1) return v;
    return -1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "%s: record a USB analog TV tuner (Hauppauge WinTV HVR-950Q) to MPEG-2 with closed captions\n\n"
            "  %s -s tuner -c 3 -t 1:30 -q high -o show.mpg\n"
            "  %s -s tuner -c 3 --antenna --at 20:00 -t 30m -b 6.5\n\n"
            "  -o FILE            output (.mpg; .ts gives a transport stream). Default: timestamped, in --dir\n"
            "      --dir FOLDER   folder for timestamped files (default: current)\n"
            "  -t TIME            how long: 90 | 90s | 45m | 2h | 1:30 (1 h 30 min) | 1:30:15. Default: until Ctrl-C\n"
            "      --at HH:MM[:SS] wait until this clock time to start (tomorrow if already past)\n"
            "  -q low|medium|high|best   quality preset: 3 / 5 / 7 / 9 Mbit/s (default high)\n"
            "  -b MBPS            video bitrate in Mbit/s, overrides the preset (1-15)\n"
            "      --peak MBPS    VBR peak bitrate (default 1.4 x the bitrate)\n"
            "      --cbr          constant bitrate (default: variable)\n"
            "  -s tuner|composite|svideo   input (default tuner)\n"
            "  -c N               TV channel with -s tuner\n"
            "      --cable|--antenna      tuner input (default cable)\n"
            "      --pal          625-line 25 fps picture (default NTSC 480i)\n"
            "      --wide         flag the picture as 16:9\n"
            "      --audio-kbps N audio bitrate (default 224)\n"
            "      --ac3          Dolby Digital audio instead of MPEG audio\n"
            "      --tff          top field first (default bottom field first)\n"
            "      --no-cc        do not decode or write captions\n"
            "      --device TEXT  capture device (name contains TEXT, or a /dev path on Linux)\n"
            "      --audio-device TEXT   audio device name contains TEXT (default 950)\n"
            "      --ffmpeg PATH  ffmpeg (default: next to this program, $WINTV_FFMPEG, then PATH)\n"
            "      --vbi-offset MS  shift the captions against the picture (default 0)\n"
            "      --keep-temp    keep the temporary video/audio files\n"
            "      --list         list devices and exit\n"
            "      --scan         list channels that deliver a picture\n"
            "      --probe SECS   run SECS seconds and report what the driver delivers\n"
            "      --selftest SECS  encode synthetic bars, tone and a test caption (no tuner needed)\n"
            "      --serve        helper mode for HauppaugeRecorder (commands on stdin, events on stdout)\n",
            prog, prog, prog);
}

static bool parse_args(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) { logf("%s needs a value", name); exit(2); }
            return argv[++i];
        };
        if (a == "-o") o.out = need("-o");
        else if (a == "--dir") o.dir = need("--dir");
        else if (a == "-t") { o.seconds = parse_duration(need("-t")); if (o.seconds < 0) { logf("bad time for -t"); return false; } }
        else if (a == "--at") {
            if (sscanf(need("--at"), "%d:%d:%d", &o.start_h, &o.start_m, &o.start_s) < 2 || o.start_h > 23 || o.start_m > 59) { logf("bad time for --at"); return false; }
        } else if (a == "-q") o.quality = need("-q");
        else if (a == "-b") o.mbps = atof(need("-b"));
        else if (a == "--peak") o.peak = atof(need("--peak"));
        else if (a == "--cbr") o.cbr = true;
        else if (a == "-s") o.source = need("-s");
        else if (a == "-c") o.channel = atoi(need("-c"));
        else if (a == "--cable") o.cable = true;
        else if (a == "--antenna") o.cable = false;
        else if (a == "--pal") o.pal = true;
        else if (a == "--wide") o.wide = true;
        else if (a == "--audio-kbps") o.audio_kbps = atoi(need("--audio-kbps"));
        else if (a == "--ac3") o.ac3 = true;
        else if (a == "--tff") { o.bff = false; o.bff_set = true; }
        else if (a == "--bff") { o.bff = true; o.bff_set = true; }
        else if (a == "--no-cc") o.no_cc = true;
        else if (a == "--keep-temp") o.keep_temp = true;
        else if (a == "--device") o.device = need("--device");
        else if (a == "--audio-device") o.audio_device = need("--audio-device");
        else if (a == "--ffmpeg") o.ffmpeg = need("--ffmpeg");
        else if (a == "--vbi-offset") o.vbi_offset_ms = atof(need("--vbi-offset"));
        else if (a == "--list") o.list = true;
        else if (a == "--scan") o.scan = true;
        else if (a == "--probe") o.probe = atof(need("--probe"));
        else if (a == "--selftest") o.selftest = atof(need("--selftest"));
        else if (a == "--serve") o.serve = true;
        else if (a == "-h" || a == "--help") o.help = true;
        else { logf("unknown option %s", a.c_str()); return false; }
    }
    return true;
}

struct AudioFmt {
    int rate = 48000, channels = 2, bits = 16;
    int block() const { return channels * bits / 8; }
    int bytes_per_sec() const { return rate * block(); }
};

/* ---- output to the GUI (--serve): framed messages on stdout -------------------------------------- *
 * Each message: 1 type byte, 3 zero bytes, u32 little-endian payload length, payload.                */
class Out {
public:
    void start() { th_ = std::thread([this] { loop(); }); }
    void msg(char type, const void *p, size_t n)
    {
        std::vector<uint8_t> b(8 + n);
        b[0] = (uint8_t)type;
        uint32_t len = (uint32_t)n;
        memcpy(&b[4], &len, 4);
        if (n) memcpy(&b[8], p, n);
        std::lock_guard<std::mutex> l(m_);
        if (qbytes_ > (4u << 20) && type == 'A') return; /* the GUI is not keeping up: drop audio, never status */
        qbytes_ += b.size();
        q_.push_back(std::move(b));
        cv_.notify_one();
    }
    void text(char type, const std::string &s) { msg(type, s.data(), s.size()); }
    void frame(const uint8_t *bgrx, int w, int h)
    {
        std::lock_guard<std::mutex> l(m_);
        size_t n = (size_t)w * h * 4;
        fr_.resize(16 + n);
        fr_[0] = 'F'; fr_[1] = fr_[2] = fr_[3] = 0;
        uint32_t len = (uint32_t)(8 + n), ww = (uint32_t)w, hh = (uint32_t)h;
        memcpy(&fr_[4], &len, 4); memcpy(&fr_[8], &ww, 4); memcpy(&fr_[12], &hh, 4);
        memcpy(&fr_[16], bgrx, n);
        have_fr_ = true; /* only the latest picture is kept */
        cv_.notify_one();
    }
    void stop()
    {
        { std::lock_guard<std::mutex> l(m_); quit_ = true; cv_.notify_one(); }
        if (th_.joinable()) th_.join();
    }
private:
    void loop()
    {
        for (;;) {
            std::vector<uint8_t> b;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return !q_.empty() || have_fr_ || quit_; });
                if (!q_.empty()) { b = std::move(q_.front()); q_.pop_front(); qbytes_ -= b.size(); }
                else if (have_fr_) { b.swap(fr_); have_fr_ = false; }
                else break;
            }
            if (!plat::stdout_write(b.data(), b.size())) { g_stop = true; return; }
        }
    }
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> q_;
    size_t qbytes_ = 0;
    std::vector<uint8_t> fr_;
    bool have_fr_ = false, quit_ = false;
};

/* packed 4:2:2 (YUYV or UYVY, BT.601 limited range) to 32-bit BGRX for the preview */
static void packed422_to_bgrx(const uint8_t *s, int w, int h, bool uyvy, uint8_t *d)
{
    auto clamp = [](int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += 2) {
            const uint8_t *p = s + ((size_t)y * w + x) * 2;
            int y0, y1, u, v;
            if (uyvy) { u = p[0] - 128; y0 = p[1] - 16; v = p[2] - 128; y1 = p[3] - 16; }
            else { y0 = p[0] - 16; u = p[1] - 128; y1 = p[2] - 16; v = p[3] - 128; }
            uint8_t *o = d + ((size_t)y * w + x) * 4;
            int rr = 409 * v + 128, gg = -100 * u - 208 * v + 128, bb = 516 * u + 128;
            int c0 = 298 * y0, c1 = 298 * y1;
            o[0] = clamp((c0 + bb) >> 8); o[1] = clamp((c0 + gg) >> 8); o[2] = clamp((c0 + rr) >> 8); o[3] = 0;
            o[4] = clamp((c1 + bb) >> 8); o[5] = clamp((c1 + gg) >> 8); o[6] = clamp((c1 + rr) >> 8); o[7] = 0;
        }
}

static std::string find_ffmpeg(const std::string &given)
{
    if (!given.empty()) return given;
    if (const char *e = getenv("WINTV_FFMPEG")) if (plat::file_exists(e)) return e;
    std::string name = std::string("ffmpeg") + plat::exe_ext(), beside = plat::exe_dir() + name;
    if (plat::file_exists(beside)) return beside;
    return plat::find_in_path(name);
}

/* Inserts one A/53 block after every picture header, before its first slice. */
class Injector {
public:
    std::function<void(i64 idx, uint8_t *blk)> next_block;
    i64 pic = 0;
    bool pending = false;
    std::vector<uint8_t> buf;
    void feed(const uint8_t *d, size_t n, std::vector<uint8_t> &out, bool flush)
    {
        buf.insert(buf.end(), d, d + n);
        size_t emit = 0, i = 0, end = buf.size();
        while (i + 3 < end) {
            if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
                uint8_t code = buf[i + 3];
                if (code == 0x00) pending = true;
                else if (pending && code >= 0x01 && code <= 0xAF) {
                    out.insert(out.end(), buf.begin() + emit, buf.begin() + i);
                    emit = i;
                    uint8_t blk[32];
                    next_block(pic++, blk);
                    out.insert(out.end(), blk, blk + 18);
                    pending = false;
                }
                i += 3;
            } else i++;
        }
        size_t keep = flush ? end : (end >= 3 ? end - 3 : 0);
        if (keep < emit) keep = emit;
        out.insert(out.end(), buf.begin() + emit, buf.begin() + keep);
        buf.erase(buf.begin(), buf.begin() + keep);
    }
};

static void wav_header(FILE *f, const AudioFmt &a, i64 data_bytes)
{
    auto w32 = [&](uint32_t x) { fwrite(&x, 4, 1, f); };
    auto w16 = [&](uint16_t x) { fwrite(&x, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32((uint32_t)(36 + data_bytes)); fwrite("WAVEfmt ", 1, 8, f); w32(16);
    w16(1); w16((uint16_t)a.channels); w32((uint32_t)a.rate); w32((uint32_t)a.bytes_per_sec()); w16((uint16_t)a.block()); w16((uint16_t)a.bits);
    fwrite("data", 1, 4, f); w32((uint32_t)data_bytes);
}

/* ---- the engine and the backend interface ---------------------------------------------------------- */
struct VFrame { i64 t; std::vector<uint8_t> px; };
struct VbiRes { i64 t; bool valid; uint8_t b[2]; };

class Engine;

/* What a platform provides. Frames, audio and VBI lines go to the Engine's video(), audio_in() and vbi_push();
 * time stamps are in 100 ns on any clock, the same one for all three. */
struct Backend {
    virtual ~Backend() {}
    /* Opens the tuner and builds the capture: sets e.vw, e.vh, e.vbytes, e.pixfmt, e.fps_num, e.fps_den, e.frame_dur, e.have_audio/audio,
     * e.have_vbi/vbi_fs. On failure returns false with e.error set (a message for the user). */
    virtual bool open(Engine &e) = 0;
    virtual bool run(Engine &e) = 0;   /* start delivering */
    virtual void stop(Engine &e) = 0;  /* stop delivering, keep open */
    virtual void close(Engine &e) = 0;
    virtual bool route(Engine &e) = 0; /* apply e.o.source (and the TV standard) */
    virtual bool tune(Engine &e) = 0;  /* apply e.o.channel / e.o.cable */
    virtual void list_devices() = 0;
    virtual bool has_tuner(Engine &) { return true; }
};

class Session;

class Engine {
public:
    Options o;
    Out *out = nullptr;
    Backend *be = nullptr;
    std::string error;
    bool is_open = false;
    i64 run_start = 0;

    /* formats, set by the backend */
    int vw = 720, vh = 480;
    size_t vbytes = 720 * 480 * 2;
    std::string pixfmt = "yuyv422"; /* what the backend delivers: yuyv422 or uyvy422 */
    double fps_num = 30000, fps_den = 1001;
    i64 frame_dur = 333667;
    AudioFmt audio;
    bool have_audio = false, have_vbi = false;
    double vbi_fs = 27e6;

    /* live state, read by the serve loop */
    std::atomic<bool> preview_on{true}, monitor_on{false};
    std::atomic<i64> last_video{0}; /* plat::now_100ns() of the last picture */
    std::atomic<int> peak_l{0}, peak_r{0};
    std::atomic<i64> cb_v{0}, cb_a{0}, cb_b{0}, cb_v_short{0}, vbi_valid{0}, vbi_bufs{0};
    bool probe_dump = false;
    std::vector<uint8_t> pv_buf;
    int pv_count = 0;

    /* the running recording, if any */
    std::mutex rm;
    std::shared_ptr<Session> rec;

    /* VBI queues: one entry per field per frame, in arrival order */
    std::mutex bm;
    std::condition_variable bcv;
    std::deque<VbiRes> q1, q2;
    i64 vbi_last_t = 0;
    std::atomic<bool> vbi_done{false};

    bool open() { error.clear(); if (!o.bff_set) o.bff = !o.pal; is_open = be->open(*this); return is_open; }
    bool run() { run_start = plat::now_100ns(); last_video = 0; return be->run(*this); }
    void close() { be->stop(*this); be->close(*this); is_open = false; }
    i64 rel_now() const { return plat::now_100ns() - run_start; }

    void video(i64 t, const uint8_t *p, long n);
    void audio_in(i64 t, const uint8_t *p, long n);
    /* One field's VBI lines (nlines of `samples` 8-bit samples, `stride` bytes apart; line 21 is searched for
     * from the last line backwards). field 1 or 2. Pushes one entry, valid or not. */
    void vbi_push(int field, i64 t, const uint8_t *lines, int nlines, int stride, int samples, double fs);
    /* One caption entry for a field, already sliced (or null): the same queues vbi_push() fills. */
    void cc_push(int field, i64 t, bool valid, uint8_t b1, uint8_t b2);
    void cc_reset();
};

void Engine::cc_push(int field, i64 t, bool valid, uint8_t b1, uint8_t b2)
{
    VbiRes r{};
    r.t = t;
    r.valid = valid;
    r.b[0] = b1;
    r.b[1] = b2;
    std::lock_guard<std::mutex> l(bm);
    std::deque<VbiRes> &q = field == 1 ? q1 : q2;
    q.push_back(r);
    if (q.size() > 600) q.pop_front();
    vbi_last_t = t;
    bcv.notify_all();
}

void Engine::vbi_push(int field, i64 t, const uint8_t *lines, int nlines, int stride, int samples, double fs)
{
    vbi_bufs++;
    VbiRes r{};
    r.t = t;
    uint8_t got[2];
    int found = -1;
    for (int l = nlines - 1; l >= 0; l--) { /* line 21 is the last line of its field */
        if (cc608_slice(lines + (size_t)l * stride, samples, fs, got)) { found = l; break; }
    }
    if (found >= 0) { r.valid = true; memcpy(r.b, got, 2); vbi_valid++; }
    {
        std::lock_guard<std::mutex> l(bm);
        std::deque<VbiRes> &q = field == 1 ? q1 : q2;
        q.push_back(r);
        if (q.size() > 600) q.pop_front();
        vbi_last_t = t;
        bcv.notify_all();
    }
    if (probe_dump && vbi_bufs <= 8)
        logf("  vbi field %d #%lld: %d lines, t=%.1f ms, line 21 at index %d%s", field, (i64)vbi_bufs, nlines, t / 1e4, found,
             found >= 0 ? (std::string(" bytes ") + std::to_string(got[0]) + "," + std::to_string(got[1])).c_str() : "");
}

void Engine::cc_reset()
{
    std::lock_guard<std::mutex> l(bm);
    q1.clear(); q2.clear();
    vbi_done = false;
}

/* ---- a recording ------------------------------------------------------------------------------- */
class Session {
public:
    Session(Engine &e) : e(e) {}
    Engine &e;
    std::string out, ffmpeg_path, tmp_m2v, tmp_wav, error;
    double mbps = 7, peak = 9.8;
    bool cbr = false, keep_temp = false, ac3 = false, wide = false, no_cc = false, synthetic = false;
    int audio_kbps = 224;
    double vbi_offset_ms = 0;
    bool bff = true;

    /* geometry copied from the engine at start, so a restart of the capture cannot change it under us */
    int vw = 720, vh = 480;
    size_t vbytes = 0;
    std::string pixfmt = "yuyv422";
    double fps_num = 30000, fps_den = 1001;
    i64 frame_dur = 333667;
    AudioFmt af;
    bool have_audio = false, have_vbi = false;

    std::mutex pm;
    bool closed = false;
    std::mutex vm;
    std::condition_variable vcv;
    std::deque<VFrame> vq;
    std::atomic<bool> vdone{false};
    std::atomic<i64> v_in{0}, v_dropped_queue{0}, v_late{0}, v_repeated{0}, v_out{0}, m2v_bytes{0};
    std::atomic<i64> cc_f1{0}, cc_f2{0}, cc_frames{0};
    i64 t0v = -1;
    bool t0v_set = false, cc_synced = false;
    std::mutex am;
    FILE *wav = nullptr;
    i64 a_first = -1, a_end = 0, a_bytes = 0, a_gaps = 0;

    plat::Proc ff;
    std::thread t_vwrite, t_vread;
    FILE *m2v = nullptr;

    bool start();
    void push_video(i64 t, const uint8_t *p);
    void push_audio(i64 t, const uint8_t *p, long n);
    bool finish();      /* stops the pipeline and writes the final file */
    void abort_files();
    double seconds() const { return v_out * fps_den / fps_num; }

private:
    void video_writer();
    void es_reader();
    void cc_for_frame(uint8_t f1[2], bool &has1, uint8_t f2[2], bool &has2);
    bool mux();
};

void Engine::video(i64 t, const uint8_t *p, long n)
{
    cb_v++;
    if (!p || (size_t)n < vbytes) { if (cb_v_short++ < 3) logf("  short video sample: %ld of %zu bytes", n, vbytes); return; }
    last_video = plat::now_100ns();
    if (out && preview_on && (pv_count++ & 1) == 0) {
        if (pv_buf.size() != (size_t)vw * vh * 4) pv_buf.assign((size_t)vw * vh * 4, 0);
        packed422_to_bgrx(p, vw, vh, pixfmt == "uyvy422", pv_buf.data());
        out->frame(pv_buf.data(), vw, vh);
    }
    std::shared_ptr<Session> r;
    { std::lock_guard<std::mutex> l(rm); r = rec; }
    if (r) r->push_video(t, p);
}

void Engine::audio_in(i64 t, const uint8_t *p, long n)
{
    cb_a++;
    if (!p || n <= 0) return;
    if (audio.bits == 16 && audio.channels == 2) {
        const int16_t *a = (const int16_t *)(const void *)p;
        int frames = (int)(n / 4), ml = peak_l, mr = peak_r;
        for (int i = 0; i < frames; i++) {
            int l = abs((int)a[2 * i]), r = abs((int)a[2 * i + 1]);
            if (l > ml) ml = l;
            if (r > mr) mr = r;
        }
        peak_l = ml; peak_r = mr;
    }
    if (out && monitor_on) out->msg('A', p, (size_t)n);
    std::shared_ptr<Session> r;
    { std::lock_guard<std::mutex> l(rm); r = rec; }
    if (r) r->push_audio(t, p, n);
}


bool Session::start()
{
    ffmpeg_path = find_ffmpeg(ffmpeg_path);
    if (ffmpeg_path.empty()) { error = "ffmpeg not found. Put it next to the program."; return false; }
    vw = e.vw; vh = e.vh; vbytes = e.vbytes; pixfmt = e.pixfmt; fps_num = e.fps_num; fps_den = e.fps_den; frame_dur = e.frame_dur;
    af = e.audio; have_audio = e.have_audio; have_vbi = e.have_vbi && !no_cc;
    static std::atomic<int> serial{0};
    std::string base = plat::temp_dir() + "wintv-" + std::to_string(plat::pid()) + "-" + std::to_string(serial++);
    tmp_m2v = base + ".m2v";
    tmp_wav = base + ".wav";

    double rate = mbps;
    double pk = cbr ? rate : (peak > 0 ? peak : rate * 1.4);
    if (pk < rate) pk = rate;
    char fr[64];
    snprintf(fr, sizeof fr, "%d/%d", (int)fps_num, (int)fps_den);
    char buf[2048];
    snprintf(buf, sizeof buf,
             "%s -hide_banner -loglevel warning -f rawvideo -pix_fmt %s -video_size %dx%d -framerate %s -i - -an "
             "-vf setfield=%s,scale=interl=1 -pix_fmt yuv420p -c:v mpeg2video -b:v %dk -maxrate %dk -minrate %dk -bufsize %dk "
             "-bf 0 -g %d -flags +ildct+ilme -aspect %s -f mpeg2video -",
             plat::quote(ffmpeg_path).c_str(), pixfmt.c_str(), vw, vh, fr, bff ? "bff" : "tff", (int)(rate * 1000), (int)(pk * 1000),
             cbr ? (int)(rate * 1000) : 0, (int)(pk * 1000 * 1.2 > 1835 ? 1835 : pk * 1000 * 1.2), fps_num == 25 ? 12 : 15,
             wide ? "16:9" : "4:3");
    logf("video encoder: %.2f Mbit/s %s (peak %.2f)", rate, cbr ? "constant" : "variable", pk);
    if (!plat::spawn(buf, true, true, g_quiet, ff)) { error = "cannot start ffmpeg"; return false; }
    m2v = plat::fopen_utf8(tmp_m2v, "wb");
    if (!m2v) { error = "cannot write " + tmp_m2v; return false; }
    if (have_audio) { wav = plat::fopen_utf8(tmp_wav, "wb"); if (wav) wav_header(wav, af, 0); }
    t_vread = std::thread([this] { es_reader(); });
    t_vwrite = std::thread([this] { video_writer(); });
    return true;
}

void Session::push_video(i64 t, const uint8_t *p)
{
    std::lock_guard<std::mutex> pl(pm);
    if (closed) return;
    v_in++;
    VFrame f;
    f.t = t;
    f.px.assign(p, p + vbytes);
    std::lock_guard<std::mutex> l(vm);
    if (!synthetic && vq.size() > 240) { v_dropped_queue++; return; }
    vq.push_back(std::move(f));
    vcv.notify_one();
}

void Session::push_audio(i64 t, const uint8_t *p, long n)
{
    std::lock_guard<std::mutex> pl(pm);
    if (closed || !wav || n <= 0) return;
    std::lock_guard<std::mutex> l(am);
    if (a_first < 0) { a_first = t; a_end = t; }
    i64 gap = t - a_end; /* 100 ns of silence to insert if the driver skipped audio */
    if (gap > 300000 && gap < 100000000) {
        size_t bytes = (size_t)(gap * (i64)af.bytes_per_sec() / 10000000);
        bytes -= bytes % (size_t)af.block();
        std::vector<uint8_t> z(std::min<size_t>(bytes, 1 << 20), 0);
        for (size_t left = bytes; left;) { size_t k = std::min(left, z.size()); fwrite(z.data(), 1, k, wav); a_bytes += (i64)k; left -= k; }
        a_gaps++;
    }
    fwrite(p, 1, (size_t)n, wav);
    a_bytes += n;
    a_end = t + (i64)n * 10000000 / (i64)af.bytes_per_sec();
}

/* Caption bytes are a stream: dropping or repeating a pair garbles the text, so each frame takes one pair per
 * field from a FIFO in arrival order rather than matching time stamps (which jitter by more than a frame).
 * Only the starting point is aligned by time. */
void Session::cc_for_frame(uint8_t f1[2], bool &has1, uint8_t f2[2], bool &has2)
{
    has1 = has2 = false;
    if (!have_vbi) return;
    std::unique_lock<std::mutex> l(e.bm);
    if (!cc_synced) { /* discard VBI that predates the first video frame */
        i64 start = t0v + (i64)(vbi_offset_ms * 1e4) - frame_dur / 2;
        for (auto *q : { &e.q1, &e.q2 })
            while (!q->empty() && q->front().t < start) q->pop_front();
        cc_synced = true;
    }
    e.bcv.wait_for(l, std::chrono::milliseconds(400), [&] { return (!e.q1.empty() && !e.q2.empty()) || e.vbi_done.load(); });
    if (!e.q1.empty()) { VbiRes &r = e.q1.front(); if (r.valid) { has1 = true; memcpy(f1, r.b, 2); } e.q1.pop_front(); }
    if (!e.q2.empty()) { VbiRes &r = e.q2.front(); if (r.valid) { has2 = true; memcpy(f2, r.b, 2); } e.q2.pop_front(); }
}

void Session::es_reader()
{
    Injector inj;
    inj.next_block = [this](i64, uint8_t *blk) {
        uint8_t f1[2], f2[2];
        bool h1 = false, h2 = false;
        cc_for_frame(f1, h1, f2, h2);
        cc_frames++;
        if (h1) cc_f1++;
        if (h2) cc_f2++;
        cc608_pack_a53(blk, h1 ? f1 : nullptr, h2 ? f2 : nullptr);
    };
    std::vector<uint8_t> in(1 << 16), outb;
    for (;;) {
        long got = plat::read_out(ff, in.data(), in.size());
        if (got <= 0) break;
        outb.clear();
        inj.feed(in.data(), (size_t)got, outb, false);
        if (!outb.empty()) { fwrite(outb.data(), 1, outb.size(), m2v); m2v_bytes += (i64)outb.size(); }
    }
    outb.clear();
    inj.feed(nullptr, 0, outb, true);
    if (!outb.empty()) { fwrite(outb.data(), 1, outb.size(), m2v); m2v_bytes += (i64)outb.size(); }
}

void Session::video_writer()
{
    VFrame last;
    bool have_last = false;
    i64 next = 0;
    for (;;) {
        VFrame f;
        {
            std::unique_lock<std::mutex> l(vm);
            vcv.wait(l, [&] { return !vq.empty() || vdone.load(); });
            if (vq.empty()) break;
            f = std::move(vq.front());
            vq.pop_front();
            vcv.notify_all();
        }
        if (!t0v_set) { t0v = f.t; t0v_set = true; }
        i64 idx = (f.t - t0v + frame_dur / 2) / frame_dur;
        if (idx < next) { v_late++; continue; }
        auto put = [&](const std::vector<uint8_t> &px) {
            if (!plat::write_in(ff, px.data(), px.size())) return false;
            v_out++;
            return true;
        };
        bool ok = true;
        i64 fill = std::min<i64>(idx - next, 300);
        if (idx - next > 300) t0v += (idx - next - 300) * frame_dur; /* a long hole: give up filling it */
        while (ok && fill-- > 0 && have_last) { ok = put(last.px); v_repeated++; }
        if (!ok || !put(f.px)) break;
        next = idx + 1;
        last = std::move(f);
        have_last = true;
    }
    plat::close_in(ff);
}

bool Session::finish()
{
    { std::lock_guard<std::mutex> pl(pm); closed = true; }
    vdone = true;
    vcv.notify_all();
    if (t_vwrite.joinable()) t_vwrite.join();
    if (t_vread.joinable()) t_vread.join();
    e.vbi_done = true;
    e.bcv.notify_all();
    if (plat::wait(ff, 30000) < 0) plat::kill(ff);
    plat::close_out(ff);
    if (m2v) { fclose(m2v); m2v = nullptr; }
    if (wav) {
        std::lock_guard<std::mutex> l(am);
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, af, a_bytes);
        fclose(wav);
        wav = nullptr;
    }
    if (v_out == 0) { error = "no video was captured: is there a signal?"; abort_files(); return false; }
    bool ok = mux();
    if (!ok) error = "ffmpeg failed to write " + out;
    if (ok) abort_files();
    return ok;
}

void Session::abort_files()
{
    if (keep_temp) return;
    plat::remove_file(tmp_m2v);
    plat::remove_file(tmp_wav);
}

bool Session::mux()
{
    double v0 = t0v_set ? t0v / 1e7 : 0, a0 = a_first >= 0 ? a_first / 1e7 : 0;
    double skew = a0 - v0; /* audio started this much later (+) or earlier (-) than the video */
    char fr[64], b[160];
    snprintf(fr, sizeof fr, "%d/%d", (int)fps_num, (int)fps_den);
    std::string cmd = plat::quote(ffmpeg_path) + " -y -hide_banner -loglevel warning -framerate " + fr + " -f mpegvideo -i " + plat::quote(tmp_m2v);
    /* The raw stream has no timestamps. With no B-frames, PTS = DTS = frame number x frame duration; the
     * expression is in the packet time base (TB), which is not 1/90000 for this demuxer. */
    snprintf(b, sizeof b, " -bsf:v \"setts=ts=N*%d/%d/TB\"", (int)fps_den, (int)fps_num);
    std::string ts_bsf = b;
    if (have_audio && a_bytes > 0) {
        if (skew > 0.001) { snprintf(b, sizeof b, " -itsoffset %.3f", skew); cmd += b; }
        else if (skew < -0.001) { snprintf(b, sizeof b, " -ss %.3f", -skew); cmd += b; }
        cmd += " -i " + plat::quote(tmp_wav) + " -c:v copy" + ts_bsf;
        if (ac3) cmd += " -c:a ac3 -b:a " + std::to_string(audio_kbps) + "k";
        else cmd += " -c:a mp2 -b:a " + std::to_string(audio_kbps) + "k";
        cmd += " -ar 48000 -shortest";
    } else cmd += " -c:v copy" + ts_bsf;
    bool ts = out.size() > 3 && icontains(out.substr(out.size() - 3), ".ts");
    cmd += ts ? " -f mpegts " : " -f mpeg ";
    cmd += plat::quote(out);
    plat::Proc p;
    if (!plat::spawn(cmd, false, false, g_quiet, p)) return false;
    return plat::wait(p, -1) == 0;
}

/* ---- synthetic source for --selftest ------------------------------------------------------------ */
static uint8_t cc_byte(uint8_t c) { c &= 0x7F; return cc608_parity_ok(c) ? c : (uint8_t)(c | 0x80); }

static bool run_selftest(Engine &e, Session &s, double secs)
{
    e.have_vbi = true;
    e.have_audio = true;
    e.audio = AudioFmt();
    e.pixfmt = "yuyv422";
    if (e.o.pal) { e.fps_num = 25; e.fps_den = 1; e.frame_dur = 400000; e.vh = 576; }
    e.vbytes = (size_t)e.vw * e.vh * 2;
    s.synthetic = true;
    e.cc_reset();
    if (!s.start()) return false;
    std::vector<std::pair<uint8_t, uint8_t>> seq;
    auto ctl = [&](uint8_t a, uint8_t b) { seq.push_back({ cc_byte(a), cc_byte(b) }); seq.push_back({ cc_byte(a), cc_byte(b) }); };
    ctl(0x14, 0x20); ctl(0x14, 0x70);
    const char *text = "HELLO WORLD ";
    for (int i = 0; text[i]; i += 2) seq.push_back({ cc_byte((uint8_t)text[i]), cc_byte((uint8_t)text[i + 1]) });
    ctl(0x14, 0x2F);
    i64 frames = (i64)(secs * e.fps_num / e.fps_den), tone = 0;
    std::vector<uint8_t> px(e.vbytes);
    for (i64 i = 0; i < frames && !g_stop; i++) {
        static const uint8_t Y[8] = { 235, 210, 170, 145, 106, 81, 41, 16 };
        for (int y = 0; y < e.vh; y++)
            for (int x = 0; x < e.vw; x += 2) {
                int bar = ((x + (int)i * 4) * 8 / e.vw) & 7;
                size_t o2 = ((size_t)y * e.vw + x) * 2;
                px[o2] = Y[bar]; px[o2 + 1] = (uint8_t)(128 + bar * 5); px[o2 + 2] = Y[bar]; px[o2 + 3] = (uint8_t)(128 - bar * 5);
            }
        i64 t = i * e.frame_dur + 50000000;
        {   /* the synthetic source waits for the encoder instead of dropping */
            std::unique_lock<std::mutex> l(s.vm);
            s.vcv.wait(l, [&] { return s.vq.size() < 30; });
        }
        s.push_video(t, px.data());
        for (int fld = 1; fld <= 2; fld++) {
            VbiRes r{};
            r.t = t + (fld - 1) * e.frame_dur / 2 + e.frame_dur / 8;
            if (fld == 1) {
                i64 k = i - 20; /* caption data starts at frame 20 */
                if (k >= 0 && k < (i64)seq.size()) { r.valid = true; r.b[0] = seq[(size_t)k].first; r.b[1] = seq[(size_t)k].second; }
            }
            std::lock_guard<std::mutex> l(e.bm);
            (fld == 1 ? e.q1 : e.q2).push_back(r);
            e.vbi_last_t = r.t;
            e.bcv.notify_all();
        }
        int n = (int)(e.audio.rate * e.fps_den / e.fps_num);
        std::vector<int16_t> a((size_t)n * 2);
        for (int k = 0; k < n; k++) a[(size_t)2 * k] = a[(size_t)2 * k + 1] = (int16_t)(8000 * sin(2 * M_PI * 440.0 * (double)(tone + k) / e.audio.rate));
        tone += n;
        s.push_audio(t, (const uint8_t *)a.data(), (long)a.size() * 2);
        plat::sleep_ms(2);
    }
    return true;
}

/* ---- serve mode: the GUI's helper ---------------------------------------------------------------- */
/* Status states match app_state_t in app_core.h: 0 searching, 1 no driver, 2 starting, 3 no signal, 4 streaming. */
static void configure_session(Session &s, const Options &o)
{
    double mbps = o.mbps;
    if (mbps <= 0) mbps = preset_mbps(o.quality);
    s.out = o.out; s.ffmpeg_path = o.ffmpeg; s.mbps = mbps; s.peak = o.peak; s.cbr = o.cbr; s.ac3 = o.ac3; s.wide = o.wide;
    s.no_cc = o.no_cc; s.audio_kbps = o.audio_kbps; s.bff = o.bff; s.keep_temp = o.keep_temp; s.vbi_offset_ms = o.vbi_offset_ms;
}

static void serve(Options o, Backend &be)
{
    g_quiet = true;
    Out out;
    out.start();
    Engine e;
    e.o = o;
    e.out = &out;
    e.be = &be;

    std::mutex cm;
    std::deque<std::string> cmds;
    std::atomic<bool> quit{false};
    std::thread reader([&] {
        std::string line;
        char c;
        while (plat::stdin_read(&c, 1) == 1) {
            if (c == '\n') {
                std::lock_guard<std::mutex> l(cm);
                cmds.push_back(line);
                line.clear();
            } else if (c != '\r') line += c;
        }
        quit = true; /* the GUI went away */
    });

    bool want_open = false, restart = false;
    int state = -1;
    std::string state_msg, ffpath = o.ffmpeg;
    i64 last_try = 0, open_at = 0, last_lvl = 0, last_prog = 0, last_state_send = 0;
    std::vector<std::thread> finishers;
    auto send_state = [&](int st, const std::string &msg) {
        if (st == state && msg == state_msg && plat::now_100ns() - last_state_send < 20000000) return;
        state = st; state_msg = msg;
        last_state_send = plat::now_100ns();
        out.text('S', fmt_str("state=%d;w=%d;h=%d;fps=%.2f;wide=%d;msg=%s", st, e.vw, e.vh, e.fps_num / e.fps_den, e.o.wide ? 1 : 0, msg.c_str()));
    };
    auto stop_record = [&](std::shared_ptr<Session> r) {
        finishers.emplace_back([r, &out] {
            bool ok = r->finish();
            out.text('R', fmt_str("ok=%d;path=%s;err=%s;frames=%lld;cc1=%lld;cc2=%lld;cc=%lld", ok ? 1 : 0, r->out.c_str(),
                                  ok ? "" : r->error.c_str(), (i64)r->v_out, (i64)r->cc_f1, (i64)r->cc_f2, (i64)r->cc_frames));
        });
    };

    while (!quit && !g_stop) {
        for (;;) {
            std::string c;
            { std::lock_guard<std::mutex> l(cm); if (cmds.empty()) break; c = cmds.front(); cmds.pop_front(); }
            size_t sp = c.find(' ');
            std::string k = c.substr(0, sp), v = sp == std::string::npos ? "" : c.substr(sp + 1);
            if (k == "source") { e.o.source = v; if (e.is_open) { be.route(e); be.tune(e); } }
            else if (k == "channel") { e.o.channel = atoi(v.c_str()); if (e.is_open) be.tune(e); }
            else if (k == "cable") { e.o.cable = v == "1"; if (e.is_open) be.tune(e); }
            else if (k == "pal") { bool p = v == "1"; if (p != e.o.pal) { e.o.pal = p; restart = e.is_open; } }
            else if (k == "wide") e.o.wide = v == "1";
            else if (k == "quality") e.o.quality = v;
            else if (k == "bitrate") e.o.mbps = atof(v.c_str());
            else if (k == "cbr") e.o.cbr = v == "1";
            else if (k == "audiokbps") e.o.audio_kbps = atoi(v.c_str());
            else if (k == "cc") e.o.no_cc = v == "0";
            else if (k == "preview") e.preview_on = v == "1";
            else if (k == "monitor") e.monitor_on = v == "1";
            else if (k == "ffmpeg") ffpath = v;
            else if (k == "start") want_open = true;
            else if (k == "retry") last_try = 0;
            else if (k == "record") {
                auto s = std::make_shared<Session>(e);
                configure_session(*s, e.o);
                s->out = v;
                s->ffmpeg_path = ffpath;
                bool busy;
                { std::lock_guard<std::mutex> l(e.rm); busy = e.rec != nullptr; }
                if (!e.is_open || state != 4) out.text('R', "ok=0;path=;err=There is no picture to record yet;frames=0;cc1=0;cc2=0;cc=0");
                else if (busy) out.text('R', "ok=0;path=;err=Already recording;frames=0;cc1=0;cc2=0;cc=0");
                else {
                    e.cc_reset();
                    if (!s->start()) out.text('R', fmt_str("ok=0;path=;err=%s;frames=0;cc1=0;cc2=0;cc=0", s->error.c_str()));
                    else { std::lock_guard<std::mutex> l(e.rm); e.rec = s; out.text('K', "recording"); }
                }
            } else if (k == "stop") {
                std::shared_ptr<Session> r;
                { std::lock_guard<std::mutex> l(e.rm); r.swap(e.rec); }
                if (r) stop_record(r);
            } else if (k == "quit") quit = true;
        }

        i64 now = plat::now_100ns();
        if (restart) { restart = false; std::shared_ptr<Session> r; { std::lock_guard<std::mutex> l(e.rm); r.swap(e.rec); } if (r) stop_record(r); e.close(); last_try = 0; }

        if (want_open && !e.is_open && now - last_try > 20000000) {
            last_try = now;
            send_state(2, "Starting the tuner...");
            if (e.open() && e.run()) { open_at = plat::now_100ns(); send_state(2, "Waiting for a picture..."); }
            else {
                std::string msg = e.error.empty() ? "Cannot open the tuner" : e.error;
                bool missing = msg.find("Plug") != std::string::npos || msg.find("No capture") != std::string::npos;
                if (e.is_open) e.close(); else be.close(e);
                send_state(missing ? 0 : 1, msg);
            }
        }
        if (e.is_open) {
            i64 lv = e.last_video.load();
            bool flowing = lv && now - lv < 25000000;
            if (flowing) send_state(4, "");
            else if (now - open_at > 30000000) send_state(3, "No signal. Is a source on this channel?");
            if (now - last_lvl > 500000) { /* levels, 20 times a second */
                last_lvl = now;
                float l = e.peak_l.exchange(0) / 32768.0f, r = e.peak_r.exchange(0) / 32768.0f;
                float db[2] = { l > 0.001f ? 20 * log10f(l) : -60.f, r > 0.001f ? 20 * log10f(r) : -60.f };
                out.msg('L', db, sizeof db);
            }
            if (now - last_prog > 5000000) {
                last_prog = now;
                std::shared_ptr<Session> r;
                { std::lock_guard<std::mutex> l(e.rm); r = e.rec; }
                if (r) {
                    struct { double sec; uint64_t bytes; } pr = { r->seconds(), (uint64_t)(r->m2v_bytes.load() + r->a_bytes / 6) };
                    out.msg('P', &pr, sizeof pr);
                }
            }
        }
        plat::sleep_ms(20);
    }
    { std::shared_ptr<Session> r; { std::lock_guard<std::mutex> l(e.rm); r.swap(e.rec); } if (r) stop_record(r); }
    for (auto &t : finishers) if (t.joinable()) t.join();
    if (e.is_open) e.close();
    out.stop();
    plat::hard_exit(0); /* the stdin reader is blocked in a read */
}

/* ---- a test pattern "tuner" (--device synthetic) ------------------------------------------------- *
 * Colour bars, a tone and a caption that changes every two seconds, through the real Engine, so the whole
 * helper (preview, levels, recording, the --serve protocol) can be tried without a tuner on any platform. */
class SynthBackend : public Backend {
public:
    std::thread th;
    std::atomic<bool> running{false};
    Engine *E = nullptr;

    bool open(Engine &e) override
    {
        E = &e;
        e.vw = 720; e.vh = e.o.pal ? 576 : 480; e.vbytes = (size_t)e.vw * e.vh * 2; e.pixfmt = "yuyv422";
        if (e.o.pal) { e.fps_num = 25; e.fps_den = 1; e.frame_dur = 400000; } else { e.fps_num = 30000; e.fps_den = 1001; e.frame_dur = 333667; }
        e.audio = AudioFmt();
        e.have_audio = e.have_vbi = true;
        return true;
    }
    bool run(Engine &e) override
    {
        running = true;
        th = std::thread([this, &e] { loop(e); });
        return true;
    }
    void stop(Engine &) override { running = false; if (th.joinable()) th.join(); }
    void close(Engine &e) override { stop(e); }
    bool route(Engine &) override { return true; }
    bool tune(Engine &) override { return true; }
    void list_devices() override { printf("Video capture devices:\n  synthetic (test pattern)\n"); }

private:
    void loop(Engine &e)
    {
        std::vector<uint8_t> px(e.vbytes);
        static const uint8_t Y[8] = { 235, 210, 170, 145, 106, 81, 41, 16 };
        std::deque<std::pair<uint8_t, uint8_t>> pending; /* caption bytes still to send, one pair per frame */
        i64 start = plat::now_100ns(), frames = 0, tone = 0;
        while (running) {
            i64 t = start + frames * e.frame_dur;
            for (int y = 0; y < e.vh; y++)
                for (int x = 0; x < e.vw; x += 2) {
                    int bar = ((x + (int)frames * 4) * 8 / e.vw) & 7;
                    size_t o2 = ((size_t)y * e.vw + x) * 2;
                    px[o2] = Y[bar]; px[o2 + 1] = (uint8_t)(128 + bar * 5); px[o2 + 2] = Y[bar]; px[o2 + 3] = (uint8_t)(128 - bar * 5);
                }
            e.video(t, px.data(), (long)px.size());
            int n = (int)(e.audio.rate * e.fps_den / e.fps_num);
            std::vector<int16_t> a((size_t)n * 2);
            for (int k = 0; k < n; k++) a[(size_t)2 * k] = a[(size_t)2 * k + 1] = (int16_t)(6000 * sin(2 * M_PI * 440.0 * (double)(tone + k) / e.audio.rate));
            tone += n;
            e.audio_in(t, (const uint8_t *)a.data(), (long)a.size() * 2);
            if (pending.empty() && frames % 60 == 0) { /* a new pop-on caption */
                auto ctl = [&](uint8_t x, uint8_t y2) { pending.push_back({ cc_byte(x), cc_byte(y2) }); pending.push_back({ cc_byte(x), cc_byte(y2) }); };
                ctl(0x14, 0x20); ctl(0x14, 0x70);
                std::string text = fmt_str("SYNTHETIC CAPTION %02d ", (int)(frames / 60) % 100);
                if (text.size() % 2) text += ' ';
                for (size_t i = 0; i < text.size(); i += 2) pending.push_back({ cc_byte((uint8_t)text[i]), cc_byte((uint8_t)text[i + 1]) });
                ctl(0x14, 0x2F);
            }
            bool have = !pending.empty();
            e.cc_push(1, t, have, have ? pending.front().first : 0, have ? pending.front().second : 0);
            if (have) pending.pop_front();
            e.cc_push(2, t, false, 0, 0);
            e.cb_b++;
            frames++;
            i64 due = start + frames * e.frame_dur, now = plat::now_100ns();
            if (due > now) plat::sleep_ms((int)((due - now) / 10000));
        }
    }
};

/* ---- command line ------------------------------------------------------------------------------- */
static std::string timestamped(const std::string &dir)
{
    time_t t = time(0);
    struct tm tm;
    plat::local_time(t, &tm);
    char b[128];
    strftime(b, sizeof b, "WinTV %Y-%m-%d at %H.%M.%S.mpg", &tm);
    std::string d = dir;
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += plat::dir_sep();
    return d + b;
}

static void wait_until(const Options &o)
{
    time_t now = time(0);
    struct tm tm;
    plat::local_time(now, &tm);
    tm.tm_hour = o.start_h; tm.tm_min = o.start_m; tm.tm_sec = o.start_s;
    time_t at = mktime(&tm);
    if (at <= now) at += 24 * 3600;
    char b[64];
    struct tm t2;
    plat::local_time(at, &t2);
    strftime(b, sizeof b, "%a %H:%M:%S", &t2);
    logf("waiting until %s to start (Ctrl-C cancels)", b);
    while (!g_stop && time(0) < at) plat::sleep_ms(200);
}

[[maybe_unused]] static int tv_main(int argc, char **argv, Backend &real_be)
{
    Options o;
    const char *prog = argc > 0 ? argv[0] : "tv-capture";
    if (!parse_args(argc, argv, o)) { usage(prog); return 2; }
    if (o.help) { usage(prog); return 0; }
    plat::install_stop_handler(&g_stop);
    SynthBackend synth;
    Backend &be = o.device == "synthetic" ? static_cast<Backend &>(synth) : real_be;
    if (o.list) { be.list_devices(); return 0; }
    if (o.serve) { serve(o, be); return 0; }
    if (o.source != "tuner" && o.source != "composite" && o.source != "svideo") { logf("-s must be tuner, composite or svideo"); return 2; }
    if (o.mbps < 0 || o.mbps > 15) { logf("-b must be 1-15"); return 2; }
    if (o.mbps == 0) {
        bool known = false;
        for (auto &p : PRESETS) if (o.quality == p.name) known = true;
        if (!known) { logf("-q must be low, medium, high or best"); return 2; }
    }
    if (o.out.empty()) o.out = timestamped(o.dir);

    Engine e;
    e.o = o;
    e.be = &be;
    e.probe_dump = o.probe > 0;

    if (o.selftest > 0) {
        Session s(e);
        configure_session(s, o);
        if (!run_selftest(e, s, o.selftest)) { logf("selftest failed: %s", s.error.c_str()); s.finish(); return 1; }
        bool ok = s.finish();
        logf("selftest: %lld frames, captions in %lld frame(s)", (i64)s.v_out, (i64)s.cc_f1);
        logf(ok ? "saved %s" : "failed: %s", ok ? o.out.c_str() : s.error.c_str());
        return ok ? 0 : 1;
    }
    if (o.scan) {
        e.o.source = "tuner"; e.o.channel = 2;
        if (!e.open() || !be.has_tuner(e)) { logf("no tuner: %s", e.error.c_str()); return 1; }
        e.run();
        for (int ch = 2; ch <= 99 && !g_stop; ch++) {
            i64 v0 = e.cb_v, b0 = e.cb_b, a0 = e.cb_a;
            e.o.channel = ch;
            be.tune(e);
            plat::sleep_ms(700);
            i64 dv = e.cb_v - v0, db = e.cb_b - b0, da = e.cb_a - a0;
            if (dv > 2 || db > 2 || da > 2) printf("channel %d: video %lld, vbi %lld, audio %lld callbacks\n", ch, dv, db, da);
        }
        printf("scan done\n");
        e.close();
        return 0;
    }

    if (o.start_h >= 0 && o.probe <= 0) { wait_until(o); if (g_stop) return 1; }
    if (!e.open() || !e.run()) { logf("%s", e.error.c_str()); if (e.is_open) e.close(); else be.close(e); return 1; }
    std::shared_ptr<Session> s;
    if (o.probe <= 0) {
        s = std::make_shared<Session>(e);
        configure_session(*s, o);
        e.cc_reset();
        if (!s->start()) { logf("%s", s->error.c_str()); e.close(); return 1; }
        std::lock_guard<std::mutex> l(e.rm);
        e.rec = s;
    }
    double limit = o.probe > 0 ? o.probe : o.seconds;
    logf("recording to %s%s (Ctrl-C to stop)", o.probe > 0 ? "(probe)" : o.out.c_str(), limit > 0 ? "" : ", until stopped");
    i64 t_begin = plat::now_100ns(), last_report = t_begin;
    while (!g_stop) {
        plat::sleep_ms(100);
        i64 now = plat::now_100ns();
        double el = (now - t_begin) / 1e7;
        i64 vout = s ? (i64)s->v_out : (i64)e.cb_v;
        if (limit > 0 && vout >= (i64)(limit * e.fps_num / e.fps_den)) break;
        if (limit > 0 && el > limit + 10) break; /* no video arriving */
        if (s && now - last_report > 50000000) {
            last_report = now;
            fprintf(stderr, "\r  %02d:%02d:%02d  frames %lld (repeated %lld, late %lld)  captions in %lld/%lld frames   ", (int)(el / 3600),
                    (int)(el / 60) % 60, (int)el % 60, (i64)s->v_out, (i64)s->v_repeated, (i64)s->v_late, (i64)s->cc_f1, (i64)s->cc_frames);
        }
    }
    fprintf(stderr, "\n");
    { std::lock_guard<std::mutex> l(e.rm); e.rec.reset(); }
    be.stop(e);
    logf("callbacks: video %lld (%lld too short), audio %lld, vbi %lld", (i64)e.cb_v, (i64)e.cb_v_short, (i64)e.cb_a, (i64)e.cb_b);
    if (!s) { e.close(); return 0; }
    bool ok = s->finish();
    logf("frames: %lld in, %lld written, %lld repeated to fill gaps, %lld late, %lld dropped (queue full)", (i64)s->v_in, (i64)s->v_out,
         (i64)s->v_repeated, (i64)s->v_late, (i64)s->v_dropped_queue);
    if (s->have_audio) logf("audio: %.1f s, %lld gaps filled", s->a_bytes / (double)std::max(s->af.bytes_per_sec(), 1), (i64)s->a_gaps);
    if (s->have_vbi)
        logf("captions: %lld VBI buffers, frames with field 1 data: %lld, field 2: %lld of %lld", (i64)e.vbi_bufs, (i64)s->cc_f1, (i64)s->cc_f2, (i64)s->cc_frames);
    if (ok) logf("saved %s", o.out.c_str());
    else logf("%s", s->error.c_str());
    if (ok && !o.no_cc && s->have_vbi && s->cc_f1 == 0 && s->cc_f2 == 0)
        logf("warning: no captions were found on the input. Does the source carry line 21 captions, and is the cable good?");
    e.close();
    return ok ? 0 : 1;
}

#endif
