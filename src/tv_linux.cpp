/*
 * tv-capture-linux: the V4L2 backend for the TV tuner helper, for the Hauppauge WinTV
 * HVR-950Q and other tuners the kernel drives with a V4L2 driver (the HVR-950Q uses
 * au0828, with the xc5000 tuner, which needs its firmware: xc5000-1.6.114.fw from
 * linux-firmware). Everything else (captions, recording, the --serve protocol for
 * HauppaugeRecorder, the command line) is in tvcore.hpp; see there and docs/TVHELPER.md.
 *
 *   video   /dev/videoN  V4L2 memory-mapped capture, UYVY or YUYV 720x480 (720x576 PAL)
 *   VBI     /dev/vbiN    raw VBI, one read() per frame holding both fields (line 21 of
 *                        field 1 and line 284 of field 2 are in the last line of each)
 *   audio   ALSA         the tuner's USB audio device, captured through libasound loaded
 *                        at run time (no ALSA headers or library needed to build)
 *   tuner   VIDIOC_S_FREQUENCY, with the North American channel plan (tvchan.hpp)
 *
 * Build: cc -O2 -std=c++17 -Isrc src/tv_linux.cpp -lpthread -ldl -o tv-capture-linux
 *        (scripts/build-all.sh does it with Zig).
 *
 * Written from the V4L2 documentation and the au0828 driver; not yet run on a real
 * tuner. Run with --probe first.
 */
#include "tvplat.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "tvchan.hpp"
#include "tvcore.hpp"

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

static std::string cstr(const unsigned char *s, size_t n)
{
    size_t l = 0;
    while (l < n && s[l]) l++;
    return std::string((const char *)s, l);
}

/* ---- ALSA, loaded at run time ----------------------------------------------------------------- */
namespace alsa {
typedef struct snd_pcm snd_pcm_t;
typedef struct snd_mixer snd_mixer_t;
typedef struct snd_mixer_elem snd_mixer_elem_t;
enum { STREAM_CAPTURE = 1, FORMAT_S16_LE = 2, ACCESS_RW_INTERLEAVED = 3 };

static struct {
    bool loaded = false;
    int (*pcm_open)(snd_pcm_t **, const char *, int, int);
    int (*pcm_set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
    long (*pcm_readi)(snd_pcm_t *, void *, unsigned long);
    int (*pcm_recover)(snd_pcm_t *, int, int);
    int (*pcm_close)(snd_pcm_t *);
    int (*mixer_open)(snd_mixer_t **, int);
    int (*mixer_attach)(snd_mixer_t *, const char *);
    int (*mixer_register)(snd_mixer_t *, void *, void *);
    int (*mixer_load)(snd_mixer_t *);
    snd_mixer_elem_t *(*mixer_first)(snd_mixer_t *);
    snd_mixer_elem_t *(*mixer_next)(snd_mixer_elem_t *);
    const char *(*selem_name)(snd_mixer_elem_t *);
    int (*has_cswitch)(snd_mixer_elem_t *);
    int (*set_cswitch_all)(snd_mixer_elem_t *, int);
    int (*has_cvolume)(snd_mixer_elem_t *);
    int (*cvolume_range)(snd_mixer_elem_t *, long *, long *);
    int (*set_cvolume_all)(snd_mixer_elem_t *, long);
    int (*mixer_close)(snd_mixer_t *);
} A;

static bool load()
{
    if (A.loaded) return true;
    void *h = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
    auto sym = [&](const char *n) { return dlsym(h, n); };
    *(void **)&A.pcm_open = sym("snd_pcm_open");
    *(void **)&A.pcm_set_params = sym("snd_pcm_set_params");
    *(void **)&A.pcm_readi = sym("snd_pcm_readi");
    *(void **)&A.pcm_recover = sym("snd_pcm_recover");
    *(void **)&A.pcm_close = sym("snd_pcm_close");
    *(void **)&A.mixer_open = sym("snd_mixer_open");
    *(void **)&A.mixer_attach = sym("snd_mixer_attach");
    *(void **)&A.mixer_register = sym("snd_mixer_selem_register");
    *(void **)&A.mixer_load = sym("snd_mixer_load");
    *(void **)&A.mixer_first = sym("snd_mixer_first_elem");
    *(void **)&A.mixer_next = sym("snd_mixer_elem_next");
    *(void **)&A.selem_name = sym("snd_mixer_selem_get_name");
    *(void **)&A.has_cswitch = sym("snd_mixer_selem_has_capture_switch");
    *(void **)&A.set_cswitch_all = sym("snd_mixer_selem_set_capture_switch_all");
    *(void **)&A.has_cvolume = sym("snd_mixer_selem_has_capture_volume");
    *(void **)&A.cvolume_range = sym("snd_mixer_selem_get_capture_volume_range");
    *(void **)&A.set_cvolume_all = sym("snd_mixer_selem_set_capture_volume_all");
    *(void **)&A.mixer_close = sym("snd_mixer_close");
    A.loaded = A.pcm_open && A.pcm_set_params && A.pcm_readi && A.pcm_recover && A.pcm_close;
    return A.loaded;
}

/* The card number whose /proc/asound/cards entry mentions `text` (case-insensitive), or -1. */
static int find_card(const std::string &text)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return -1;
    char line[512];
    int found = -1, cur = -1;
    while (fgets(line, sizeof line, f)) {
        int n;
        if (sscanf(line, " %d [", &n) == 1) cur = n; /* "N [id]: driver - name", then "    long name" */
        if (cur >= 0 && icontains(line, text)) { found = cur; break; }
    }
    fclose(f);
    return found;
}

/* Capture switches and volumes of a USB audio device start muted or low (the same thing as the disabled input
 * mixer of the Windows driver, which records silence). Turn every capture control on and up. */
static void unmute(int card)
{
    if (!A.mixer_open || !A.mixer_attach || !A.mixer_register || !A.mixer_load || !A.mixer_first || !A.mixer_next) return;
    snd_mixer_t *m = nullptr;
    char name[32];
    snprintf(name, sizeof name, "hw:%d", card);
    if (A.mixer_open(&m, 0) < 0) return;
    if (A.mixer_attach(m, name) < 0 || A.mixer_register(m, nullptr, nullptr) < 0 || A.mixer_load(m) < 0) { if (A.mixer_close) A.mixer_close(m); return; }
    for (snd_mixer_elem_t *e = A.mixer_first(m); e; e = A.mixer_next(e)) {
        const char *n = A.selem_name ? A.selem_name(e) : "?";
        if (A.has_cswitch && A.has_cswitch(e) && A.set_cswitch_all) { A.set_cswitch_all(e, 1); logf("alsa mixer: capture switch on for '%s'", n); }
        if (A.has_cvolume && A.has_cvolume(e) && A.cvolume_range && A.set_cvolume_all) {
            long lo = 0, hi = 0;
            A.cvolume_range(e, &lo, &hi);
            A.set_cvolume_all(e, hi);
            logf("alsa mixer: capture volume of '%s' set to maximum (%ld)", n, hi);
        }
    }
    if (A.mixer_close) A.mixer_close(m);
}
} // namespace alsa

/* ---- the backend --------------------------------------------------------------------------------- */
class V4lBackend : public Backend {
public:
    int vfd = -1, bfd = -1;
    std::string vdev, bdev;
    struct Map { void *p = nullptr; size_t len = 0; };
    std::vector<Map> maps;
    bool streaming = false, tuner_ok = false;
    std::atomic<bool> running{false};
    std::thread tv, tb, ta;
    alsa::snd_pcm_t *pcm = nullptr;
    Engine *E = nullptr;
    int bytesperline = 0;
    /* raw VBI */
    unsigned vbi_samples = 0, vbi_count[2] = { 0, 0 }, vbi_size = 0;
    int audio_card = -1;

    bool open(Engine &e) override;
    bool run(Engine &e) override;
    void stop(Engine &e) override;
    void close(Engine &e) override;
    bool route(Engine &e) override;
    bool tune(Engine &e) override;
    void list_devices() override;
    bool has_tuner(Engine &) override { return tuner_ok; }

private:
    bool find_devices(const Options &o, std::string &err);
    void video_loop();
    void vbi_loop();
    void audio_loop();
};

bool V4lBackend::find_devices(const Options &o, std::string &err)
{
    std::string busid;
    vdev.clear();
    bdev.clear();
    if (o.device.rfind("/dev/", 0) == 0) vdev = o.device;
    for (int pass = 0; pass < 2 && vdev.empty(); pass++)
        for (int i = 0; i < 64 && vdev.empty(); i++) {
            std::string path = "/dev/video" + std::to_string(i);
            int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0) continue;
            struct v4l2_capability cap;
            memset(&cap, 0, sizeof cap);
            bool ok = xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
            uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            if (ok && (caps & V4L2_CAP_VIDEO_CAPTURE)) {
                std::string card = cstr(cap.card, sizeof cap.card), drv = cstr(cap.driver, sizeof cap.driver);
                bool match = icontains(card, o.device) || icontains(drv, o.device);
                if (o.device == "WinTV") match = match || drv == "au0828" || icontains(card, "HVR"); /* the default */
                if (match) { vdev = path; busid = cstr(cap.bus_info, sizeof cap.bus_info); logf("video device: %s (%s, driver %s)", path.c_str(), card.c_str(), drv.c_str()); }
            }
            ::close(fd);
        }
    if (vdev.empty()) { err = fmt_str("No capture device matching '%s'. Plug in the tuner (the kernel's au0828 driver and the xc5000 firmware are needed).", o.device.c_str()); return false; }
    if (busid.empty()) {
        int fd = ::open(vdev.c_str(), O_RDWR | O_NONBLOCK);
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof cap);
        if (fd >= 0 && xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) busid = cstr(cap.bus_info, sizeof cap.bus_info);
        if (fd >= 0) ::close(fd);
    }
    /* the VBI device of the same tuner shares its bus_info */
    for (int i = 0; i < 64 && bdev.empty() && !busid.empty(); i++) {
        std::string path = "/dev/vbi" + std::to_string(i);
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof cap);
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && cstr(cap.bus_info, sizeof cap.bus_info) == busid) bdev = path;
        ::close(fd);
    }
    return true;
}

bool V4lBackend::open(Engine &e)
{
    E = &e;
    const Options &o = e.o;
    e.have_vbi = e.have_audio = false;
    std::string err;
    if (!find_devices(o, err)) { e.error = err; return false; }
    vfd = ::open(vdev.c_str(), O_RDWR | O_NONBLOCK);
    if (vfd < 0) { e.error = fmt_str("Cannot open %s: %s. Is another program using the tuner, and are you in the video group?", vdev.c_str(), strerror(errno)); return false; }

    /* input and standard */
    if (!route(e)) return false;

    /* picture: UYVY (what au0828 gives) or YUYV, interlaced */
    struct v4l2_format f;
    bool set = false;
    for (uint32_t pf : { (uint32_t)V4L2_PIX_FMT_UYVY, (uint32_t)V4L2_PIX_FMT_YUYV }) {
        memset(&f, 0, sizeof f);
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.width = 720;
        f.fmt.pix.height = o.pal ? 576 : 480;
        f.fmt.pix.pixelformat = pf;
        f.fmt.pix.field = V4L2_FIELD_INTERLACED;
        if (xioctl(vfd, VIDIOC_S_FMT, &f) == 0 && f.fmt.pix.pixelformat == pf) { set = true; break; }
    }
    if (!set) { e.error = "The tuner offers no UYVY or YUYV picture format."; return false; }
    e.vw = (int)f.fmt.pix.width;
    e.vh = (int)f.fmt.pix.height;
    bytesperline = (int)f.fmt.pix.bytesperline ? (int)f.fmt.pix.bytesperline : e.vw * 2;
    e.pixfmt = f.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY ? "uyvy422" : "yuyv422";
    e.vbytes = (size_t)e.vw * e.vh * 2;
    if (o.pal) { e.fps_num = 25; e.fps_den = 1; e.frame_dur = 400000; } else { e.fps_num = 30000; e.fps_den = 1001; e.frame_dur = 333667; }
    logf("video: %dx%d %s, %d bytes/line", e.vw, e.vh, e.pixfmt.c_str(), bytesperline);

    /* memory-mapped buffers */
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = 6;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) { e.error = "The tuner would not give capture buffers."; return false; }
    maps.assign(rb.count, Map());
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) { e.error = "VIDIOC_QUERYBUF failed"; return false; }
        maps[i].len = b.length;
        maps[i].p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
        if (maps[i].p == MAP_FAILED) { maps[i].p = nullptr; e.error = "mmap of a capture buffer failed"; return false; }
    }

    /* tuner present? */
    struct v4l2_tuner t;
    memset(&t, 0, sizeof t);
    t.index = 0;
    tuner_ok = xioctl(vfd, VIDIOC_G_TUNER, &t) == 0;
    if (!tune(e)) return false;

    /* raw VBI */
    if (!o.no_cc || o.probe > 0) {
        if (bdev.empty()) logf("warning: no VBI device found next to %s; no captions", vdev.c_str());
        else {
            bfd = ::open(bdev.c_str(), O_RDWR | O_NONBLOCK);
            struct v4l2_format vf;
            memset(&vf, 0, sizeof vf);
            vf.type = V4L2_BUF_TYPE_VBI_CAPTURE;
            if (bfd >= 0 && xioctl(bfd, VIDIOC_G_FMT, &vf) == 0 && vf.fmt.vbi.samples_per_line && vf.fmt.vbi.sample_format == V4L2_PIX_FMT_GREY) {
                vbi_samples = vf.fmt.vbi.samples_per_line;
                vbi_count[0] = vf.fmt.vbi.count[0];
                vbi_count[1] = vf.fmt.vbi.count[1];
                vbi_size = (vbi_count[0] + vbi_count[1]) * vbi_samples;
                e.vbi_fs = vf.fmt.vbi.sampling_rate;
                e.have_vbi = vbi_size > 0;
                logf("vbi: %s, lines starting at %u and %u, %u+%u lines of %u samples at %.3f MHz", bdev.c_str(), vf.fmt.vbi.start[0],
                     vf.fmt.vbi.start[1], vbi_count[0], vbi_count[1], vbi_samples, e.vbi_fs / 1e6);
            } else logf("warning: cannot read the VBI format of %s; no captions", bdev.c_str());
        }
    }

    /* audio: the tuner's USB audio device through ALSA */
    if (alsa::load()) {
        audio_card = alsa::find_card(o.audio_device);
        if (audio_card < 0 && o.audio_device == "950") audio_card = alsa::find_card("WinTV");
        if (audio_card >= 0) {
            char dev[32];
            snprintf(dev, sizeof dev, "plughw:%d,0", audio_card);
            if (alsa::A.pcm_open(&pcm, dev, alsa::STREAM_CAPTURE, 0) >= 0 &&
                alsa::A.pcm_set_params(pcm, alsa::FORMAT_S16_LE, alsa::ACCESS_RW_INTERLEAVED, 2, 48000, 1, 100000) >= 0) {
                alsa::unmute(audio_card);
                e.audio = AudioFmt();
                e.have_audio = true;
                logf("audio: %s, 48000 Hz, 2 ch, 16 bit", dev);
            } else {
                logf("warning: cannot open ALSA capture on card %d; recording without sound", audio_card);
                if (pcm) { alsa::A.pcm_close(pcm); pcm = nullptr; }
            }
        } else logf("warning: no ALSA card matching '%s'; recording without sound (see /proc/asound/cards)", o.audio_device.c_str());
    } else logf("warning: libasound.so.2 not found; recording without sound");
    return true;
}

bool V4lBackend::route(Engine &e)
{
    if (vfd < 0) return true;
    const Options &o = e.o;
    /* the input: the tuner is the one of type TUNER, the others are found by name */
    int want = -1;
    for (unsigned i = 0;; i++) {
        struct v4l2_input in;
        memset(&in, 0, sizeof in);
        in.index = i;
        if (xioctl(vfd, VIDIOC_ENUMINPUT, &in) < 0) break;
        std::string n = cstr(in.name, sizeof in.name);
        if (o.source == "tuner" && in.type == V4L2_INPUT_TYPE_TUNER) want = (int)i;
        if (o.source == "composite" && icontains(n, "composite")) want = (int)i;
        if (o.source == "svideo" && (icontains(n, "s-video") || icontains(n, "svideo"))) want = (int)i;
    }
    if (want < 0) { e.error = fmt_str("this device has no '%s' input", o.source.c_str()); return false; }
    unsigned idx = (unsigned)want;
    if (xioctl(vfd, VIDIOC_S_INPUT, &idx) < 0 && errno != EBUSY) logf("warning: cannot select the %s input: %s", o.source.c_str(), strerror(errno));
    v4l2_std_id std = o.pal ? V4L2_STD_PAL : V4L2_STD_NTSC;
    if (xioctl(vfd, VIDIOC_S_STD, &std) < 0 && errno != EBUSY) logf("warning: cannot set the %s standard: %s", o.pal ? "PAL" : "NTSC", strerror(errno));
    return true;
}

bool V4lBackend::tune(Engine &e)
{
    const Options &o = e.o;
    if (o.source != "tuner" || vfd < 0) return true;
    if (!tuner_ok) { e.error = "no TV tuner found"; return false; }
    if (o.channel <= 0) { e.error = "choose a channel"; return false; }
    double mhz = tv_channel_mhz(o.channel, o.cable);
    if (mhz <= 0) { e.error = fmt_str("channel %d does not exist on %s", o.channel, o.cable ? "cable" : "an antenna"); return false; }
    struct v4l2_tuner t;
    memset(&t, 0, sizeof t);
    t.index = 0;
    xioctl(vfd, VIDIOC_G_TUNER, &t);
    struct v4l2_frequency fr;
    memset(&fr, 0, sizeof fr);
    fr.tuner = 0;
    fr.type = V4L2_TUNER_ANALOG_TV;
    /* units of 62.5 kHz, or 62.5 Hz if the tuner says V4L2_TUNER_CAP_LOW */
    fr.frequency = (uint32_t)(mhz * ((t.capability & V4L2_TUNER_CAP_LOW) ? 16000.0 : 16.0) + 0.5);
    if (xioctl(vfd, VIDIOC_S_FREQUENCY, &fr) < 0) logf("warning: tuner refused channel %d (%.2f MHz): %s", o.channel, mhz, strerror(errno));
    return true;
}

bool V4lBackend::run(Engine &e)
{
    for (size_t i = 0; i < maps.size(); i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned)i;
        xioctl(vfd, VIDIOC_QBUF, &b);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) { e.error = fmt_str("Cannot start the tuner: %s", strerror(errno)); return false; }
    streaming = true;
    running = true;
    tv = std::thread([this] { video_loop(); });
    if (e.have_vbi) tb = std::thread([this] { vbi_loop(); });
    if (e.have_audio) ta = std::thread([this] { audio_loop(); });
    return true;
}

void V4lBackend::stop(Engine &)
{
    running = false;
    if (tv.joinable()) tv.join();
    if (tb.joinable()) tb.join();
    if (ta.joinable()) ta.join();
    if (streaming && vfd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(vfd, VIDIOC_STREAMOFF, &type);
        streaming = false;
    }
}

void V4lBackend::close(Engine &)
{
    if (pcm) { alsa::A.pcm_close(pcm); pcm = nullptr; }
    for (auto &m : maps) if (m.p) munmap(m.p, m.len);
    maps.clear();
    if (bfd >= 0) { ::close(bfd); bfd = -1; }
    if (vfd >= 0) { ::close(vfd); vfd = -1; }
}

void V4lBackend::video_loop()
{
    std::vector<uint8_t> tight;
    while (running) {
        struct pollfd p = { vfd, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if (r <= 0) continue;
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) continue;
        if (b.index < maps.size() && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
            i64 t = ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
                        ? (i64)b.timestamp.tv_sec * 10000000 + (i64)b.timestamp.tv_usec * 10
                        : plat::now_100ns();
            const uint8_t *src = (const uint8_t *)maps[b.index].p;
            if (bytesperline == E->vw * 2) E->video(t, src, (long)b.bytesused);
            else { /* rows are padded: hand over a tightly packed picture */
                tight.resize(E->vbytes);
                for (int y = 0; y < E->vh; y++) memcpy(&tight[(size_t)y * E->vw * 2], src + (size_t)y * bytesperline, (size_t)E->vw * 2);
                E->video(t, tight.data(), (long)tight.size());
            }
        }
        xioctl(vfd, VIDIOC_QBUF, &b);
    }
}

/* One read() is one frame of raw VBI: the first count[0] lines are field 1, the next count[1] lines field 2. */
void V4lBackend::vbi_loop()
{
    std::vector<uint8_t> buf(vbi_size);
    while (running) {
        struct pollfd p = { bfd, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        ssize_t n = read(bfd, buf.data(), buf.size());
        if (n < (ssize_t)buf.size()) continue;
        i64 t = plat::now_100ns();
        E->cb_b++;
        E->vbi_push(1, t, buf.data(), (int)vbi_count[0], (int)vbi_samples, (int)vbi_samples, E->vbi_fs);
        E->vbi_push(2, t, buf.data() + (size_t)vbi_count[0] * vbi_samples, (int)vbi_count[1], (int)vbi_samples, (int)vbi_samples, E->vbi_fs);
    }
}

void V4lBackend::audio_loop()
{
    const int frames = 960; /* 20 ms */
    std::vector<int16_t> buf((size_t)frames * 2);
    while (running) {
        long n = alsa::A.pcm_readi(pcm, buf.data(), frames);
        if (n < 0) {
            if (alsa::A.pcm_recover(pcm, (int)n, 1) < 0) { plat::sleep_ms(50); }
            continue;
        }
        if (n == 0) continue;
        /* the chunk ended about now; the clock is the one V4L2 stamps the pictures with */
        i64 t = plat::now_100ns() - (i64)n * 10000000 / 48000;
        E->audio_in(t, (const uint8_t *)buf.data(), (long)n * 4);
    }
}

void V4lBackend::list_devices()
{
    printf("Video capture devices:\n");
    for (int i = 0; i < 64; i++) {
        std::string path = "/dev/video" + std::to_string(i);
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof cap);
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            if (caps & V4L2_CAP_VIDEO_CAPTURE)
                printf("  %s  %s (driver %s, %s)\n", path.c_str(), cstr(cap.card, sizeof cap.card).c_str(), cstr(cap.driver, sizeof cap.driver).c_str(),
                       cstr(cap.bus_info, sizeof cap.bus_info).c_str());
        }
        ::close(fd);
    }
    printf("ALSA cards (/proc/asound/cards):\n");
    if (FILE *f = fopen("/proc/asound/cards", "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) printf("  %s", line);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    V4lBackend be;
    return tv_main(argc, argv, be);
}
