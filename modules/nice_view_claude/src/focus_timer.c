/*
 * Focus (Pomodoro) timer: FOCUS for CONFIG_NICE_VIEW_CLAUDE_FOCUS_MINUTES, then
 * BREAK for CONFIG_NICE_VIEW_CLAUDE_BREAK_MINUTES, then idle. Toggled by the
 * &focus_timer behavior (zmk,behavior-focus-timer) bound in the keymap.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_focus_timer

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <nice_view_claude/focus_timer.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(focus_timer_state_changed);

#define FOCUS_MS ((int64_t)CONFIG_NICE_VIEW_CLAUDE_FOCUS_MINUTES * 60 * MSEC_PER_SEC)
#define BREAK_MS ((int64_t)CONFIG_NICE_VIEW_CLAUDE_BREAK_MINUTES * 60 * MSEC_PER_SEC)

static enum focus_timer_phase phase = FOCUS_TIMER_IDLE;
static int64_t phase_end_ms;

static void tick_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_handler);

struct focus_timer_state_changed focus_timer_get_state(void) {
    if (phase == FOCUS_TIMER_IDLE) {
        return (struct focus_timer_state_changed){.phase = FOCUS_TIMER_IDLE, .remaining_s = 0};
    }
    int64_t left_ms = phase_end_ms - k_uptime_get();
    if (left_ms < 0) {
        left_ms = 0;
    }
    // round up so a fresh 25:00 session shows 25:00, not 24:59
    return (struct focus_timer_state_changed){
        .phase = phase,
        .remaining_s = (uint16_t)((left_ms + MSEC_PER_SEC - 1) / MSEC_PER_SEC),
    };
}

static void enter_phase(enum focus_timer_phase next) {
    phase = next;
    if (next == FOCUS_TIMER_FOCUS) {
        phase_end_ms = k_uptime_get() + FOCUS_MS;
    } else if (next == FOCUS_TIMER_BREAK) {
        phase_end_ms = k_uptime_get() + BREAK_MS;
    }
    LOG_INF("focus timer phase %d", next);
}

static void publish_and_reschedule(void) {
    raise_focus_timer_state_changed(focus_timer_get_state());

    if (phase == FOCUS_TIMER_IDLE) {
        k_work_cancel_delayable(&tick_work);
        return;
    }
    // wake on the next whole-second boundary of the countdown
    int64_t left_ms = phase_end_ms - k_uptime_get();
    int64_t delay_ms = left_ms % MSEC_PER_SEC;
    if (delay_ms <= 0) {
        delay_ms = MSEC_PER_SEC;
    }
    k_work_reschedule(&tick_work, K_MSEC(delay_ms));
}

static void tick_handler(struct k_work *work) {
    if (phase != FOCUS_TIMER_IDLE && k_uptime_get() >= phase_end_ms) {
        enter_phase(phase == FOCUS_TIMER_FOCUS ? FOCUS_TIMER_BREAK : FOCUS_TIMER_IDLE);
    }
    publish_and_reschedule();
}

void focus_timer_toggle(void) {
    enter_phase(phase == FOCUS_TIMER_IDLE ? FOCUS_TIMER_FOCUS : FOCUS_TIMER_IDLE);
    publish_and_reschedule();
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    focus_timer_toggle();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_focus_timer_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_focus_timer_driver_api);

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
