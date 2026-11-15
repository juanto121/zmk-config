/*
 * Focus (Pomodoro) timer.
 *
 *   FOCUS (25 min) --completed--> BREAK (5 min, or 30 min after every 4th
 *   completed session of the day) --> idle
 *
 * Keys (zmk,behavior-focus-timer instances in the keymap):
 *   &focus      start a focus session when idle, stop the timer otherwise
 *   &focus_brk  (start-break) go straight to a short break; does not count a session
 *
 * Only focus sessions that run their full length count. The number of sessions
 * completed today is kept in flash (Zephyr settings, key "nvc/focus") so it
 * survives power loss. The dongle has no clock: the Mac sender reports local
 * time over Raw HID (focus_timer_set_local_time), which is how "today" is known.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_focus_timer

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <nice_view_claude/focus_timer.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(focus_timer_state_changed);

#define MINUTES_MS(m) ((int64_t)(m) * 60 * MSEC_PER_SEC)
#define FOCUS_MS MINUTES_MS(CONFIG_NICE_VIEW_CLAUDE_FOCUS_MINUTES)
#define BREAK_MS MINUTES_MS(CONFIG_NICE_VIEW_CLAUDE_BREAK_MINUTES)
#define LONG_BREAK_MS MINUTES_MS(CONFIG_NICE_VIEW_CLAUDE_LONG_BREAK_MINUTES)
#define SECONDS_PER_DAY 86400

#define SETTINGS_KEY "nvc/focus"

// Persisted record. `day` is days since 1970-01-01 in local time; 0 = unknown.
struct focus_record {
    uint32_t day;
    uint16_t count;
} __packed;

static struct k_spinlock lock;
static enum focus_timer_phase phase = FOCUS_TIMER_IDLE;
static int64_t phase_end_ms;
static bool long_break;
static uint32_t stored_day;      // day the count belongs to
static uint16_t today_count;     // completed sessions on stored_day
static uint16_t pending_count;   // completed before the date was known since boot
static bool clock_valid;
static int64_t clock_offset_s;   // local epoch seconds minus uptime seconds

static void tick_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_handler);

// --- persistence ---------------------------------------------------------------

static void save_handler(struct k_work *work) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    struct focus_record record = {.day = stored_day, .count = today_count};
    k_spin_unlock(&lock, key);

    int rc = settings_save_one(SETTINGS_KEY, &record, sizeof(record));
    if (rc < 0) {
        LOG_ERR("focus timer: saving the session count failed (%d)", rc);
    }
}
static K_WORK_DEFINE(save_work, save_handler);

static void publish(void);
static void publish_handler(struct k_work *work) { publish(); }
static K_WORK_DEFINE(publish_work, publish_handler);

static int focus_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    if (!settings_name_steq(name, "focus", NULL)) {
        return -ENOENT;
    }
    struct focus_record record;
    if (len != sizeof(record)) {
        return -EINVAL;
    }
    int rc = read_cb(cb_arg, &record, sizeof(record));
    if (rc < 0) {
        return rc;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    stored_day = record.day;
    today_count = record.count;
    k_spin_unlock(&lock, key);

    // the display may have drawn before settings were loaded
    k_work_submit(&publish_work);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nvc_focus, "nvc", NULL, focus_settings_set, NULL, NULL);

// --- day tracking (call with the lock held) -----------------------------------------

static uint32_t current_day_locked(void) {
    if (!clock_valid) {
        return 0;
    }
    int64_t local_s = clock_offset_s + k_uptime_get() / MSEC_PER_SEC;
    return local_s > 0 ? (uint32_t)(local_s / SECONDS_PER_DAY) : 0;
}

// Moves the count to today if the date changed. Returns true if anything changed.
static bool roll_day_locked(void) {
    bool changed = false;
    uint32_t day = current_day_locked();
    if (day != 0 && day != stored_day) {
        // a stored count of an unknown day (first use) is kept as today's
        if (stored_day != 0) {
            today_count = 0;
        }
        stored_day = day;
        changed = true;
    }
    if (day != 0 && pending_count > 0) {
        today_count += pending_count;
        pending_count = 0;
        changed = true;
    }
    return changed;
}

// --- state -------------------------------------------------------------------------------

static struct focus_timer_state_changed state_locked(void) {
    struct focus_timer_state_changed state = {
        .phase = phase,
        .remaining_s = 0,
        .sessions_today = today_count + pending_count,
        .long_break = phase == FOCUS_TIMER_BREAK && long_break,
    };
    if (phase != FOCUS_TIMER_IDLE) {
        int64_t left_ms = phase_end_ms - k_uptime_get();
        if (left_ms < 0) {
            left_ms = 0;
        }
        // round up so a fresh 25:00 session shows 25:00, not 24:59
        state.remaining_s = (uint16_t)((left_ms + MSEC_PER_SEC - 1) / MSEC_PER_SEC);
    }
    return state;
}

struct focus_timer_state_changed focus_timer_get_state(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    struct focus_timer_state_changed state = state_locked();
    k_spin_unlock(&lock, key);
    return state;
}

static void enter_phase_locked(enum focus_timer_phase next, bool is_long) {
    phase = next;
    long_break = is_long;
    int64_t now = k_uptime_get();
    if (next == FOCUS_TIMER_FOCUS) {
        phase_end_ms = now + FOCUS_MS;
    } else if (next == FOCUS_TIMER_BREAK) {
        phase_end_ms = now + (is_long ? LONG_BREAK_MS : BREAK_MS);
    }
}

// Raise the state event and schedule the next tick (or stop ticking when idle).
static void publish(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    struct focus_timer_state_changed state = state_locked();
    int64_t left_ms = phase_end_ms - k_uptime_get();
    k_spin_unlock(&lock, key);

    raise_focus_timer_state_changed(state);

    if (state.phase == FOCUS_TIMER_IDLE) {
        k_work_cancel_delayable(&tick_work);
        return;
    }
    // wake on the next whole-second boundary of the countdown
    int64_t delay_ms = left_ms % MSEC_PER_SEC;
    if (delay_ms <= 0) {
        delay_ms = MSEC_PER_SEC;
    }
    k_work_reschedule(&tick_work, K_MSEC(delay_ms));
}

static void tick_handler(struct k_work *work) {
    bool save = false;

    k_spinlock_key_t key = k_spin_lock(&lock);
    if (phase != FOCUS_TIMER_IDLE && k_uptime_get() >= phase_end_ms) {
        if (phase == FOCUS_TIMER_FOCUS) {
            // a full session: count it, then break (long after every Nth today)
            roll_day_locked();
            if (clock_valid) {
                today_count++;
            } else {
                pending_count++;
            }
            uint16_t total = today_count + pending_count;
            bool is_long = total % CONFIG_NICE_VIEW_CLAUDE_LONG_BREAK_EVERY == 0;
            enter_phase_locked(FOCUS_TIMER_BREAK, is_long);
            save = clock_valid;
            LOG_INF("focus session %u done, %s break", total, is_long ? "long" : "short");
        } else {
            enter_phase_locked(FOCUS_TIMER_IDLE, false);
        }
    }
    k_spin_unlock(&lock, key);

    if (save) {
        k_work_submit(&save_work);
    }
    publish();
}

void focus_timer_toggle(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    enter_phase_locked(phase == FOCUS_TIMER_IDLE ? FOCUS_TIMER_FOCUS : FOCUS_TIMER_IDLE, false);
    k_spin_unlock(&lock, key);
    publish();
}

void focus_timer_start_break(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    enter_phase_locked(FOCUS_TIMER_BREAK, false);
    k_spin_unlock(&lock, key);
    publish();
}

void focus_timer_set_local_time(uint32_t local_epoch_s) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    clock_offset_s = (int64_t)local_epoch_s - k_uptime_get() / MSEC_PER_SEC;
    clock_valid = true;
    bool changed = roll_day_locked();
    k_spin_unlock(&lock, key);

    if (changed) {
        k_work_submit(&save_work);
        k_work_submit(&publish_work);
    }
}

// --- behavior ----------------------------------------------------------------------------

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_focus_timer_config {
    bool start_break;
};

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_focus_timer_config *config = dev->config;

    if (config->start_break) {
        focus_timer_start_break();
    } else {
        focus_timer_toggle();
    }
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

#define FOCUS_TIMER_INST(n)                                                                        \
    static const struct behavior_focus_timer_config behavior_focus_timer_config_##n = {            \
        .start_break = DT_INST_PROP(n, start_break),                                               \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &behavior_focus_timer_config_##n, POST_KERNEL,   \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_focus_timer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(FOCUS_TIMER_INST)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
