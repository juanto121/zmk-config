#pragma once

#include <zmk/event_manager.h>

enum focus_timer_phase {
    FOCUS_TIMER_IDLE = 0,
    FOCUS_TIMER_FOCUS,
    FOCUS_TIMER_BREAK,
};

// Raised when the timer starts/stops/changes phase and once per second while running.
struct focus_timer_state_changed {
    uint8_t phase;          // enum focus_timer_phase
    uint16_t remaining_s;   // seconds left in the current phase
};

ZMK_EVENT_DECLARE(focus_timer_state_changed);

// Start a focus session when idle, stop the timer otherwise.
void focus_timer_toggle(void);

struct focus_timer_state_changed focus_timer_get_state(void);
