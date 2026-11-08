/* USB IDs of the supported HD PVR models, shared by the driver and the Windows driver installer. */
#ifndef HDPVR_KNOWN_DEVICES_H
#define HDPVR_KNOWN_DEVICES_H

#include <stdint.h>

static const struct {
    uint16_t vid, pid;
    const char *name;
} known_devices[] = {
    { 0x2040, 0x4900, "Hauppauge HD PVR" },
    { 0x2040, 0x4901, "Hauppauge HD PVR" },
    { 0x2040, 0x4902, "Hauppauge HD PVR" },
    { 0x2040, 0x4903, "Hauppauge HD PVR" },
    { 0x2040, 0x4982, "Hauppauge HD PVR" },
};

#define KNOWN_DEVICE_COUNT (sizeof(known_devices) / sizeof(known_devices[0]))

#endif
