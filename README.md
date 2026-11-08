# HauppaugeRecorder

Userspace capture driver and recorder for the **Hauppauge HD PVR** (model 1212, USB `2040:4900`,
`4901`, `4902`, `4903`, `4982`) on macOS, Windows and Linux. It needs no kernel extension or vendor
driver: it unlocks and configures the box through libusb and records the H.264 transport stream the
box's own encoder produces.

Status: the command line recorder `hdpvr-capture` is written and builds for every platform. It has
not been tested on a device yet.

## Use

```bash
hdpvr-capture --info                          # firmware and current input signal
hdpvr-capture -o show.ts                      # record from component + back RCA audio until Ctrl-C
hdpvr-capture -i composite -a front -o tape.ts -t 3600
hdpvr-capture -o - | ffplay -                 # watch live
```

| Option | |
|---|---|
| `-i component\|svideo\|composite` | video input (default component) |
| `-a back\|front\|spdif` | audio input (default back RCA) |
| `--ac3` | AC-3 audio instead of AAC (firmware 0x0d and later) |
| `--pal` | 50 Hz on S-Video and composite |
| `-b MBPS`, `-p MBPS`, `--vbr` | average bitrate 1–13.5 (default 6.5), peak up to 20.2, variable bitrate |
| `--brightness` … `--sharpness 0..255` | picture controls |
| `--boost` | boost the analog audio |
| `-t SECONDS` | stop after this long |
| `-v` | more logging; `-vv` traces every USB request |

The output is a standard MPEG transport stream (H.264 video, AAC or AC-3 audio) that VLC, ffmpeg
and most editors open directly. If the encoder stops sending (the firmware does this now and then),
it is restarted after four seconds, as the Windows driver does.

## Platforms

* **macOS**: nothing to install. Universal 2 (Intel and Apple silicon), macOS 10.11 or later.
* **Linux**: the kernel's `hdpvr` driver is detached automatically while recording. Allow access
  with a udev rule: `SUBSYSTEM=="usb", ATTR{idVendor}=="2040", MODE="0666"`.
* **Windows**: install WinUSB for the HD PVR with [Zadig](https://zadig.akeo.ie) (or have UsbDk
  installed).

## Build

```bash
make                     # macOS: build/hdpvr-capture
scripts/build-all.sh     # Windows and Linux, with Zig: dist/<target>/
```

## How it works

See [docs/PROTOCOL.md](docs/PROTOCOL.md). The protocol was documented from the Linux `hdpvr` driver;
the code here is a separate implementation. libusb 1.0.30 is vendored in `vendor/libusb`
(LGPL-2.1).
