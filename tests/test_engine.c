/* Drives the engine without a device: HDPVR_REPLAY feeds a recorded .ts.
 *   HDPVR_REPLAY=sample.ts build/test_engine OUTDIR */
#include "app_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile int done, done_ok;
static char done_path[1100];
static void rec_done(void *u, int ok, const char *p)
{
    (void)u;
    snprintf(done_path, sizeof(done_path), "%s", p);
    done_ok = ok;
    done = 1;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    app_t *a = app_start(0, 0, 0, 0);
    for (int i = 0; i < 50 && app_state(a) != APP_STREAMING; i++)
        usleep(100000);
    if (app_state(a) != APP_STREAMING) {
        fprintf(stderr, "FAIL: not streaming\n");
        return 1;
    }
    /* poll the preview at 60 Hz for 10 s, like the Mac viewer */
    int w, h;
    app_frame_size(a, &w, &h);
    uint32_t *f = calloc((size_t)w * h, 4);
    uint64_t seq = 0;
    int frames = 0, hitches = 0;
    double first = -1, last = -1, worst = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double poll0 = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
    double shown = -1; /* the first real picture (the first frame counted is the cleared one) */
    int shown_frames = 0;
    for (int i = 0; i < 600; i++) {
        if (app_get_frame(a, f, w, h, &seq)) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            double now = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
            if (first < 0)
                first = now;
            else if (now - last > 0.050) {
                hitches++;
                if (getenv("GAPS"))
                    fprintf(stderr, "gap %.0f ms at %.2f s\n", (now - last) * 1000, now);
            }
            if (last >= 0 && now - last > worst)
                worst = now - last;
            last = now;
            frames++;
            if (frames == 2)
                shown = now;
            if (frames >= 2)
                shown_frames++;
        }
        /* poll on a steady 60 Hz grid, as a display link would */
        clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
        double next = poll0 + (i + 1) / 60.0;
        if (next > el)
            usleep((useconds_t)((next - el) * 1e6));
    }
    printf("preview: picture %.2f s after start, then %.2f frames/s, %d gaps over 50 ms (worst %.0f ms incl. the wait for the first picture)\n",
           shown, (shown_frames - 1) / (last - shown), hitches, worst * 1000);
    double db[2];
    app_peaks(a, db);
    printf("frame %dx%d, %d frames, aspect %.3f, peaks %.1f/%.1f dBFS\n", w, h, frames, app_aspect(a),
           db[0], db[1]);
    char path[1100], err[300];
    if (app_snapshot(a, dir, path, sizeof(path), err, sizeof(err)))
        fprintf(stderr, "FAIL snapshot: %s\n", err);
    else
        printf("snapshot %s\n", path);
    if (getenv("WIDE"))
        app_set_widescreen(a, 1);
    if (app_record_start(a, dir, path, sizeof(path), err, sizeof(err))) {
        fprintf(stderr, "FAIL record: %s\n", err);
        return 1;
    }
    sleep(6);
    printf("recorded %.1f s, %.2f MB\n", app_record_seconds(a), app_record_bytes(a) / 1e6);
    app_record_stop(a, rec_done, NULL);
    while (!done)
        usleep(10000);
    printf("saved %s ok=%d\n", done_path, done_ok);
    app_close(a);
    return frames > 0 && done_ok ? 0 : 1;
}
