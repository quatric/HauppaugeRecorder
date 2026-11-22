/*
 * Client for the capture helper that drives analog TV tuners (Hauppauge WinTV
 * HVR-950Q and similar): the helper is a separate program per platform
 * (wintv-capture on Windows, tv-capture-linux on Linux) that owns the tuner,
 * decodes the line 21 captions and encodes MPEG-2. The app starts it, sends it
 * text commands on its stdin and receives framed messages on its stdout. The
 * protocol is described in docs/TVHELPER.md.
 */
#ifndef HDPVR_TVDEV_H
#define HDPVR_TVDEV_H

#include <stdint.h>

typedef struct tvdev tvdev_t;

/* Called on the client's reader thread. */
typedef struct {
    /* state: 0 searching, 1 no driver/problem, 2 starting, 3 no signal, 4 streaming (app_state_t) */
    void (*state)(void *user, int state, const char *msg, int w, int h, int fps_x100, int wide);
    void (*frame)(void *user, const uint8_t *bgrx, int w, int h);
    void (*levels)(void *user, float left_db, float right_db);
    void (*audio)(void *user, const int16_t *stereo, int frames); /* 48 kHz */
    void (*record_started)(void *user);
    void (*record_done)(void *user, int ok, const char *path, const char *err, int frames, int cc_frames);
    void (*progress)(void *user, double seconds, uint64_t bytes);
    void *user;
} tv_events_t;

/* 1 if the helper for this platform is installed next to the program. */
int tv_helper_available(void);
/* Starts the helper; NULL if it is missing or cannot be started. */
tvdev_t *tv_open(const tv_events_t *ev);
/* One command line, e.g. tv_send(t, "channel %d", 3). Never blocks for long. */
void tv_send(tvdev_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Asks the helper to finish (a recording in progress is closed) and waits for it. */
void tv_close(tvdev_t *t);

#endif
