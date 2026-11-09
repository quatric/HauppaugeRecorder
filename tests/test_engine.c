/* Drives the engine without a device: HDPVR_REPLAY feeds a recorded .ts.
 *   HDPVR_REPLAY=sample.ts build/test_engine OUTDIR */
#include "app_core.h"

#include <stdio.h>
#include <stdlib.h>
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
    app_t *a = app_start(0, 0, 0, 0);
    for (int i = 0; i < 50 && app_state(a) != APP_STREAMING; i++)
        usleep(100000);
    if (app_state(a) != APP_STREAMING) {
        fprintf(stderr, "FAIL: not streaming\n");
        return 1;
    }
    usleep(2500000);
    int w, h;
    app_frame_size(a, &w, &h);
    uint32_t *f = calloc((size_t)w * h, 4);
    uint64_t seq = 0;
    int frames = 0;
    for (int i = 0; i < 100; i++) {
        frames += app_get_frame(a, f, w, h, &seq);
        usleep(10000);
    }
    double db[2];
    app_peaks(a, db);
    printf("frame %dx%d, %d new frames in 1 s, aspect %.3f, peaks %.1f/%.1f dBFS\n", w, h, frames, app_aspect(a),
           db[0], db[1]);
    char path[1100], err[300];
    if (app_snapshot(a, dir, path, sizeof(path), err, sizeof(err)))
        fprintf(stderr, "FAIL snapshot: %s\n", err);
    else
        printf("snapshot %s\n", path);
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
