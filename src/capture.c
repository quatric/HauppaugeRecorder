/*
 * hdpvr-capture: record the Hauppauge HD PVR's H.264 transport stream.
 *
 *   hdpvr-capture -o show.ts              record until Ctrl-C
 *   hdpvr-capture -o - | ffplay -         watch it live
 */
#include "hdpvr.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static volatile int g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static FILE *g_out;
static volatile int g_write_error;

static void on_data(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!g_write_error && fwrite(data, 1, len, g_out) != len)
        g_write_error = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -o, --output FILE        transport stream file, - for stdout (default -)\n"
            "  -i, --input component|svideo|composite   video input (default component)\n"
            "  -a, --audio back|front|spdif              audio input (default back RCA)\n"
            "      --ac3                AC-3 audio instead of AAC (newer firmware)\n"
            "      --pal                50 Hz on the S-Video and composite inputs\n"
            "  -b, --bitrate MBPS       average video bitrate, 1-13.5 (default 6.5)\n"
            "  -p, --peak MBPS          peak bitrate for VBR, up to 20.2 (default 9)\n"
            "      --vbr                variable bitrate (default constant)\n"
            "      --brightness/--contrast/--hue/--saturation/--sharpness 0..255\n"
            "      --boost              boost the analog audio\n"
            "  -t, --time SECONDS       stop after this many seconds\n"
            "      --info               print device and signal information and exit\n"
            "  -v, --verbose            more logging (repeat for USB traces)\n",
            argv0);
}

enum { OPT_AC3 = 256, OPT_PAL, OPT_VBR, OPT_INFO, OPT_BOOST, OPT_BRI, OPT_CON, OPT_HUE, OPT_SAT, OPT_SHA };

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "output", required_argument, 0, 'o' },   { "input", required_argument, 0, 'i' },
        { "audio", required_argument, 0, 'a' },    { "ac3", no_argument, 0, OPT_AC3 },
        { "pal", no_argument, 0, OPT_PAL },        { "bitrate", required_argument, 0, 'b' },
        { "peak", required_argument, 0, 'p' },     { "vbr", no_argument, 0, OPT_VBR },
        { "time", required_argument, 0, 't' },     { "info", no_argument, 0, OPT_INFO },
        { "boost", no_argument, 0, OPT_BOOST },    { "brightness", required_argument, 0, OPT_BRI },
        { "contrast", required_argument, 0, OPT_CON }, { "hue", required_argument, 0, OPT_HUE },
        { "saturation", required_argument, 0, OPT_SAT }, { "sharpness", required_argument, 0, OPT_SHA },
        { "verbose", no_argument, 0, 'v' },        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    hdpvr_options_t o;
    hdpvr_default_options(&o);
    const char *outpath = "-";
    double seconds = 0;
    int info = 0, verbose = 0, c;
    while ((c = getopt_long(argc, argv, "o:i:a:b:p:t:vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'o': outpath = optarg; break;
        case 'i':
            if (!strcmp(optarg, "component")) o.video_input = HDPVR_IN_COMPONENT;
            else if (!strcmp(optarg, "svideo")) o.video_input = HDPVR_IN_SVIDEO;
            else if (!strcmp(optarg, "composite")) o.video_input = HDPVR_IN_COMPOSITE;
            else { usage(argv[0]); return 2; }
            break;
        case 'a':
            if (!strcmp(optarg, "back")) o.audio_input = HDPVR_AUDIO_RCA_BACK;
            else if (!strcmp(optarg, "front")) o.audio_input = HDPVR_AUDIO_RCA_FRONT;
            else if (!strcmp(optarg, "spdif")) o.audio_input = HDPVR_AUDIO_SPDIF;
            else { usage(argv[0]); return 2; }
            break;
        case OPT_AC3: o.ac3 = 1; break;
        case OPT_PAL: o.pal = 1; break;
        case 'b': o.kbps = (int)(atof(optarg) * 1000); break;
        case 'p': o.peak_kbps = (int)(atof(optarg) * 1000); break;
        case OPT_VBR: o.rate_mode = HDPVR_VBR_PEAK; break;
        case OPT_BOOST: o.audio_boost = 1; break;
        case OPT_BRI: o.brightness = atoi(optarg); break;
        case OPT_CON: o.contrast = atoi(optarg); break;
        case OPT_HUE: o.hue = atoi(optarg); break;
        case OPT_SAT: o.saturation = atoi(optarg); break;
        case OPT_SHA: o.sharpness = atoi(optarg); break;
        case 't': seconds = atof(optarg); break;
        case OPT_INFO: info = 1; break;
        case 'v': hdpvr_set_verbose(++verbose); break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }

    hdpvr_t *dev;
    int r = hdpvr_open(&dev);
    if (r) {
        fprintf(stderr, "hdpvr-capture: %s\n", hdpvr_strerror(r));
        return 1;
    }
    const hdpvr_info_t *in = hdpvr_info(dev);
    if (info) {
        hdpvr_signal_t s;
        printf("device              %04x:%04x\n", in->vid, in->pid);
        printf("firmware            0x%02x (%s)%s\n", in->firmware, in->firmware_date,
               in->ac3_capable ? ", AC-3 capable" : "");
        /* the encoder needs a moment to lock onto a newly selected input */
        int ok = hdpvr_configure(dev, &o) == 0 && hdpvr_get_signal(dev, &s) == 0;
        for (int i = 0; ok && !s.width && i < 12; i++) {
            usleep(250000);
            ok = hdpvr_get_signal(dev, &s) == 0;
        }
        if (ok) {
            if (s.width)
                printf("signal              %dx%d at %d Hz\n", s.width, s.height, s.fps);
            else
                printf("signal              none on the selected input\n");
        }
        hdpvr_close(dev);
        return 0;
    }
    if ((r = hdpvr_configure(dev, &o))) {
        fprintf(stderr, "hdpvr-capture: configuring the device failed: %s\n", hdpvr_strerror(r));
        hdpvr_close(dev);
        return 1;
    }

    g_out = strcmp(outpath, "-") ? fopen(outpath, "wb") : stdout;
    if (!g_out) {
        perror(outpath);
        hdpvr_close(dev);
        return 1;
    }
#ifdef _WIN32
    if (g_out == stdout)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "hdpvr-capture: waiting for a picture...\n");
    if ((r = hdpvr_start(dev, 10000, on_data, NULL))) {
        fprintf(stderr, "hdpvr-capture: %s\n", hdpvr_strerror(r));
        hdpvr_close(dev);
        return 1;
    }
    hdpvr_signal_t s;
    if (hdpvr_get_signal(dev, &s) == 0 && s.width)
        fprintf(stderr, "hdpvr-capture: recording %dx%d at %d Hz, %.1f Mbit/s, Ctrl-C to stop\n", s.width,
                s.height, s.fps, o.kbps / 1000.0);

    time_t start = time(NULL);
    while (!g_stop && !g_write_error && (seconds <= 0 || difftime(time(NULL), start) < seconds))
        usleep(100000);

    hdpvr_stop(dev);
    uint64_t bytes = hdpvr_bytes(dev);
    int restarts = hdpvr_restarts(dev);
    hdpvr_close(dev);
    if (g_out != stdout)
        fclose(g_out);
    else
        fflush(stdout);
    fprintf(stderr, "hdpvr-capture: %.1f MB in %.0f s%s", bytes / 1e6, difftime(time(NULL), start),
            g_write_error ? ", stopped by a write error" : "");
    if (restarts)
        fprintf(stderr, ", encoder restarted %d times", restarts);
    fputc('\n', stderr);
    return g_write_error ? 1 : 0;
}
