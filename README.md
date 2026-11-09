# HauppaugeRecorder

Userspace capture driver and recorder for the **Hauppauge HD PVR** (model 1212, USB `2040:4900`,
`4901`, `4902`, `4903`, `4982`) on macOS, Windows and Linux. It needs no kernel extension or vendor
driver: it unlocks and configures the box through libusb and records the H.264 transport stream the
box's own encoder produces.

Status: the command line recorder `hdpvr-capture` builds for every platform. It has been tested on
macOS 10.15 (Intel) with an HD PVR 1212 on firmware 0x1e, recording 480i and 480p from the
component input and 480i from composite, with AAC audio and no dropped packets. S-Video, 720p and
1080i, AC-3 audio, and the Windows and Linux builds have not been tested on a device yet.

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

## HauppaugeRecorder (the app)

macOS and Windows also get **HauppaugeRecorder**, a window with a live preview, a level meter, a
Record button with a timer, a snapshot button and a choice of recording folder. It shows the
picture and plays the sound through a small bundled ffmpeg (decoding only), and records the same
transport stream as `hdpvr-capture`. On macOS 10.9 to 10.14 the sound goes straight to Core Audio's
default output; from 10.15 it uses AVAudioEngine.

## Platforms

* **macOS**: nothing to install. Universal 2 (Intel and Apple silicon), macOS 10.9 or later.
* **Older Macs**: `HauppaugeRecorder-macos-legacy-ppc-i386-app.zip` holds **HauppaugeRecorder Legacy**
  for PowerPC on Mac OS X 10.3.9 or later and Intel on 10.4 or later (OpenGL preview, the old Core Audio
  output). Recording only copies the HD PVR's stream, so it is light on any Mac, but decoding the
  preview is not: a PowerPC Mac shows an HD source at 640x360 with the deblocking filter off, and needs a
  fast G4 or a G5 even so. Not tested on real old hardware yet; reports are welcome.
* **Linux**: the kernel's `hdpvr` driver is detached automatically while recording. Allow access
  with a udev rule: `SUBSYSTEM=="usb", ATTR{idVendor}=="2040", MODE="0666"`.
* **Windows**: 7 and later, on x86_64, x86 and Arm. The setup program installs `hdpvr-capture` and
  a driver for the HD PVR, the way [Zadig](https://zadig.akeo.ie) does. With the zip instead, run
  `hdpvr-driver.exe` once (it asks for administrator rights). It uses WinUSB, or the signed libusbK
  driver on a Windows that has no WinUSB setup file. `hdpvr-driver uninstall` gives the HD PVR back
  to its previous driver, such as Hauppauge's own, and `hdpvr-driver status` shows what is in use.

  To install the driver, `hdpvr-driver` writes an INF for the HD PVR, builds a catalog for it and
  signs that catalog with a certificate it creates on your PC. The certificate is trusted for code
  signing on that PC only, and its private key is deleted right after signing, so nothing else can
  be signed with it. The certificate is removed again on uninstall.

## Build

```bash
make                     # macOS: build/hdpvr-capture and "build/HauppaugeRecorder.app"
make test SAMPLE=recording.ts   # engine test without a device
scripts/build-all.sh     # Windows and Linux, with Zig: dist/<target>/
```

`scripts/build-legacy-mac.sh` builds the PowerPC and i386 app for old Macs in the `powerpc-apple-darwin8`
cross toolchain (`ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u`, with clang,
nasm and pkg-config added), as the release workflow does.

Zig links the Universal CRT, which a stock Windows 7 lacks. For Windows 7 builds, point
`WIN_CC_x86_64`, `WIN_CC_x86` (and optionally `WIN_CC_aarch64`) at an msvcrt MinGW-w64 compiler such
as [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)'s msvcrt release, and set `LIBUSBK_DIR` to
the folder `scripts/fetch-libusbk.sh` prints to include the libusbK fallback driver. Pushing a `v*`
tag runs `.github/workflows/release.yml`, which does all of this and publishes the macOS tarballs
(including the legacy one), the Linux tarballs, and the Windows zips and setup programs.

## How it works

See [docs/PROTOCOL.md](docs/PROTOCOL.md). The protocol was documented from the Linux `hdpvr` driver;
the code here is a separate implementation. libusb 1.0.30 is vendored in `vendor/libusb`
(LGPL-2.1).
