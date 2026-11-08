/*
 * hdpvr - userspace driver for the Hauppauge HD PVR (model 1212)
 *
 * The box encodes H.264 + AAC/AC-3 itself and streams an MPEG-2 transport
 * stream over USB bulk. This library unlocks and configures it through
 * libusb and hands the stream to a callback. See docs/PROTOCOL.md.
 */
#ifndef HDPVR_H
#define HDPVR_H

#include <stddef.h>
#include <stdint.h>

typedef struct hdpvr hdpvr_t;

/* Errors are negative: a libusb_error value, or one of these. */
#define HDPVR_ERR_NO_DEVICE (-2001)
#define HDPVR_ERR_NO_SIGNAL (-2002)
#define HDPVR_ERR_UNLOCK    (-2003)
#define HDPVR_ERR_NO_MEMORY (-2004)
#define HDPVR_ERR_NO_DRIVER (-2005) /* Windows: present, but not bound to WinUSB */

typedef enum { HDPVR_IN_COMPONENT = 0, HDPVR_IN_SVIDEO, HDPVR_IN_COMPOSITE } hdpvr_video_input_t;
typedef enum { HDPVR_AUDIO_RCA_BACK = 0, HDPVR_AUDIO_RCA_FRONT, HDPVR_AUDIO_SPDIF } hdpvr_audio_input_t;
typedef enum { HDPVR_CBR = 1, HDPVR_VBR_PEAK, HDPVR_VBR_AVERAGE } hdpvr_rate_mode_t;

typedef struct {
    hdpvr_video_input_t video_input;
    hdpvr_audio_input_t audio_input;
    int ac3;              /* AC-3 instead of AAC (firmware 0x0d and later) */
    int pal;              /* 50 Hz for the SD inputs */
    int kbps, peak_kbps;  /* average 1000-13500, peak above it up to 20200 */
    hdpvr_rate_mode_t rate_mode;
    int gop;              /* 0-3: bit 0 simple GOP, bit 1 no IDR */
    int brightness, contrast, hue, saturation, sharpness; /* 0-255, -1 = firmware default */
    int audio_boost;
} hdpvr_options_t;

typedef struct {
    uint16_t vid, pid;
    int firmware;          /* version byte */
    char firmware_date[40];
    int ac3_capable;
} hdpvr_info_t;

typedef struct {
    int width, height, fps; /* all 0 when there is no signal */
} hdpvr_signal_t;

/* Called from the USB thread with each chunk of transport stream. */
typedef void (*hdpvr_data_cb)(const uint8_t *data, size_t len, void *user);

void hdpvr_default_options(hdpvr_options_t *o);
int hdpvr_open(hdpvr_t **out);
void hdpvr_close(hdpvr_t *dev);
const hdpvr_info_t *hdpvr_info(hdpvr_t *dev);
int hdpvr_configure(hdpvr_t *dev, const hdpvr_options_t *o);
int hdpvr_get_signal(hdpvr_t *dev, hdpvr_signal_t *s);
/* Waits up to timeout_ms for a picture, then starts the encoder. A stalled
 * encoder is restarted automatically while streaming. */
int hdpvr_start(hdpvr_t *dev, int timeout_ms, hdpvr_data_cb cb, void *user);
void hdpvr_stop(hdpvr_t *dev);
/* Bytes received since hdpvr_start, and how many times a stall was recovered. */
uint64_t hdpvr_bytes(hdpvr_t *dev);
int hdpvr_restarts(hdpvr_t *dev);

const char *hdpvr_strerror(int err);
void hdpvr_set_verbose(int level);

#endif
