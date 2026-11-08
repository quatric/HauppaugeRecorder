/*
 * hdpvr - userspace driver for the Hauppauge HD PVR (model 1212).
 * Protocol notes: docs/PROTOCOL.md.
 */
#include "hdpvr.h"

#include <libusb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define REQ_OUT       0x38 /* vendor | device | out */
#define REQ_IN        0xb8
#define CTRL_INDEX    0x0003

#define V_STATUS      0x0400
#define V_START       0x0700
#define V_STOP        0x0800
#define V_BITRATE     0x1000
#define V_RATE_MODE   0x1200
#define V_GOP         0x1300
#define V_VIDEO_INFO  0x1400
#define V_VIDEO_IN    0x1500
#define V_VIDEO_STD   0x1700
#define V_AUDIO_IN    0x2500
#define V_BRIGHTNESS  0x2900
#define V_CONTRAST    0x2a00
#define V_HUE         0x2b00
#define V_SATURATION  0x2c00
#define V_SHARPNESS   0x2d00
#define V_FILTER      0x3100

#define CTRL_TIMEOUT_MS 2000
#define BOOT_TIMEOUT_MS 10000 /* status and unlock: a box that just powered on is still booting */
#define NXFERS          16
#define XFER_SIZE       (64 * 1024)
#define STALL_MS        1500  /* no data for this long: restart the encoder */
#define SETTLE_MS       3000  /* after configuring, a start sooner than ~2 s never sends data */
#define FIRST_DATA_MS   10000 /* the encoder needs a few seconds to send its first data after a start */
#define RESTART_WAIT_MS 4000  /* the firmware needs this after a stop */

static const uint16_t known_pids[] = { 0x4900, 0x4901, 0x4902, 0x4903, 0x4982 };
#define HCW_VID 0x2040

struct hdpvr {
    libusb_context *ctx;
    libusb_device_handle *h;
    uint8_t ep;
    int claimed;
    hdpvr_info_t info;
    hdpvr_options_t opts;

    struct libusb_transfer *xfers[NXFERS];
    uint8_t *bufs[NXFERS];
    volatile int active;      /* transfers in flight */
    volatile int streaming;   /* encoder running and transfers being resubmitted */
    volatile int quit;
    pthread_t thread;
    int thread_started;
    hdpvr_data_cb cb;
    void *user;
    volatile uint64_t bytes;
    int ctrl_timeout_ms;
    volatile int64_t last_data_ms;
    int64_t configured_ms; /* when the last configuration change was sent */
    volatile int got_data; /* data arrived since the last encoder start */
    int restarts;
};

static int g_verbose;
void hdpvr_set_verbose(int level) { g_verbose = level; }

static void dbg(int level, const char *fmt, ...)
{
    if (g_verbose < level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("hdpvr: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

static int64_t now_ms(void)
{
    struct timespec ts;
#ifdef __APPLE__
    struct timeval tv;
    gettimeofday(&tv, NULL); /* clock_gettime() needs macOS 10.12 */
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = (long)tv.tv_usec * 1000;
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const char *hdpvr_strerror(int err)
{
    switch (err) {
    case HDPVR_ERR_NO_DEVICE: return "no Hauppauge HD PVR found";
    case HDPVR_ERR_NO_SIGNAL: return "no video signal on the selected input";
    case HDPVR_ERR_UNLOCK: return "the HD PVR did not accept the unlock handshake";
    case HDPVR_ERR_NO_MEMORY: return "out of memory";
    case HDPVR_ERR_NO_DRIVER:
        return "an HD PVR is plugged in, but it does not use the WinUSB driver, so it cannot be opened";
    }
    return libusb_strerror((enum libusb_error)err);
}

/* ---- control requests ---- */

static int ctrl_out(hdpvr_t *dev, uint8_t req, uint16_t value, uint16_t index, const uint8_t *data, uint16_t len)
{
    uint8_t buf[8] = { 0 };
    if (len > sizeof(buf))
        return LIBUSB_ERROR_INVALID_PARAM;
    if (len)
        memcpy(buf, data, len);
    int r = libusb_control_transfer(dev->h, REQ_OUT, req, value, index, len ? buf : NULL, len, dev->ctrl_timeout_ms ? dev->ctrl_timeout_ms : CTRL_TIMEOUT_MS);
    dbg(2, "out req 0x%02x value 0x%04x len %d -> %d", req, value, len, r);
    if (r < 0)
        return r;
    return r == len ? 0 : LIBUSB_ERROR_IO;
}

static int ctrl_in(hdpvr_t *dev, uint16_t value, uint8_t *buf, uint16_t len)
{
    int r = libusb_control_transfer(dev->h, REQ_IN, 0x81, value, CTRL_INDEX, buf, len, dev->ctrl_timeout_ms ? dev->ctrl_timeout_ms : CTRL_TIMEOUT_MS);
    if (g_verbose >= 2) {
        char hex[3 * 64 + 1] = "";
        for (int i = 0; i < r && i < 64; i++)
            snprintf(hex + 3 * i, 4, " %02x", buf[i]);
        dbg(2, "in value 0x%04x len %d -> %d:%s", value, len, r, hex);
    }
    return r;
}

static int set1(hdpvr_t *dev, uint16_t value, uint8_t v)
{
    return ctrl_out(dev, 0x01, value, CTRL_INDEX, &v, 1);
}

/* ---- unlock (docs/PROTOCOL.md, "Unlock") ---- */

static void mix64(uint8_t *b, unsigned shift)
{
    uint64_t x = 0;
    for (int i = 7; i >= 0; i--)
        x = x << 8 | b[i];
    x += x << shift;
    for (int i = 0; i < 8; i++, x >>= 8)
        b[i] = (uint8_t)x;
}

static void answer_challenge(uint8_t b[8])
{
    for (unsigned n = 0; n < 32; n++) {
        if (n % 4)
            b[n / 8 + 3] = b[(n / 4) % 4];
        switch (n % 4) {
        case 0:
            b[3 - n / 8] = b[n / 4];
            b[5] = (uint8_t)(b[5] + b[6] * 3);
            for (int k = 0; k < 3; k++)
                b[3] = (uint8_t)(b[3] * (b[3] + 1));
            break;
        case 1:
            b[0] = (uint8_t)(b[0] * 8 + 7 * n + 4);
            b[6] = (uint8_t)(b[6] + b[3] * 3);
            break;
        case 2:
            for (int k = 0; k < 3; k++)
                b[1] = (uint8_t)(b[1] * (b[6] + 1));
            for (int k = 0; k < 3; k++)
                mix64(b, b[7] & 15);
            break;
        case 3:
            b[2] = (uint8_t)(b[2] + b[3] * 4 + b[4] + b[5]);
            b[4] = (uint8_t)(b[4] + b[(n % 2) * 2] * 9 + 9);
            break;
        }
    }
}

static int unlock(hdpvr_t *dev)
{
    uint8_t st[46];
    int r = ctrl_in(dev, V_STATUS, st, sizeof(st));
    if (r != (int)sizeof(st)) {
        dbg(0, "status request returned %d", r);
        return r < 0 ? r : HDPVR_ERR_UNLOCK;
    }
    dev->info.firmware = st[1];
    size_t n = 0;
    for (; n < sizeof(dev->info.firmware_date) - 1 && 2 + n < 38 && st[2 + n] >= 0x20 && st[2 + n] < 0x7f; n++)
        dev->info.firmware_date[n] = (char)st[2 + n];
    dev->info.firmware_date[n] = 0;
    dev->info.ac3_capable = dev->info.firmware >= 0x0d;
    dbg(1, "firmware 0x%02x dated %s", dev->info.firmware, dev->info.firmware_date);

    uint8_t resp[8];
    memcpy(resp, st + 38, 8);
    answer_challenge(resp);
    msleep(100);
    r = ctrl_out(dev, 0xd1, 0, 0, resp, 8);
    if (r) {
        dbg(0, "unlock request failed: %s", hdpvr_strerror(r));
        return HDPVR_ERR_UNLOCK;
    }
    return 0;
}

/* ---- open/close ---- */

static int is_hdpvr(const struct libusb_device_descriptor *d)
{
    if (d->idVendor != HCW_VID)
        return 0;
    for (size_t i = 0; i < sizeof(known_pids) / sizeof(known_pids[0]); i++)
        if (d->idProduct == known_pids[i])
            return 1;
    return 0;
}

static int find_bulk_in(libusb_device *udev, uint8_t *ep)
{
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(udev, &cfg);
    if (r)
        return r;
    r = LIBUSB_ERROR_NOT_FOUND;
    if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
        const struct libusb_interface_descriptor *d = &cfg->interface[0].altsetting[0];
        for (int e = 0; e < d->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *x = &d->endpoint[e];
            if ((x->bEndpointAddress & LIBUSB_ENDPOINT_IN) &&
                (x->bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_BULK) {
                *ep = x->bEndpointAddress;
                r = 0;
                break;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return r;
}

static libusb_device_handle *open_first(libusb_context *ctx, int *found, int *err, uint16_t *vid, uint16_t *pid)
{
    libusb_device **list;
    libusb_device_handle *h = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        *err = (int)n;
        return NULL;
    }
    for (ssize_t i = 0; i < n && !h; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) || !is_hdpvr(&d))
            continue;
        (*found)++;
        int r = libusb_open(list[i], &h);
        if (r) {
            dbg(0, "found %04x:%04x but cannot open it: %s", d.idVendor, d.idProduct, libusb_strerror(r));
            *err = r;
            h = NULL;
        } else {
            *vid = d.idVendor;
            *pid = d.idProduct;
        }
    }
    libusb_free_device_list(list, 1);
    return h;
}

void hdpvr_default_options(hdpvr_options_t *o)
{
    memset(o, 0, sizeof(*o));
    o->video_input = HDPVR_IN_COMPONENT;
    o->audio_input = HDPVR_AUDIO_RCA_BACK;
    o->kbps = 6500;
    o->peak_kbps = 9000;
    o->rate_mode = HDPVR_CBR;
    o->gop = 1; /* simple GOP with IDR */
    o->brightness = o->contrast = o->hue = o->saturation = o->sharpness = -1;
}

int hdpvr_open(hdpvr_t **out)
{
    hdpvr_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return HDPVR_ERR_NO_MEMORY;
    int r = libusb_init_context(&dev->ctx, NULL, 0);
    if (r) {
        free(dev);
        return r;
    }
    int found = 0, err = 0;
    dev->h = open_first(dev->ctx, &found, &err, &dev->info.vid, &dev->info.pid);
#ifdef _WIN32
    if (!dev->h) {
        libusb_context *dk = NULL;
        const struct libusb_init_option opt = { .option = LIBUSB_OPTION_USE_USBDK };
        if (libusb_init_context(&dk, &opt, 1) == 0) {
            int f2 = 0, e2 = 0;
            libusb_device_handle *h = open_first(dk, &f2, &e2, &dev->info.vid, &dev->info.pid);
            if (h) {
                libusb_exit(dev->ctx);
                dev->ctx = dk;
                dev->h = h;
            } else {
                libusb_exit(dk);
            }
        }
    }
#endif
    if (!dev->h) {
        libusb_exit(dev->ctx);
        free(dev);
        if (!found)
            return HDPVR_ERR_NO_DEVICE;
#ifdef _WIN32
        if (err == LIBUSB_ERROR_NOT_SUPPORTED || err == LIBUSB_ERROR_NOT_FOUND)
            return HDPVR_ERR_NO_DRIVER;
#endif
        return err ? err : HDPVR_ERR_NO_DEVICE;
    }

    libusb_set_auto_detach_kernel_driver(dev->h, 1); /* Linux: the hdpvr kernel driver */
    if ((r = find_bulk_in(libusb_get_device(dev->h), &dev->ep)) ||
        (r = libusb_claim_interface(dev->h, 0))) {
        dbg(0, "cannot use interface 0: %s", libusb_strerror(r));
        hdpvr_close(dev);
        return r;
    }
    dev->claimed = 1;
    dbg(1, "HD PVR %04x:%04x, stream on endpoint 0x%02x", dev->info.vid, dev->info.pid, dev->ep);

    dev->ctrl_timeout_ms = BOOT_TIMEOUT_MS;
    r = unlock(dev);
    dev->ctrl_timeout_ms = 0;
    if (r) {
        hdpvr_close(dev);
        return r;
    }
    /* filter defaults, fan and LEDs on */
    static const uint8_t filter[4] = { 0x03, 0x03, 0x00, 0x00 };
    uint8_t one = 1;
    ctrl_out(dev, 0x01, V_FILTER, CTRL_INDEX, filter, 4);
    ctrl_out(dev, 0xd4, 0, 0, &one, 1);

    hdpvr_options_t o;
    hdpvr_default_options(&o);
    if ((r = hdpvr_configure(dev, &o))) {
        hdpvr_close(dev);
        return r;
    }
    *out = dev;
    return 0;
}

void hdpvr_close(hdpvr_t *dev)
{
    if (!dev)
        return;
    hdpvr_stop(dev);
    if (dev->h) {
        if (dev->claimed)
            libusb_release_interface(dev->h, 0);
        libusb_close(dev->h);
    }
    if (dev->ctx)
        libusb_exit(dev->ctx);
    free(dev);
}

const hdpvr_info_t *hdpvr_info(hdpvr_t *dev) { return &dev->info; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int hdpvr_configure(hdpvr_t *dev, const hdpvr_options_t *o)
{
    hdpvr_options_t c = *o;
    c.kbps = clampi(c.kbps, 1000, 13500);
    c.peak_kbps = clampi(c.peak_kbps, c.kbps + 100, 20200);
    int newfw = dev->info.firmware > 0x15;
    static const int def_new[5] = { 0x80, 0x40, 0x0f, 0x40, 0x80 }, def_old[5] = { 0x86, 0x80, 0x80, 0x80, 0x80 };
    const int *def = newfw ? def_new : def_old;
    int pic[5] = { c.brightness, c.contrast, c.hue, c.saturation, c.sharpness };
    static const uint16_t pic_reg[5] = { V_BRIGHTNESS, V_CONTRAST, V_HUE, V_SATURATION, V_SHARPNESS };

    int r = 0;
    r |= set1(dev, V_VIDEO_STD, c.pal ? 1 : 0);
    r |= set1(dev, V_VIDEO_IN, (uint8_t)(c.video_input + 1));
    if (dev->info.ac3_capable) {
        uint8_t a[2] = { (uint8_t)(c.audio_input + 1), (uint8_t)(c.ac3 ? 1 : 0) };
        r |= ctrl_out(dev, 0x01, V_AUDIO_IN, CTRL_INDEX, a, 2);
    } else {
        r |= set1(dev, V_AUDIO_IN, (uint8_t)(c.audio_input + 1));
        c.ac3 = 0;
    }
    uint16_t avg = (uint16_t)(c.kbps / 100), peak = (uint16_t)(c.peak_kbps / 100);
    uint8_t rate[4] = { (uint8_t)avg, (uint8_t)(avg >> 8), (uint8_t)peak, (uint8_t)(peak >> 8) };
    r |= ctrl_out(dev, 0x01, V_BITRATE, CTRL_INDEX, rate, 4);
    r |= set1(dev, V_RATE_MODE, (uint8_t)c.rate_mode);
    r |= set1(dev, V_GOP, (uint8_t)(c.gop & 3));
    for (int i = 0; i < 5; i++)
        r |= set1(dev, pic_reg[i], (uint8_t)(pic[i] < 0 ? def[i] : clampi(pic[i], 0, 255)));
    uint8_t boost = c.audio_boost ? 1 : 0;
    r |= ctrl_out(dev, 0xd5, 0, 0, &boost, 1);
    dev->opts = c;
    dev->configured_ms = now_ms();
    return r ? LIBUSB_ERROR_IO : 0;
}

int hdpvr_get_signal(hdpvr_t *dev, hdpvr_signal_t *s)
{
    uint8_t b[5];
    memset(s, 0, sizeof(*s));
    int r = ctrl_in(dev, V_VIDEO_INFO, b, 5);
    if (r < 0)
        return r;
    if (r < 5)
        return LIBUSB_ERROR_IO;
    s->width = b[0] | b[1] << 8;
    s->height = b[2] | b[3] << 8;
    s->fps = b[4];
    if (!s->width || !s->height || !s->fps)
        s->width = s->height = s->fps = 0;
    return 0;
}

/* ---- streaming ---- */

static void LIBUSB_CALL xfer_cb(struct libusb_transfer *x)
{
    hdpvr_t *dev = x->user_data;
    if (x->status == LIBUSB_TRANSFER_COMPLETED && x->actual_length > 0) {
        dev->bytes += (uint64_t)x->actual_length;
        dev->last_data_ms = now_ms();
        dev->got_data = 1;
        if (dev->cb)
            dev->cb(x->buffer, (size_t)x->actual_length, dev->user);
    }
    if (dev->streaming && !dev->quit &&
        (x->status == LIBUSB_TRANSFER_COMPLETED || x->status == LIBUSB_TRANSFER_TIMED_OUT) &&
        libusb_submit_transfer(x) == 0)
        return;
    dev->active--;
}

static void cancel_transfers(hdpvr_t *dev)
{
    dev->streaming = 0;
    for (int i = 0; i < NXFERS; i++)
        if (dev->xfers[i])
            libusb_cancel_transfer(dev->xfers[i]);
    struct timeval tv = { 0, 50000 };
    for (int i = 0; dev->active > 0 && i < 100; i++)
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
}

/* read what is left in the device after Stop, so the next start is quick */
static void drain(hdpvr_t *dev)
{
    uint8_t *buf = malloc(XFER_SIZE);
    int got, n = 0;
    while (buf && n++ < 500 && libusb_bulk_transfer(dev->h, dev->ep, buf, XFER_SIZE, &got, 90) == 0)
        ;
    free(buf);
    dbg(1, "emptied the device in %d reads", n - 1);
}

static int encoder_start(hdpvr_t *dev, int timeout_ms)
{
    hdpvr_signal_t s;
    int64_t until = now_ms() + timeout_ms;
    for (;;) {
        int r = hdpvr_get_signal(dev, &s);
        if (r)
            return r;
        if (s.width)
            break;
        if (now_ms() >= until)
            return HDPVR_ERR_NO_SIGNAL;
        msleep(250);
    }
    dbg(1, "signal %dx%d at %d Hz", s.width, s.height, s.fps);
    int64_t settle = dev->configured_ms + SETTLE_MS - now_ms();
    if (settle > 0) {
        dbg(1, "letting the encoder settle for %d ms", (int)settle);
        msleep((int)settle);
    }
    int r = ctrl_out(dev, 0xb8, 0x0001, 0, NULL, 0);
    if (!r)
        r = set1(dev, V_START, 0);
    if (r)
        return r;

    dev->streaming = 1;
    dev->got_data = 0;
    dev->last_data_ms = now_ms();
    for (int i = 0; i < NXFERS; i++) {
        libusb_fill_bulk_transfer(dev->xfers[i], dev->h, dev->ep, dev->bufs[i], XFER_SIZE, xfer_cb, dev, 0);
        if ((r = libusb_submit_transfer(dev->xfers[i]))) {
            dbg(0, "submit bulk transfer: %s", libusb_strerror(r));
            cancel_transfers(dev);
            set1(dev, V_STOP, 0);
            return r;
        }
        dev->active++;
    }
    return 0;
}

static void encoder_stop(hdpvr_t *dev)
{
    set1(dev, V_STOP, 0);
    cancel_transfers(dev);
    drain(dev);
}

static void *event_thread(void *arg)
{
    hdpvr_t *dev = arg;
    while (!dev->quit) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
        int limit = dev->got_data ? STALL_MS : FIRST_DATA_MS;
        if (dev->streaming && now_ms() - dev->last_data_ms > limit) {
            /* the firmware sometimes stops sending; restart it like the Windows driver does */
            dbg(0, "no data for %d ms, restarting the encoder", limit);
            encoder_stop(dev);
            for (int t = 0; t < RESTART_WAIT_MS / 100 && !dev->quit; t++)
                msleep(100);
            if (dev->quit)
                break;
            dev->restarts++;
            int r = encoder_start(dev, 10000);
            if (r) {
                dbg(0, "restart failed: %s", hdpvr_strerror(r));
                dev->last_data_ms = now_ms(); /* try again after another stall period */
                dev->streaming = 1;
            }
        }
    }
    return NULL;
}

int hdpvr_start(hdpvr_t *dev, int timeout_ms, hdpvr_data_cb cb, void *user)
{
    if (dev->thread_started)
        return 0;
    for (int i = 0; i < NXFERS; i++) {
        dev->xfers[i] = libusb_alloc_transfer(0);
        dev->bufs[i] = malloc(XFER_SIZE);
        if (!dev->xfers[i] || !dev->bufs[i]) {
            hdpvr_stop(dev);
            return HDPVR_ERR_NO_MEMORY;
        }
    }
    dev->cb = cb;
    dev->user = user;
    dev->bytes = 0;
    dev->restarts = 0;
    dev->quit = 0;
    int r = encoder_start(dev, timeout_ms);
    if (r) {
        hdpvr_stop(dev);
        return r;
    }
    if (pthread_create(&dev->thread, NULL, event_thread, dev)) {
        encoder_stop(dev);
        hdpvr_stop(dev);
        return HDPVR_ERR_NO_MEMORY;
    }
    dev->thread_started = 1;
    return 0;
}

void hdpvr_stop(hdpvr_t *dev)
{
    if (dev->thread_started) {
        dev->quit = 1;
        pthread_join(dev->thread, NULL);
        dev->thread_started = 0;
    }
    if (dev->streaming || dev->active)
        encoder_stop(dev);
    for (int i = 0; i < NXFERS; i++) {
        if (dev->xfers[i])
            libusb_free_transfer(dev->xfers[i]);
        free(dev->bufs[i]);
        dev->xfers[i] = NULL;
        dev->bufs[i] = NULL;
    }
}

uint64_t hdpvr_bytes(hdpvr_t *dev) { return dev->bytes; }
int hdpvr_restarts(hdpvr_t *dev) { return dev->restarts; }
