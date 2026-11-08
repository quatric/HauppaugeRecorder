# Hauppauge HD PVR (model 1212) USB protocol

The HD PVR has its own H.264 encoder (Ambarella), so the computer only configures it and reads an
MPEG-2 transport stream. These notes are from the Linux `hdpvr` driver (drivers/media/usb/hdpvr,
Janne Grunau, 2008), checked against the device descriptors. HauppaugeRecorder's code is written
from these notes and does not contain code from that driver.

## Device

| USB ID | Notes |
|---|---|
| `2040:4900`, `4901`, `4902`, `4903`, `4982` | HD PVR 1212 variants. `4903` reports manufacturer "AMBA" |

High speed (480 Mbit/s), one configuration. The transport stream comes from the first bulk IN
endpoint of interface 0. Its descriptor says 512-byte packets; the stream is read in 8 KB or larger
transfers.

## Control requests

Every setting is a vendor control request to the device (`bmRequestType` `0x38` out, `0xb8` in),
`wIndex` = `0x0003` unless noted.

| Request | `bRequest` | `wValue` | Data | Meaning |
|---|---|---|---|---|
| Status | `0x81` in | `0x0400` | 46 bytes | byte 1 = firmware version, bytes 2.. = build date (text), bytes 38–45 = challenge |
| Unlock | `0xd1` out, `wIndex` 0 | `0x0000` | 8 bytes | challenge answer, see below. Required before streaming |
| Filter | `0x01` out | `0x3100` | 4 bytes | luma and chroma low-pass strength, chroma MF enable. Default `03 03 00 00` |
| Fan and LEDs | `0xd4` out, `wIndex` 0 | `0` | 1 byte | `1` = on |
| Audio boost | `0xd5` out, `wIndex` 0 | `0` | 1 byte | `1` = boost analog audio |
| Video standard | `0x01` out | `0x1700` | 1 byte | `0` = 60 Hz, `1` = 50 Hz (SD inputs) |
| Video input | `0x01` out | `0x1500` | 1 byte | `1` = component, `2` = S-Video, `3` = composite |
| Audio input | `0x01` out | `0x2500` | 1 or 2 bytes | input `1` = RCA back, `2` = RCA front, `3` = S/PDIF; firmware `0x0d`+ takes a second byte: `0` = AAC, `1` = AC-3 |
| Bitrate | `0x01` out | `0x1000` | 4 bytes | LE16 average, LE16 peak, in 100 kbit/s. Average 1–13.5 Mbit/s, peak above average and up to 20.2 Mbit/s |
| Bitrate mode | `0x01` out | `0x1200` | 1 byte | `1` = constant, `2` = variable (peak), `3` = variable (average) |
| GOP mode | `0x01` out | `0x1300` | 1 byte | bit 0: simple GOP; bit 1: no IDR |
| Brightness, contrast, hue, saturation, sharpness | `0x01` out | `0x2900`, `0x2a00`, `0x2b00`, `0x2c00`, `0x2d00` | 1 byte | firmware above `0x15` defaults: `80 40 0f 40 80`; older: `86 80 80 80 80` |
| Video info | `0x81` in | `0x1400` | 5 bytes | LE16 width, LE16 height, frame rate. All zero = no signal |
| Input lines | `0x81` in | `0x1800` | 3 bytes | LE16 detected lines |
| Encoder start | `0xb8` out, `wIndex` 0 | `0x0001` | none | before Start |
| Start | `0x01` out | `0x0700` | 1 byte `0` | stream starts on the bulk endpoint |
| Stop | `0x01` out | `0x0800` | 1 byte `0` | then read the bulk endpoint until it times out (90 ms) to empty it |

## Unlock

The 8 challenge bytes go through 32 rounds; round `n` does, in order:

* if `n` is not a multiple of 4: `b[n/8 + 3] = b[(n/4) % 4]`
* then by `n % 4`:
  * 0: `b[3 - n/8] = b[n/4]`; `b[5] += b[6] * 3`; three times `b[3] *= b[3] + 1`
  * 1: `b[0] = b[0] * 8 + 7n + 4`; `b[6] += b[3] * 3`
  * 2: three times `b[1] *= b[6] + 1`; three times, with `x` = the 8 bytes as a little-endian 64-bit
    number, `x += x << (b[7] & 15)`
  * 3: `b[2] += b[3] * 4 + b[4] + b[5]`; `b[4] += b[(n % 2) * 2] * 9 + 9`

All byte arithmetic wraps at 8 bits. The result is sent with the Unlock request.

## Streaming

1. Wait until Video info reports a picture (poll every 250 ms).
2. Wait until at least 3 s have passed since the last setting was sent. Tested on firmware 0x1e: a
   start sent less than about 2 s after configuring is accepted but never produces any data.
3. Encoder start, then Start. The first data can take a few seconds to arrive.
4. Read the bulk endpoint. Once data has been flowing, a gap of more than 1.5 s means the encoder
   has stalled: Stop, empty the endpoint, wait 4 s (the firmware needs it), and start again.
5. Stop, then empty the endpoint.
