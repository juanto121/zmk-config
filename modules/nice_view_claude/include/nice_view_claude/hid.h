#pragma once

#include <zmk/event_manager.h>

/*
 * Raw HID report layout (32 bytes, first byte is the data type):
 *   0xAB _VOLUME        [1] = volume 0..100 (same value qmk-hid-host uses)
 *   0xC0 _CLAUDE_USAGE  [1] = 5-hour window used %, [2] = 7-day window used %
 *                       0..100, or CLAUDE_USAGE_UNKNOWN when not available.
 *   0xC1 _LOCAL_TIME    [1..4] local time, seconds since 1970-01-01, little endian
 */
#define CLAUDE_USAGE_UNKNOWN 0xFF

struct is_connected_notification {
    bool value;
};

ZMK_EVENT_DECLARE(is_connected_notification);

struct volume_notification {
    uint8_t value;
};

ZMK_EVENT_DECLARE(volume_notification);

struct claude_usage_notification {
    uint8_t five_hour;
    uint8_t seven_day;
};

ZMK_EVENT_DECLARE(claude_usage_notification);
