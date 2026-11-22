# TV tuner helper protocol

HauppaugeRecorder drives analog TV tuners (the Hauppauge WinTV HVR-950Q and similar USB tuners) through a **helper
program**, one per platform. The helper owns the tuner, decodes the line 21 closed captions (CEA-608) from the raw VBI
lines, and encodes MPEG-2 video with the captions inside. The app (`src/tvdev.c`, used through `app_start_tv` and
`app_tv_command` in `app_core.h`) starts the helper as `<helper> --serve`, writes text commands to its stdin and reads
framed messages from its stdout. Anything else the helper wants to say goes to stderr.

| Platform | Helper | Capture | Status |
|---|---|---|---|
| Windows | `wintv-capture.exe` (`src/wintv.cpp`) | DirectShow, through Hauppauge's own WDM driver | works, tested on an HVR-950Q |
| Linux | `tv-capture-linux` (`src/tv_linux.cpp`) | V4L2 (`/dev/videoN`, `/dev/vbiN`) and ALSA, through the kernel's `au0828` driver | written, compiles, and its core runs; **not yet tried on a real tuner** |
| macOS | `tv-capture-macos` | a libusb user space driver for the Auvitek AU0828 bridge, AU8522 decoder and xc5000 tuner | not written yet |

The helpers share `src/tvcore.hpp` (options, recording, caption injection, this protocol, the command line) and
`src/tvplat.hpp` (time, files, processes, stdio for Windows and POSIX). A platform adds a `Backend` (`open`, `run`,
`stop`, `close`, `route`, `tune`) that hands pictures, audio and VBI lines to the `Engine`. Any helper also takes
`--device synthetic`: colour bars, a tone and a caption that changes every two seconds, through the real engine,
for trying the app or a new backend without a tuner.

`tv_helper_available()` is true when the file is next to the app, so a platform without a helper shows no TV option.
Helpers with the same protocol are interchangeable; the app does not know which capture API is behind it.

## Commands (app to helper)

One command per line, UTF-8, `key value`. The configuration commands can be sent at any time; send `start` once the
tuner is configured.

| Command | Meaning |
|---|---|
| `source tuner\|composite\|svideo` | input |
| `channel N` | TV channel (tuner input); applied live |
| `cable 0\|1` | cable or antenna; applied live |
| `pal 0\|1` | 625-line 25 fps instead of NTSC (restarts the capture) |
| `wide 0\|1` | flag recordings as 16:9 |
| `quality low\|medium\|high\|best` | 3, 5, 7, 9 Mbit/s |
| `bitrate MBPS` | overrides the quality; 0 = use the quality |
| `cbr 0\|1`, `audiokbps N`, `cc 0\|1` | constant bitrate, audio bitrate, captions on or off |
| `preview 0\|1` | send preview pictures |
| `monitor 0\|1` | send audio for monitoring |
| `ffmpeg PATH` | ffmpeg to use (default: next to the helper, `WINTV_FFMPEG`, then PATH) |
| `start` | open the tuner (retried every few seconds while it is missing or busy) |
| `retry` | try opening the tuner now |
| `record PATH` | start recording to PATH (`.mpg`, or `.ts` for a transport stream) |
| `stop` | finish the recording; the helper then sends `R` |
| `quit` | finish a recording in progress, exit |

## Messages (helper to app)

Header: 1 type byte, 3 zero bytes, `u32` little-endian payload length. Then the payload.

| Type | Payload |
|---|---|
| `S` status | text `state=N;w=720;h=480;fps=29.97;wide=0;msg=text`. `N`: 0 searching, 1 needs attention, 2 starting, 3 no signal, 4 streaming. `msg` runs to the end and may contain `;`. |
| `F` picture | `u32 w, u32 h`, then `w*h*4` bytes of BGRX (the latest picture only; the helper may skip pictures) |
| `L` levels | two `float32`: left and right peak in dBFS since the last message (about 20 per second) |
| `A` audio | 48 kHz 16-bit stereo PCM, only while `monitor 1` |
| `K` | the recording started |
| `P` progress | `f64 seconds, u64 bytes`, about 5 per second while recording |
| `R` result | text `ok=1;path=...;err=...;frames=N;cc1=N;cc2=N;cc=N`. Sent after `stop` (or when a recording fails). `cc1`/`cc2` count frames that carried a caption pair on field 1 and 2, `cc` the frames. A failed `record` sends `ok=0` instead of `K`. |

## What a helper must do

* **Captions.** Decode line 21 of both fields from the raw VBI data (`src/cc608.h` does the slicing from 8-bit samples
  at any sampling rate). Each recorded frame carries exactly one pair per field, in the order they arrived, because the
  caption text is a byte stream: a dropped or repeated pair garbles it. A frame with no data gets the null pair
  `0x80 0x80`. The pairs are written as A/53 `GA94` user data after each MPEG-2 picture header (`cc608_pack_a53`); with no
  B-frames the pictures are in display order.
* **Field order.** The VBI buffers arrive one per field in display order; the first of a frame is field 2 for bottom
  field first (what the HVR-950Q delivers), so tell them apart by time stamp gaps, not by counting.
* **Audio.** The HVR-950Q's USB audio input mixer is disabled by default and records silence until it is enabled.
* **Timing.** Keep a constant frame rate in the file by repeating a frame to fill a gap (the Windows helper does), and
  line the audio up with the first video frame.
* **Never block.** The helper's stdout writer keeps only the latest picture and drops audio when the app falls behind;
  status and results are never dropped.
