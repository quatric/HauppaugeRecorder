/*
 * North American analog TV channel plan: picture carrier frequency of a channel.
 * Backends that tune by frequency (V4L2) use it; DirectShow has its own table in the driver.
 */
#ifndef TVCHAN_HPP
#define TVCHAN_HPP

/* Picture carrier in MHz for channel `ch`, or 0 if there is no such channel.
 * Antenna: 2-13 VHF, 14-83 UHF. Cable (standard plan): 1-13 as broadcast, 14-22 (A to I), 23-94, 95-99, 100-135. */
static inline double tv_channel_mhz(int ch, bool cable)
{
    if (ch < 1 || ch > 135) return 0;
    if (ch >= 2 && ch <= 4) return 55.25 + 6.0 * (ch - 2);
    if (ch == 5 || ch == 6) return 77.25 + 6.0 * (ch - 5);
    if (ch >= 7 && ch <= 13) return 175.25 + 6.0 * (ch - 7);
    if (!cable) return ch >= 14 && ch <= 83 ? 471.25 + 6.0 * (ch - 14) : 0;
    if (ch == 1) return 73.25;
    if (ch >= 14 && ch <= 22) return 121.25 + 6.0 * (ch - 14);
    if (ch >= 23 && ch <= 94) return 217.25 + 6.0 * (ch - 23);
    if (ch >= 95 && ch <= 99) return 93.25 + 6.0 * (ch - 95);
    return 649.25 + 6.0 * (ch - 100);
}

#endif
