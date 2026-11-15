#pragma once

#include <zmk/event_manager.h>

enum focus_timer_phase {
    FOCUS_TIMER_IDLE = 0,
    FOCUS_TIMER_FOCUS,
    FOCUS_TIMER_BREAK,
};

// Raised when the timer starts/stops/changes phase, when the day's count changes,
// and once per second while running.
struct focus_timer_state_changed {
    uint8_t phase;            // enum focus_timer_phase
    bool long_break;          // current break is the long one
    uint16_t remaining_s;     // seconds left in the current phase
    uint16_t sessions_today;  // completed focus sessions today
};

ZMK_EVENT_DECLARE(focus_timer_state_changed);

// Start a focus session when idle, stop the timer otherwise.
void focus_timer_toggle(void);

// Go straight to a short break (from focus or idle). Does not count a session.
void focus_timer_start_break(void);

// Local wall-clock time from the host (seconds since 1970-01-01, local time zone).
void focus_timer_set_local_time(uint32_t local_epoch_s);

struct focus_timer_state_changed focus_timer_get_state(void);
