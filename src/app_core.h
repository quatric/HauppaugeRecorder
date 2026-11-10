/*
 * HauppaugeRecorder engine shared by the macOS, Windows and Linux apps: finds
 * and configures the HD PVR, keeps its encoder running, records its H.264
 * transport stream unchanged to a .ts file, and decodes it with ffmpeg for
 * the preview picture, the level meters and audio monitoring. Everything
 * here is thread safe; the GUIs poll it from a timer.
 */
#ifndef HDPVR_APP_CORE_H
#define HDPVR_APP_CORE_H

#include <stdint.h>
#include <stdio.h>

#define APP_NAME "HauppaugeRecorder"

typedef struct app app_t;

enum { APP_VIDEO_COMPONENT = 0, APP_VIDEO_SVIDEO, APP_VIDEO_COMPOSITE };
enum { APP_AUDIO_BACK = 0, APP_AUDIO_FRONT, APP_AUDIO_SPDIF };
#define APP_QUALITIES 3 /* 0 = 6.5, 1 = 10, 2 = 13.5 Mbit/s */
extern const char *const app_quality_names[APP_QUALITIES];
extern const char *const app_video_names[3];
extern const char *const app_audio_names[3];

typedef enum {
    APP_SEARCHING,  /* no HD PVR yet; retried every couple of seconds */
    APP_NO_DRIVER,  /* Windows: plugged in, but without the WinUSB/libusbK driver */
    APP_STARTING,   /* configuring, or waiting for the encoder */
    APP_NO_SIGNAL,  /* no picture on the selected input */
    APP_STREAMING,
} app_state_t;

/* Starts the engine; it connects to the HD PVR in the background. */
app_t *app_start(int video_input, int audio_input, int quality, int pal);
void app_close(app_t *a); /* finishes a recording in progress */
void app_retry(app_t *a); /* look for the device again now (after installing the driver) */

/* Changes the source or bitrate; restarts the encoder (a few seconds). Ignored while recording. */
void app_set_source(app_t *a, int video_input, int audio_input, int quality, int pal);

app_state_t app_state(app_t *a);
/* One line for the status bar: what the engine is doing, or the last problem. */
void app_status(app_t *a, char *out, int len);
/* Input signal, e.g. 1920x1080 at 60; 0 when there is none. */
int app_signal(app_t *a, int *w, int *h, int *fps);

/* Preview frames: 32-bit pixels 0x00RRGGBB (BGRX bytes, a Windows DIB and
 * CAIRO_FORMAT_RGB24). The size follows the signal: 720x480/576 for SD,
 * 1280x720 for HD. app_get_frame copies the latest frame into buf if it is
 * w x h and newer than *seq, and returns 1 then. */
void app_frame_size(app_t *a, int *w, int *h);
int app_get_frame(app_t *a, uint32_t *buf, int w, int h, uint64_t *seq);
/* Display aspect of the picture: 16:9 for HD, 4:3 or 16:9 (widescreen) for SD. */
double app_aspect(app_t *a);
/* Off stops decoding the picture (the sound and recording go on) so a slow computer has more CPU; snapshots
 * need it on. Takes effect within a moment, also while streaming. Default on. */
void app_set_preview(app_t *a, int on);
int app_preview(app_t *a);
void app_set_widescreen(app_t *a, int on); /* SD only: preview, snapshots, and the 16:9 flag in recordings started after */

/* Audio: peak levels since the last call in dBFS (-60..0), monitoring. */
void app_peaks(app_t *a, double db[2]);
void app_set_monitor(app_t *a, int on);
/* Fills frames of 48 kHz stereo for the speakers (macOS calls this from its
 * own audio output; Windows and Linux play it themselves). Returns 0 if silent. */
int app_audio_render(app_t *a, int16_t *out, int frames);
int app_audio_render_float(app_t *a, float *left, float *right, int frames);
const char *app_audio_problem(app_t *a); /* NULL when the preview and sound work */

/* Recording: the transport stream as it comes from the box, to a .ts file. */
int app_record_start(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen);
/* Stops in the background; done(user, ok, path) runs on a worker thread. */
typedef void (*app_done_cb)(void *user, int ok, const char *path);
void app_record_stop(app_t *a, app_done_cb done, void *user);
int app_is_recording(app_t *a);
double app_record_seconds(app_t *a);
/* "Stop after": the choices (minutes; 0 = no limit) and their names, shared by every GUI. The GUI polls
 * app_stop_after_due() and stops the recording itself when it returns 1. */
#define APP_STOP_AFTERS 9
extern const int app_stop_after_minutes[APP_STOP_AFTERS];
extern const char *const app_stop_after_names[APP_STOP_AFTERS];
void app_set_stop_after(app_t *a, int index);
int app_stop_after_due(app_t *a);
uint64_t app_record_bytes(app_t *a);

/* Saves the current picture as a square-pixel PNG. */
int app_snapshot(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen);

/* "HauppaugeRecorder 2026-11-08 at 21.05.33.ext" in folder. */
void app_timestamped_path(const char *folder, const char *ext, char *out, int len);

/* Settings: key=value lines in the user's config folder (Windows and Linux). */
int app_setting_int(const char *key, int def);
void app_setting_str(const char *key, char *out, int len, const char *def);
void app_set_setting_int(const char *key, int v);
void app_set_setting_str(const char *key, const char *v);

/* Default recordings folder: Videos (Windows), XDG videos dir or ~/Videos. */
void app_default_folder(char *out, int len);

/* fopen with a UTF-8 path on every platform. */
FILE *app_fopen(const char *path, const char *mode);

#endif
