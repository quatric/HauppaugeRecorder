/* HauppaugeRecorder engine; see app_core.h. */
#include "app_core.h"
#include "decoder.h"
#include "hdpvr.h"
#include "monitor_ring.h"
#include "sink.h"
#if !defined(__APPLE__) || defined(LEGACY_MAC) /* the old-Mac build plays through Core Audio here */
#include "sysaudio.h"
#endif

#include <libusb.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#define DIR_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#define DIR_SEP '/'
#endif

/* Monotonic clock. clock_gettime() only exists from macOS 10.12, so macOS
   uses mach_absolute_time(), which is there on every version. */
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
static void mono_ts(struct timespec *ts)
{
#ifdef __APPLE__
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    uint64_t ns = t / tb.denom * tb.numer + t % tb.denom * tb.numer / tb.denom;
    ts->tv_sec = (time_t)(ns / 1000000000u);
    ts->tv_nsec = (long)(ns % 1000000000u);
#else
    clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

#define RING_FRAMES 32768      /* power of two, ~680 ms at 48 kHz */
/* Decoded preview frames are queued and shown one per frame interval, because
 * ffmpeg hands them over in bursts as the USB data arrives. */
#define FRAME_QUEUE 8
#define QUEUE_KEEP 5 /* more queued than this: the preview has fallen behind, skip ahead */
#define QUEUE_START 3 /* frames in hand before the preview starts playing */
#define AUDIO_RATE 48000
/* ffmpeg's output in the CPU's own byte order: pixels as 32-bit 0x00RRGGBB, samples as int16 */
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define PIX_FMT "0rgb"
#define PCM_FMT "s16be"
#else
#define PIX_FMT "bgr0"
#define PCM_FMT "s16le"
#endif
#define REC_QUEUE (64u << 20)  /* ~25 s at the highest bitrate, for a slow disk */
#define RESTART_GAP_MS 4000    /* the firmware wants this long between a stop and the next start */
#define RETRY_MS 2000

const char *const app_quality_names[APP_QUALITIES] = { "Standard (6.5 Mbit/s)", "High (10 Mbit/s)",
                                                       "Maximum (13.5 Mbit/s)" };
const char *const app_video_names[3] = { "Component", "S-Video", "Composite" };
const char *const app_audio_names[3] = { "Back RCA", "Front RCA", "S/PDIF" };
static const int quality_kbps[APP_QUALITIES] = { 6500, 10000, 13500 };

typedef struct {
    int video, audio, quality, pal;
} source_t;

struct app {
    pthread_mutex_t lock; /* state, status, source, signal */
    pthread_cond_t cv;
    pthread_t worker;
    volatile int quit;
    volatile int retry, reconfig;
    source_t src;
    app_state_t state;
    char status[256], problem[256];
    int sig_w, sig_h, sig_fps;
    hdpvr_t *dev;

    pthread_mutex_t data_lock; /* the consumers of the stream */
    decoder_t *vdec, *adec;
    sink_t *rec;
    FILE *rec_file;
    int rec_synced;
    int rec_wide; /* SD widescreen: the file's H.264 is flagged 16:9 (see wide_packet) */
    int64_t rec_wait_ms;
    uint8_t pkt[188], pat[188], pmt[188];
    int pkt_n, have_pat, have_pmt, pmt_pid;
    struct timespec rec_t0;
    char rec_path[1024];

    pthread_mutex_t frame_lock;
    uint32_t *rgb;          /* the frame on screen */
    uint32_t *q[FRAME_QUEUE]; /* decoded frames waiting for their turn */
    int q_head, q_count, q_playing;
    int64_t q_due_us, period_us;
    int fw, fh, hd;
    uint64_t seq;
    volatile int widescreen;
    volatile int preview;   /* 0: the picture is not decoded (recording and sound go on) */
    int dec_sw, dec_sh, dec_fps; /* what the decoders were last started for */

    int16_t ring_frames[RING_FRAMES * 2];
    monitor_ring_t ring;
    volatile int peak[2];
    volatile int monitor;
#if !defined(__APPLE__) || defined(LEGACY_MAC)
    sa_output_t *out;
#endif
};

/* ---- small helpers ---- */

static int64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
#else
    struct timespec ts;
    mono_ts(&ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static int64_t now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    mono_ts(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void set_state(app_t *a, app_state_t st, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_state(app_t *a, app_state_t st, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&a->lock);
    a->state = st;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&a->lock);
}

static void set_problem(app_t *a, const char *s)
{
    pthread_mutex_lock(&a->lock);
    snprintf(a->problem, sizeof(a->problem), "%s", s ? s : "");
    pthread_mutex_unlock(&a->lock);
}

/* Sleeps up to ms; returns early on quit, app_retry() or a source change. */
static void nap(app_t *a, int ms)
{
#ifdef _WIN32
    int64_t until = now_ms() + ms;
    pthread_mutex_lock(&a->lock);
    while (!a->quit && !a->retry && !a->reconfig && now_ms() < until) {
        pthread_mutex_unlock(&a->lock);
        Sleep(20);
        pthread_mutex_lock(&a->lock);
    }
    a->retry = 0;
    pthread_mutex_unlock(&a->lock);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long ns = (long long)tv.tv_usec * 1000 + (long long)ms * 1000000;
    struct timespec until = { tv.tv_sec + (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
    pthread_mutex_lock(&a->lock);
    while (!a->quit && !a->retry && !a->reconfig)
        if (pthread_cond_timedwait(&a->cv, &a->lock, &until))
            break;
    a->retry = 0;
    pthread_mutex_unlock(&a->lock);
#endif
}

/* ---- consumers of the transport stream ---- */

/* A recording starts at a video packet that carries an SPS (the start of a
 * GOP the box can be decoded from), with the latest PAT and PMT in front, so
 * players can show it from the first frame. The scanner below follows the
 * stream packet by packet, across USB transfer boundaries, for that. */
static void ts_packet(app_t *a, const uint8_t *p, int *start_here)
{
    int pid = (p[1] & 0x1f) << 8 | p[2], pusi = p[1] & 0x40, afc = p[3] >> 4 & 3;
    int off = 4 + (afc & 2 ? 1 + p[4] : 0);
    if (!(afc & 1) || off >= 188)
        return;
    const uint8_t *pl = p + off;
    int n = 188 - off;
    if (pid == 0 && pusi) {
        memcpy(a->pat, p, 188);
        int sec = 1 + pl[0];
        if (sec + 12 <= n && pl[sec] == 0x00) {
            int len = (pl[sec + 1] & 0x0f) << 8 | pl[sec + 2];
            for (int i = sec + 8; i + 4 <= sec + 3 + len - 4 && i + 4 <= n; i += 4)
                if (pl[i] << 8 | pl[i + 1]) {
                    a->pmt_pid = (pl[i + 2] & 0x1f) << 8 | pl[i + 3];
                    break;
                }
        }
        a->have_pat = 1;
        return;
    }
    if (a->pmt_pid && pid == a->pmt_pid && pusi) {
        memcpy(a->pmt, p, 188);
        a->have_pmt = 1;
        return;
    }
    if (start_here && pusi && n > 9 && !pl[0] && !pl[1] && pl[2] == 1 && (pl[3] & 0xf0) == 0xe0) {
        for (int i = 9 + pl[8]; i + 3 < n; i++)
            if (!pl[i] && !pl[i + 1] && pl[i + 2] == 1 && (pl[i + 3] & 0x1f) == 7) {
                *start_here = 1;
                return;
            }
    }
}

/* ---- widescreen flag ----
 * The HD PVR flags SD video 4:3 (sample aspect 8:9 at 720x480). With
 * "Widescreen" on, the recording's SPS gets the 16:9 sample aspect instead
 * (32:27), so players show it wide. Both are fixed 16-bit fields in the VUI, so
 * the SPS keeps its size and the TS packet is patched in place. */

typedef struct {
    const uint8_t *b;
    int n, pos; /* in bits */
} bits_t;

static unsigned get_bits(bits_t *r, int k)
{
    unsigned v = 0;
    while (k--) {
        if (r->pos >= r->n)
            return r->pos = r->n + 1, v; /* past the end: the caller checks */
        v = v << 1 | (r->b[r->pos >> 3] >> (7 - (r->pos & 7)) & 1);
        r->pos++;
    }
    return v;
}

static unsigned get_ue(bits_t *r)
{
    int z = 0;
    while (z < 32 && r->pos < r->n && !get_bits(r, 1))
        z++;
    return z >= 32 ? 0 : (1u << z) - 1 + get_bits(r, z);
}

static void skip_scaling_list(bits_t *r, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size && r->pos <= r->n; j++) {
        if (next) {
            unsigned u = get_ue(r);
            int delta = u & 1 ? (int)(u + 1) / 2 : -(int)(u / 2);
            next = (last + delta + 256) % 256;
        }
        last = next ? next : last;
    }
}

/* nal points at the SPS NAL header, len bytes of it are in hand. 1 if patched. */
static int sps_make_wide(uint8_t *nal, int len)
{
    uint8_t rbsp[96];
    int map[96], n = 0, zeros = 0;
    for (int i = 1; i < len && n < (int)sizeof(rbsp); i++) {
        if (zeros >= 2 && nal[i] == 3) { /* emulation prevention byte */
            zeros = 0;
            continue;
        }
        if (zeros >= 2 && nal[i] <= 2)
            break; /* the next start code */
        zeros = nal[i] ? 0 : zeros + 1;
        map[n] = i;
        rbsp[n++] = nal[i];
    }
    bits_t r = { rbsp, n * 8, 0 };
    unsigned profile = get_bits(&r, 8);
    get_bits(&r, 16); /* constraints, level */
    get_ue(&r);       /* sps id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        unsigned chroma = get_ue(&r);
        if (chroma == 3)
            get_bits(&r, 1);
        get_ue(&r), get_ue(&r); /* bit depths */
        get_bits(&r, 1);
        if (get_bits(&r, 1)) /* scaling matrices */
            for (int i = 0; i < (chroma == 3 ? 12 : 8); i++)
                if (get_bits(&r, 1))
                    skip_scaling_list(&r, i < 6 ? 16 : 64);
    }
    get_ue(&r); /* log2 max frame num */
    unsigned poc = get_ue(&r);
    if (poc == 0) {
        get_ue(&r);
    } else if (poc == 1) {
        get_bits(&r, 1);
        get_ue(&r), get_ue(&r);
        for (unsigned i = 0, k = get_ue(&r); i < k && i < 256 && r.pos <= r.n; i++)
            get_ue(&r);
    }
    get_ue(&r);       /* ref frames */
    get_bits(&r, 1);
    get_ue(&r), get_ue(&r); /* size in macroblocks */
    if (!get_bits(&r, 1)) /* not frame_mbs_only */
        get_bits(&r, 1);
    get_bits(&r, 1);
    if (get_bits(&r, 1)) /* cropping */
        get_ue(&r), get_ue(&r), get_ue(&r), get_ue(&r);
    if (!get_bits(&r, 1) || !get_bits(&r, 1)) /* no VUI, or no aspect in it */
        return 0;
    int at = r.pos;
    unsigned idc = get_bits(&r, 8);
    if (r.pos > r.n)
        return 0;
    /* the 4:3 sample aspects of SD and their 16:9 counterparts */
    static const unsigned table[][4] = { { 8, 9, 32, 27 }, { 10, 11, 40, 33 }, { 12, 11, 16, 11 }, { 16, 15, 64, 45 } };
    if (idc == 255) {
        unsigned sw = get_bits(&r, 16), sh = get_bits(&r, 16);
        if (r.pos > r.n)
            return 0;
        int i = 0;
        while (i < 4 && !(table[i][0] == sw && table[i][1] == sh))
            i++;
        if (i == 4)
            return 0; /* already wide, or not SD */
        sw = table[i][2], sh = table[i][3];
        for (int k = 0; k < 32; k++) { /* write the two fields back bit by bit */
            int bit = k < 16 ? sw >> (15 - k) & 1 : sh >> (31 - k) & 1, p = at + 8 + k;
            rbsp[p >> 3] = (uint8_t)((rbsp[p >> 3] & ~(0x80 >> (p & 7))) | bit << (7 - (p & 7)));
        }
    } else if (idc == 3 || idc == 2) { /* 10:11 -> 40:33, 12:11 -> 16:11 */
        unsigned v = idc + 2;
        for (int k = 0; k < 8; k++) {
            int bit = v >> (7 - k) & 1, p = at + k;
            rbsp[p >> 3] = (uint8_t)((rbsp[p >> 3] & ~(0x80 >> (p & 7))) | bit << (7 - (p & 7)));
        }
    } else {
        return 0;
    }
    /* the new bytes must not need emulation prevention where the old ones did not */
    for (int i = 0; i + 2 < n; i++)
        if (!rbsp[i] && !rbsp[i + 1] && rbsp[i + 2] <= 3 && map[i + 2] == map[i] + 2)
            return 0;
    for (int i = 0; i < n; i++)
        nal[map[i]] = rbsp[i];
    return 1;
}

/* Finds an SPS in a video TS packet that starts a PES and flags it 16:9. */
static void wide_packet(uint8_t *p)
{
    int pusi = p[1] & 0x40, afc = p[3] >> 4 & 3;
    int off = 4 + (afc & 2 ? 1 + p[4] : 0);
    if (!pusi || !(afc & 1) || off + 9 >= 188)
        return;
    uint8_t *pl = p + off;
    int n = 188 - off;
    if (pl[0] || pl[1] || pl[2] != 1 || (pl[3] & 0xf0) != 0xe0)
        return;
    for (int i = 9 + pl[8]; i + 3 < n; i++)
        if (!pl[i] && !pl[i + 1] && pl[i + 2] == 1 && (pl[i + 3] & 0x1f) == 7) {
            sps_make_wide(pl + i + 3, n - i - 3);
            return;
        }
}

static void on_data(const uint8_t *data, size_t len, void *user)
{
    app_t *a = user;
    pthread_mutex_lock(&a->data_lock);
    /* where this chunk's raw copy to the file begins; a widescreen recording is
     * written packet by packet instead, so its SPS can be patched */
    size_t raw_from = a->rec && a->rec_synced && !a->rec_wide ? 0 : len;
    for (size_t i = 0; i < len; i++) {
        if (!a->pkt_n && data[i] != 0x47)
            continue; /* find the next sync byte */
        a->pkt[a->pkt_n++] = data[i];
        if (a->pkt_n < 188)
            continue;
        a->pkt_n = 0;
        int start = 0;
        int waiting = a->rec && !a->rec_synced;
        ts_packet(a, a->pkt, waiting ? &start : NULL);
        if (waiting && !start && now_ms() - a->rec_wait_ms > 6000)
            start = 1; /* no SPS seen: start anyway, players will cope */
        if (a->rec && a->rec_wide && (start || a->rec_synced)) {
            if (start) {
                if (a->have_pat)
                    sink_push(a->rec, a->pat, 188);
                if (a->have_pmt)
                    sink_push(a->rec, a->pmt, 188);
                a->rec_synced = 1;
            }
            wide_packet(a->pkt);
            sink_push(a->rec, a->pkt, 188);
            continue;
        }
        if (start) {
            if (a->have_pat)
                sink_push(a->rec, a->pat, 188);
            if (a->have_pmt)
                sink_push(a->rec, a->pmt, 188);
            sink_push(a->rec, a->pkt, 188);
            a->rec_synced = 1;
            raw_from = i + 1;
        }
    }
    if (a->rec && raw_from < len)
        sink_push(a->rec, data + raw_from, len - raw_from);
    if (a->vdec)
        dec_feed(a->vdec, data, len);
    if (a->adec)
        dec_feed(a->adec, data, len);
    pthread_mutex_unlock(&a->data_lock);
}

static void on_video(const uint8_t *data, size_t len, void *user)
{
    app_t *a = user;
    pthread_mutex_lock(&a->frame_lock);
    if (len == (size_t)a->fw * a->fh * 4) {
        if (a->q_count == FRAME_QUEUE) { /* full: drop the oldest */
            a->q_head = (a->q_head + 1) % FRAME_QUEUE;
            a->q_count--;
        }
        memcpy(a->q[(a->q_head + a->q_count) % FRAME_QUEUE], data, len);
        a->q_count++;
    }
    pthread_mutex_unlock(&a->frame_lock);
}

/* Puts the next queued frame on screen when its time has come. frame_lock held. */
static void advance_frame(app_t *a)
{
    int64_t now = now_us();
    if (!a->q_playing) {
        if (a->q_count < QUEUE_START) /* frames in hand absorb the decoder's unevenness */
            return;
        a->q_playing = 1;
        a->q_due_us = now;
    }
    /* the screen asks every ~16 ms: a frame due within half of that goes now, or
     * jitter would hold every other frame back a whole refresh */
    if (!a->q_count || now + 8000 < a->q_due_us)
        return;
    while (a->q_count > QUEUE_KEEP) { /* behind: skip to stay close to live */
        a->q_head = (a->q_head + 1) % FRAME_QUEUE;
        a->q_count--;
    }
    uint32_t *t = a->rgb;
    a->rgb = a->q[a->q_head];
    a->q[a->q_head] = t;
    a->q_head = (a->q_head + 1) % FRAME_QUEUE;
    a->q_count--;
    a->seq++;
    /* The box's clock and ours differ a little, so follow the queue: run a few
     * percent slow when it is nearly empty and fast when it builds up. A frame
     * that came late starts the clock again from now rather than leaving a debt
     * that every later frame would pay for. */
    int64_t base = now - a->q_due_us > a->period_us / 2 ? now : a->q_due_us;
    int64_t step = a->period_us;
    if (a->q_count == 0)
        step += step / 20;
    else if (a->q_count >= QUEUE_START)
        step -= step / 20;
    a->q_due_us = base + step;
}

static void on_audio(const uint8_t *data, size_t len, void *user)
{
    app_t *a = user;
    monitor_ring_write(&a->ring, (const int16_t *)(const void *)data, (int)(len / 4), a->peak);
}

static void clear_frame(app_t *a)
{
    pthread_mutex_lock(&a->frame_lock);
    if (a->rgb)
        memset(a->rgb, 0, (size_t)a->fw * a->fh * 4);
    a->q_head = a->q_count = a->q_playing = 0;
    a->seq++;
    pthread_mutex_unlock(&a->frame_lock);
}

static void stop_decoders(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    decoder_t *v = a->vdec, *au = a->adec;
    a->vdec = a->adec = NULL;
    pthread_mutex_unlock(&a->data_lock);
    dec_stop(v);
    dec_stop(au);
    clear_frame(a);
}

/* The ffmpeg for the picture (scaled, deinterlaced, BGRX); NULL if it would not start (err says why). */
static decoder_t *start_video(app_t *a, int sh, int fps, char *err, int errlen)
{
    int hd = sh > 576, w = hd ? 1280 : 720, h = hd ? 720 : sh > 480 ? 576 : 480;
#ifdef __ppc__
    if (hd) /* a G4/G5 is short of time to decode HD at all: a quarter of the pixels to scale and show */
        w = 640, h = 360;
#endif
    pthread_mutex_lock(&a->frame_lock);
    if (w != a->fw || h != a->fh) {
        uint32_t *n[FRAME_QUEUE + 1];
        int ok = 1;
        for (int i = 0; i <= FRAME_QUEUE; i++)
            ok &= (n[i] = calloc((size_t)w * h, 4)) != NULL;
        if (ok) {
            free(a->rgb);
            a->rgb = n[FRAME_QUEUE];
            for (int i = 0; i < FRAME_QUEUE; i++) {
                free(a->q[i]);
                a->q[i] = n[i];
            }
            a->fw = w;
            a->fh = h;
        } else {
            for (int i = 0; i <= FRAME_QUEUE; i++)
                free(n[i]);
        }
    }
    a->hd = hd;
    a->q_head = a->q_count = a->q_playing = 0;
    /* the preview's frame rate: interlaced and 25/30p as is, 50/60p halved (see rate below) */
    int out = fps >= 50 ? fps / 2 : fps > 0 ? fps : 30;
    a->period_us = out == 30 ? 1001000 / 30 : 1000000 / out;
    a->seq++;
    pthread_mutex_unlock(&a->frame_lock);

    char vf[256], rate[48] = "";
    if (fps >= 50) /* 720p60/50: half the frames are plenty for a preview */
        snprintf(rate, sizeof(rate), ",fps=%s", fps == 50 ? "25" : "30000/1001");
    snprintf(vf, sizeof(vf), "yadif=mode=send_frame:parity=auto:deint=interlaced,scale=%d:%d%s,format=" PIX_FMT,
             a->fw, a->fh, rate);
    const char *vargs[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-fflags", "nobuffer",
#ifdef __ppc__
                            /* PowerPC: decode faster at some cost in picture quality; it is only the preview */
                            "-skip_loop_filter", "all", "-flags2", "fast",
#endif
                            "-probesize", "400000", "-analyzeduration", "1000000", "-f", "mpegts", "-i", "pipe:0",
                            "-map", "0:v:0", "-an", "-sn", "-vf", vf, "-f", "rawvideo", "-pix_fmt", PIX_FMT,
                            "-flush_packets", "1", "pipe:1", NULL };
    return dec_start(vargs, (size_t)a->fw * a->fh * 4, on_video, a, err, errlen);
}

/* One ffmpeg for the picture (only with the preview on), one for the sound (48 kHz s16). */
static void start_decoders(app_t *a, int sw, int sh, int fps)
{
    a->dec_sw = sw, a->dec_sh = sh, a->dec_fps = fps;
    char err[300] = "";
    decoder_t *v = a->preview ? start_video(a, sh, fps, err, sizeof(err)) : NULL;
    const char *aargs[] = { "-hide_banner", "-loglevel", "error", "-nostdin", "-fflags", "nobuffer",
                            "-probesize", "400000", "-analyzeduration", "1000000", "-f", "mpegts", "-i", "pipe:0",
                            "-map", "0:a:0", "-vn", "-sn", "-ac", "2", "-ar", "48000", "-f", PCM_FMT,
                            "-flush_packets", "1", "pipe:1", NULL };
    decoder_t *au = (v || !a->preview) ? dec_start(aargs, 4 * 480, on_audio, a, err, sizeof(err)) : NULL;
    if ((a->preview && !v) || !au) {
        char msg[400];
        snprintf(msg, sizeof(msg), "No preview or sound: %s", err);
        set_problem(a, msg);
    } else {
        set_problem(a, NULL);
    }
    monitor_ring_restart(&a->ring);
    pthread_mutex_lock(&a->data_lock);
    a->vdec = v;
    a->adec = au;
    pthread_mutex_unlock(&a->data_lock);
}

/* the preview was switched on or off: start or end the picture's decoder alone */
static void sync_preview(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    int has_v = a->vdec != NULL;
    pthread_mutex_unlock(&a->data_lock);
    if (a->preview && !has_v) {
        char err[300] = "";
        decoder_t *v = start_video(a, a->dec_sh, a->dec_fps, err, sizeof(err));
        if (v) {
            pthread_mutex_lock(&a->data_lock);
            a->vdec = v;
            pthread_mutex_unlock(&a->data_lock);
            set_problem(a, NULL);
        } else {
            char msg[400];
            snprintf(msg, sizeof(msg), "No preview: %s", err);
            set_problem(a, msg);
        }
    } else if (!a->preview && has_v) {
        pthread_mutex_lock(&a->data_lock);
        decoder_t *v = a->vdec;
        a->vdec = NULL;
        pthread_mutex_unlock(&a->data_lock);
        dec_stop(v);
        clear_frame(a);
    }
}

/* ---- the device worker ---- */

static int device_gone(int r)
{
    return r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO || r == LIBUSB_ERROR_PIPE ||
           r == LIBUSB_ERROR_NOT_FOUND || r == HDPVR_ERR_UNLOCK;
}

static void close_device(app_t *a)
{
    hdpvr_close(a->dev);
    pthread_mutex_lock(&a->lock);
    a->dev = NULL;
    a->sig_w = a->sig_h = a->sig_fps = 0;
    pthread_mutex_unlock(&a->lock);
}

static void open_failed(app_t *a, int r)
{
    if (r == HDPVR_ERR_NO_DEVICE)
        set_state(a, APP_SEARCHING, "No HD PVR found. Plug it in and switch it on.");
    else if (r == HDPVR_ERR_NO_DRIVER)
        set_state(a, APP_NO_DRIVER, "The HD PVR needs its driver (run hdpvr-driver.exe, see README)");
#ifdef __linux__
    else if (r == LIBUSB_ERROR_ACCESS)
        set_state(a, APP_SEARCHING, "No permission to use the HD PVR (install the udev rule, see README)");
#endif
    else if (r == LIBUSB_ERROR_BUSY || r == LIBUSB_ERROR_ACCESS)
        set_state(a, APP_SEARCHING, "The HD PVR is in use by another program");
    else
        set_state(a, APP_SEARCHING, "Cannot open the HD PVR: %s", hdpvr_strerror(r));
}

/* HDPVR_REPLAY=file.ts stands in for the device (for testing without one):
 * the file is fed in a loop at about 8 Mbit/s as if it came from the box. */
static void replay(app_t *a, const char *path)
{
    FILE *f = app_fopen(path, "rb");
    if (!f) {
        set_state(a, APP_SEARCHING, "Cannot open %s", path);
        return;
    }
    /* fed like the USB reads: 16 KB at a time, at HDPVR_REPLAY_KBPS (default 8000) */
    static uint8_t buf[16 * 1024];
    const char *kb = getenv("HDPVR_REPLAY_KBPS");
    int kbps = kb && atoi(kb) > 0 ? atoi(kb) : 8000;
    int64_t t0 = now_us(), sent = 0;
    start_decoders(a, 720, 480, 60);
    pthread_mutex_lock(&a->lock);
    a->sig_w = 720, a->sig_h = 480, a->sig_fps = 60;
    pthread_mutex_unlock(&a->lock);
    set_state(a, APP_STREAMING, "Replay of %s", path);
    while (!a->quit) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (!n) {
            rewind(f);
            continue;
        }
        sent += (int64_t)n;
        int64_t due = t0 + sent * 8000 / kbps, wait = due - now_us();
        if (wait > 0) {
#ifdef _WIN32
            Sleep((DWORD)(wait / 1000));
#else
            usleep((useconds_t)wait);
#endif
        }
        on_data(buf, n, a);
    }
    fclose(f);
}

static void *worker(void *arg)
{
    app_t *a = arg;
    int64_t not_before = 0;
    const char *rp = getenv("HDPVR_REPLAY");
    if (rp && *rp) {
        replay(a, rp);
        stop_decoders(a);
        return NULL;
    }
    set_state(a, APP_SEARCHING, "Looking for the HD PVR...");
    while (!a->quit) {
        if (!a->dev) {
            hdpvr_t *dev = NULL;
            int r = hdpvr_open(&dev);
            if (r) {
                open_failed(a, r);
                nap(a, RETRY_MS);
                continue;
            }
            pthread_mutex_lock(&a->lock);
            a->dev = dev;
            pthread_mutex_unlock(&a->lock);
        }

        pthread_mutex_lock(&a->lock);
        source_t s = a->src;
        a->reconfig = 0;
        pthread_mutex_unlock(&a->lock);
        const char *in = app_video_names[s.video];

        set_state(a, APP_STARTING, "Starting the encoder (%s)...", in);
        while (!a->quit && !a->reconfig && now_ms() < not_before)
            nap(a, (int)(not_before - now_ms()));
        if (a->quit || a->reconfig)
            continue;

        hdpvr_options_t o;
        hdpvr_default_options(&o);
        o.video_input = (hdpvr_video_input_t)s.video;
        o.audio_input = (hdpvr_audio_input_t)s.audio;
        o.pal = s.pal;
        o.kbps = quality_kbps[s.quality];
        o.peak_kbps = o.kbps + 2000;
        int r = hdpvr_configure(a->dev, &o);
        if (r) {
            set_state(a, APP_STARTING, "Configuring the HD PVR failed: %s", hdpvr_strerror(r));
            close_device(a);
            nap(a, RETRY_MS);
            continue;
        }

        /* Start the preview's ffmpeg now, while the encoder settles (hdpvr_start waits ~3 s):
         * launching it takes a moment, and the stream would otherwise pile up behind it. */
        hdpvr_signal_t pre = { 0, 0, 0 };
        if (hdpvr_get_signal(a->dev, &pre) == 0 && pre.width)
            start_decoders(a, pre.width, pre.height, pre.fps);

        /* wait for a picture; the source may be switched on later */
        for (;;) {
            r = hdpvr_start(a->dev, 2500, on_data, a);
            if (r != HDPVR_ERR_NO_SIGNAL || a->quit || a->reconfig)
                break;
            set_state(a, APP_NO_SIGNAL, "No picture on the %s input", in);
        }
        if (r) {
            stop_decoders(a);
            if (r != HDPVR_ERR_NO_SIGNAL) {
                set_state(a, APP_STARTING, "The HD PVR did not start: %s", hdpvr_strerror(r));
                close_device(a);
                nap(a, RETRY_MS);
            }
            continue;
        }

        hdpvr_signal_t sig = { 0, 0, 0 };
        hdpvr_get_signal(a->dev, &sig);
        pthread_mutex_lock(&a->lock);
        a->sig_w = sig.width, a->sig_h = sig.height, a->sig_fps = sig.fps;
        pthread_mutex_unlock(&a->lock);
        if (!pre.width || pre.width != sig.width || pre.height != sig.height || pre.fps != sig.fps) {
            stop_decoders(a);
            start_decoders(a, sig.width, sig.height, sig.fps);
        }
        set_state(a, APP_STREAMING, "%s %dx%d at %d Hz", in, sig.width, sig.height, sig.fps);

        int lost = 0;
        while (!a->quit && !a->reconfig) {
            nap(a, 1000);
            if (a->quit || a->reconfig)
                break;
            sync_preview(a);
            hdpvr_signal_t now;
            if ((r = hdpvr_get_signal(a->dev, &now))) {
                if (device_gone(r)) {
                    lost = 1;
                    break;
                }
                continue;
            }
            if (!now.width) {
                set_state(a, APP_NO_SIGNAL, "No picture on the %s input", in);
                continue;
            }
            if (now.width != sig.width || now.height != sig.height || now.fps != sig.fps) {
                /* the source changed format: the encoder follows by itself, the preview is resized */
                sig = now;
                pthread_mutex_lock(&a->lock);
                a->sig_w = sig.width, a->sig_h = sig.height, a->sig_fps = sig.fps;
                pthread_mutex_unlock(&a->lock);
                stop_decoders(a);
                start_decoders(a, sig.width, sig.height, sig.fps);
            }
            set_state(a, APP_STREAMING, "%s %dx%d at %d Hz", in, sig.width, sig.height, sig.fps);
        }
        stop_decoders(a);
        hdpvr_stop(a->dev);
        not_before = now_ms() + RESTART_GAP_MS;
        pthread_mutex_lock(&a->lock);
        a->sig_w = a->sig_h = a->sig_fps = 0;
        pthread_mutex_unlock(&a->lock);
        if (lost) {
            set_state(a, APP_SEARCHING, "The HD PVR was disconnected");
            close_device(a);
            not_before = 0;
        }
    }
    stop_decoders(a);
    if (a->dev)
        close_device(a);
    return NULL;
}

/* ---- public: lifecycle and state ---- */

#if !defined(__APPLE__) || defined(LEGACY_MAC)
static void output_cb(int16_t *out, int frames, void *user) { app_audio_render(user, out, frames); }
#endif

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

app_t *app_start(int video_input, int audio_input, int quality, int pal)
{
    app_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    pthread_mutex_init(&a->lock, NULL);
    pthread_cond_init(&a->cv, NULL);
    pthread_mutex_init(&a->data_lock, NULL);
    pthread_mutex_init(&a->frame_lock, NULL);
    a->src.video = clampi(video_input, 0, 2);
    a->src.audio = clampi(audio_input, 0, 2);
    a->src.quality = clampi(quality, 0, APP_QUALITIES - 1);
    a->src.pal = pal != 0;
    a->monitor = 1;
    a->preview = 1;
    a->ring.frames = a->ring_frames;
    a->ring.size = RING_FRAMES;
    a->fw = 1280;
    a->fh = 720;
    a->hd = 1;
    a->rgb = calloc((size_t)a->fw * a->fh, 4);
    for (int i = 0; i < FRAME_QUEUE; i++)
        a->q[i] = calloc((size_t)a->fw * a->fh, 4);
    a->period_us = 1001000 / 30;
#if !defined(__APPLE__) || defined(LEGACY_MAC)
    a->out = sa_output_start(AUDIO_RATE, output_cb, a);
    if (!a->out)
        snprintf(a->problem, sizeof(a->problem), "No audio output device for monitoring");
#endif
    if (pthread_create(&a->worker, NULL, worker, a)) {
        free(a);
        return NULL;
    }
    return a;
}

void app_close(app_t *a)
{
    if (!a)
        return;
    pthread_mutex_lock(&a->data_lock);
    sink_t *rec = a->rec;
    FILE *f = a->rec_file;
    a->rec = NULL;
    a->rec_file = NULL;
    pthread_mutex_unlock(&a->data_lock);
    if (rec) {
        sink_stop(rec, 1);
        fclose(f);
    }
    pthread_mutex_lock(&a->lock);
    a->quit = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->lock);
    pthread_join(a->worker, NULL);
#if !defined(__APPLE__) || defined(LEGACY_MAC)
    sa_output_stop(a->out);
#endif
    free(a->rgb);
    for (int i = 0; i < FRAME_QUEUE; i++)
        free(a->q[i]);
    free(a);
}

void app_retry(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->retry = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->lock);
}

void app_set_source(app_t *a, int video_input, int audio_input, int quality, int pal)
{
    if (app_is_recording(a))
        return;
    pthread_mutex_lock(&a->lock);
    source_t n = { clampi(video_input, 0, 2), clampi(audio_input, 0, 2), clampi(quality, 0, APP_QUALITIES - 1),
                   pal != 0 };
    if (memcmp(&n, &a->src, sizeof(n))) {
        a->src = n;
        a->reconfig = 1;
        pthread_cond_broadcast(&a->cv);
    }
    pthread_mutex_unlock(&a->lock);
}

app_state_t app_state(app_t *a)
{
    pthread_mutex_lock(&a->lock);
    app_state_t s = a->state;
    pthread_mutex_unlock(&a->lock);
    return s;
}

void app_status(app_t *a, char *out, int len)
{
    pthread_mutex_lock(&a->lock);
    if (a->problem[0])
        snprintf(out, (size_t)len, "%s \xe2\x80\x94 %s", a->status, a->problem);
    else
        snprintf(out, (size_t)len, "%s", a->status);
    pthread_mutex_unlock(&a->lock);
}

const char *app_audio_problem(app_t *a) { return a->problem[0] ? a->problem : NULL; }

int app_signal(app_t *a, int *w, int *h, int *fps)
{
    pthread_mutex_lock(&a->lock);
    *w = a->sig_w, *h = a->sig_h, *fps = a->sig_fps;
    pthread_mutex_unlock(&a->lock);
    return *w != 0;
}

/* ---- picture ---- */

void app_frame_size(app_t *a, int *w, int *h)
{
    pthread_mutex_lock(&a->frame_lock);
    *w = a->fw, *h = a->fh;
    pthread_mutex_unlock(&a->frame_lock);
}

int app_get_frame(app_t *a, uint32_t *buf, int w, int h, uint64_t *seq)
{
    int copied = 0;
    pthread_mutex_lock(&a->frame_lock);
    advance_frame(a);
    if (a->seq != *seq && w == a->fw && h == a->fh) {
        memcpy(buf, a->rgb, (size_t)w * h * 4);
        *seq = a->seq;
        copied = 1;
    }
    pthread_mutex_unlock(&a->frame_lock);
    return copied;
}

double app_aspect(app_t *a)
{
    pthread_mutex_lock(&a->frame_lock);
    int hd = a->hd;
    pthread_mutex_unlock(&a->frame_lock);
    return hd || a->widescreen ? 16.0 / 9 : 4.0 / 3;
}

void app_set_widescreen(app_t *a, int on) { a->widescreen = on; }

void app_set_preview(app_t *a, int on)
{
    pthread_mutex_lock(&a->lock);
    a->preview = on != 0;
    a->retry = 1;
    pthread_cond_broadcast(&a->cv); /* wakes the worker's nap so the decoder follows at once */
    pthread_mutex_unlock(&a->lock);
}

int app_preview(app_t *a) { return a->preview; }

/* ---- sound ---- */

void app_peaks(app_t *a, double db[2])
{
    for (int c = 0; c < 2; c++) {
        int p = __atomic_exchange_n(&a->peak[c], 0, __ATOMIC_RELAXED);
        db[c] = p > 0 ? 20 * log10(p / 32768.0) : -60;
        if (db[c] < -60)
            db[c] = -60;
    }
}

void app_set_monitor(app_t *a, int on) { a->monitor = on; }

/* 150 ms behind the decoder: ffmpeg hands the sound over a few AAC frames at a time as the
 * transport stream interleaves it, and the cushion has to outlast the gaps (see monitor_ring.h) */
static int render(app_t *a, int16_t *out, float *L, float *R, int frames)
{
    return monitor_ring_read(&a->ring, AUDIO_RATE * 150 / 1000, out, L, R, frames, a->monitor ? 1.0f : 0.0f);
}

int app_audio_render(app_t *a, int16_t *out, int frames) { return render(a, out, NULL, NULL, frames); }
int app_audio_render_float(app_t *a, float *left, float *right, int frames)
{
    return render(a, NULL, left, right, frames);
}

/* ---- files ---- */

FILE *app_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8))
        return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

static int make_dir(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024))
        return -1;
    return CreateDirectoryW(wp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 || access(path, F_OK) == 0 ? 0 : -1;
#endif
}

void app_timestamped_path(const char *folder, const char *ext, char *out, int len)
{
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d at %H.%M.%S", &tm);
    size_t fl = strlen(folder);
    int sep = fl && folder[fl - 1] != '/' && folder[fl - 1] != '\\';
    snprintf(out, (size_t)len, "%s%s" APP_NAME " %s.%s", folder, sep ? (DIR_SEP == '/' ? "/" : "\\") : "", stamp,
             ext);
}

void app_default_folder(char *out, int len)
{
#ifdef _WIN32
    static const GUID videos = { 0x18989b1d, 0x99b5, 0x455b, { 0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc } };
    PWSTR w = NULL;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&videos, 0, NULL, &w)))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, len, NULL, NULL);
    CoTaskMemFree(w);
    if (!out[0]) {
        const char *p = getenv("USERPROFILE");
        snprintf(out, (size_t)len, "%s", p ? p : ".");
    }
#else
    const char *home = getenv("HOME");
#ifdef __APPLE__
    snprintf(out, (size_t)len, "%s/Movies", home ? home : ".");
#else
    char cfg[1024], line[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(cfg, sizeof(cfg), "%s/user-dirs.dirs", xdg);
    else
        snprintf(cfg, sizeof(cfg), "%s/.config/user-dirs.dirs", home ? home : "");
    snprintf(out, (size_t)len, "%s/Videos", home ? home : ".");
    FILE *f = fopen(cfg, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *v = strstr(line, "XDG_VIDEOS_DIR=\"");
            if (v != line)
                continue;
            v += 16;
            char *end = strchr(v, '"');
            if (end)
                *end = 0;
            if (!strncmp(v, "$HOME", 5))
                snprintf(out, (size_t)len, "%s%s", home ? home : "", v + 5);
            else
                snprintf(out, (size_t)len, "%s", v);
        }
        fclose(f);
    }
#endif
    if (access(out, F_OK) != 0)
        snprintf(out, (size_t)len, "%s", home ? home : ".");
#endif
}

/* ---- settings ---- */

static void settings_path(char *out, size_t len)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s\\" APP_NAME, base ? base : ".");
    make_dir(dir);
    snprintf(out, len, "%s\\settings.ini", dir);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char base[1024], dir[1100];
    if (xdg && *xdg)
        snprintf(base, sizeof(base), "%s", xdg);
    else
        snprintf(base, sizeof(base), "%s/.config", home ? home : ".");
    make_dir(base);
    snprintf(dir, sizeof(dir), "%s/hdpvr-capture", base);
    make_dir(dir);
    snprintf(out, len, "%s/settings.ini", dir);
#endif
}

void app_setting_str(const char *key, char *out, int len, const char *def)
{
    char path[1200], line[1200];
    size_t kl = strlen(key);
    snprintf(out, (size_t)len, "%s", def ? def : "");
    settings_path(path, sizeof(path));
    FILE *f = app_fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) || line[kl] != '=')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, (size_t)len, "%s", line + kl + 1);
    }
    fclose(f);
}

int app_setting_int(const char *key, int def)
{
    char v[32];
    app_setting_str(key, v, sizeof(v), "");
    return v[0] ? atoi(v) : def;
}

void app_set_setting_str(const char *key, const char *v)
{
    char path[1200], line[1200];
    size_t kl = strlen(key), used = 0, cap = 4096;
    char *all = malloc(cap);
    if (!all)
        return;
    all[0] = 0;
    settings_path(path, sizeof(path));
    FILE *f = app_fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, key, kl) && line[kl] == '=')
                continue;
            size_t n = strlen(line);
            if (used + n + 1 > cap) {
                char *b = realloc(all, cap *= 2);
                if (!b)
                    break;
                all = b;
            }
            memcpy(all + used, line, n + 1);
            used += n;
        }
        fclose(f);
    }
    if ((f = app_fopen(path, "w"))) {
        fputs(all, f);
        fprintf(f, "%s=%s\n", key, v);
        fclose(f);
    }
    free(all);
}

void app_set_setting_int(const char *key, int v)
{
    char s[32];
    snprintf(s, sizeof(s), "%d", v);
    app_set_setting_str(key, s);
}

/* ---- recording ---- */

static int write_file(void *ctx, const uint8_t *p, size_t n) { return fwrite(p, 1, n, ctx) == n ? 0 : -1; }

int app_record_start(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen)
{
    if (app_state(a) != APP_STREAMING) {
        snprintf(err, (size_t)errlen, "There is no picture to record yet.");
        return -1;
    }
    make_dir(folder);
    app_timestamped_path(folder, "ts", path, pathlen);
    FILE *f = app_fopen(path, "wb");
    if (!f) {
        snprintf(err, (size_t)errlen, "Cannot create %s", path);
        return -1;
    }
    sink_t *s = sink_start(REC_QUEUE, write_file, f);
    if (!s) {
        fclose(f);
        snprintf(err, (size_t)errlen, "out of memory");
        return -1;
    }
    pthread_mutex_lock(&a->lock);
    int wide = a->widescreen && a->sig_h > 0 && a->sig_h <= 576; /* SD only: HD is 16:9 already */
    pthread_mutex_unlock(&a->lock);
    pthread_mutex_lock(&a->data_lock);
    a->rec = s;
    a->rec_file = f;
    a->rec_synced = 0;
    a->rec_wide = wide;
    a->rec_wait_ms = now_ms();
    mono_ts(&a->rec_t0);
    snprintf(a->rec_path, sizeof(a->rec_path), "%s", path);
    pthread_mutex_unlock(&a->data_lock);
    return 0;
}

typedef struct {
    sink_t *rec;
    FILE *f;
    app_done_cb done;
    void *user;
    char path[1024];
} stop_job_t;

static void *stop_thread(void *arg)
{
    stop_job_t *j = arg;
    uint64_t dropped = sink_dropped(j->rec);
    int ok = sink_stop(j->rec, 1) == 0;
    ok &= fclose(j->f) == 0;
    if (dropped)
        fprintf(stderr, APP_NAME ": %llu bytes were dropped because the disk was too slow\n",
                (unsigned long long)dropped);
    if (j->done)
        j->done(j->user, ok, j->path);
    free(j);
    return NULL;
}

void app_record_stop(app_t *a, app_done_cb done, void *user)
{
    pthread_mutex_lock(&a->data_lock);
    sink_t *rec = a->rec;
    FILE *f = a->rec_file;
    a->rec = NULL;
    a->rec_file = NULL;
    pthread_mutex_unlock(&a->data_lock);
    if (!rec)
        return;
    stop_job_t *j = calloc(1, sizeof(*j));
    j->rec = rec;
    j->f = f;
    j->done = done;
    j->user = user;
    snprintf(j->path, sizeof(j->path), "%s", a->rec_path);
    pthread_t t;
    if (pthread_create(&t, NULL, stop_thread, j) == 0)
        pthread_detach(t);
    else
        stop_thread(j);
}

int app_is_recording(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    int r = a->rec != NULL;
    pthread_mutex_unlock(&a->data_lock);
    return r;
}

double app_record_seconds(app_t *a)
{
    struct timespec now;
    mono_ts(&now);
    pthread_mutex_lock(&a->data_lock);
    double s = a->rec ? (now.tv_sec - a->rec_t0.tv_sec) + (now.tv_nsec - a->rec_t0.tv_nsec) / 1e9 : 0;
    pthread_mutex_unlock(&a->data_lock);
    return s;
}

uint64_t app_record_bytes(app_t *a)
{
    pthread_mutex_lock(&a->data_lock);
    uint64_t b = a->rec ? sink_written(a->rec) : 0;
    pthread_mutex_unlock(&a->data_lock);
    return b;
}

/* ---- snapshot: minimal PNG writer (stored deflate blocks) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uint32_t c = crc32_update(crc32_update(0, (const uint8_t *)type, 4), data, len);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = app_fopen(path, "wb");
    if (!f)
        return -1;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0; /* 8-bit RGB */
    png_chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (size_t)h * (1 + (size_t)w * 3), blocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(zlen), *raw = malloc(raw_len);
    if (!z || !raw) {
        free(z), free(raw), fclose(f);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (1 + (size_t)w * 3)] = 0; /* filter: none */
        memcpy(raw + (size_t)y * (1 + (size_t)w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    uint8_t *p = z;
    *p++ = 0x78, *p++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw_len;) {
        uint32_t n = (uint32_t)(raw_len - off > 65535 ? 65535 : raw_len - off);
        *p++ = off + n == raw_len;
        *p++ = (uint8_t)n, *p++ = (uint8_t)(n >> 8), *p++ = (uint8_t)~n, *p++ = (uint8_t)(~n >> 8);
        memcpy(p, raw + off, n);
        for (uint32_t i = 0; i < n; i++) {
            s1 = (s1 + raw[off + i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        p += n;
        off += n;
    }
    be32(p, s2 << 16 | s1);
    p += 4;
    png_chunk(f, "IDAT", z, (uint32_t)(p - z));
    png_chunk(f, "IEND", NULL, 0);
    free(z);
    free(raw);
    return fclose(f) ? -1 : 0;
}

int app_snapshot(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen)
{
    if (app_state(a) != APP_STREAMING) {
        snprintf(err, (size_t)errlen, "There is no picture to save yet.");
        return -1;
    }
    if (!a->preview) {
        snprintf(err, (size_t)errlen, "Turn the preview on to save a snapshot.");
        return -1;
    }
    int w, h;
    app_frame_size(a, &w, &h);
    /* resample to square pixels: 720 BT.601 samples span the 4:3 (or 16:9) picture */
    const int oh = h, ow = (int)(h * app_aspect(a) / 2 + 0.5) * 2;
    uint32_t *src = malloc((size_t)w * h * 4);
    uint8_t *dst = malloc((size_t)ow * oh * 3);
    uint64_t seq = (uint64_t)-1;
    if (!src || !dst || !app_get_frame(a, src, w, h, &seq)) {
        free(src), free(dst);
        snprintf(err, (size_t)errlen, "No picture to save");
        return -1;
    }
    for (int y = 0; y < oh; y++) {
        const uint32_t *row = src + (size_t)y * w;
        for (int x = 0; x < ow; x++) {
            double fx = (x + 0.5) * w / ow - 0.5;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < w ? x0 + 1 : x0;
            double t = fx - x0;
            if (t < 0)
                t = 0;
            for (int c = 0; c < 3; c++) {
                int sh = 16 - 8 * c;
                double v = ((row[x0] >> sh) & 0xff) * (1 - t) + ((row[x1] >> sh) & 0xff) * t;
                dst[((size_t)y * ow + x) * 3 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
    make_dir(folder);
    app_timestamped_path(folder, "png", path, pathlen);
    int r = write_png(path, dst, ow, oh);
    if (r)
        snprintf(err, (size_t)errlen, "Cannot write %s", path);
    free(src);
    free(dst);
    return r;
}
