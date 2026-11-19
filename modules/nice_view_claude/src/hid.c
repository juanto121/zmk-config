#include <nice_view_claude/hid.h>
#include <nice_view_claude/focus_timer.h>
#include <raw_hid/events.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(is_connected_notification);
ZMK_EVENT_IMPL(volume_notification);
ZMK_EVENT_IMPL(claude_usage_notification);

typedef enum {
    _VOLUME = 0xAB, // matches qmk-hid-host
    _CLAUDE_USAGE = 0xC0,
    _LOCAL_TIME = 0xC1, // [1..4] local epoch seconds, little endian
} hid_data_type;

static bool is_connected = false;

// k_timer expiry functions run in interrupt context, where raising ZMK events is not
// allowed (the display widget listeners take a k_mutex). The timers only queue work;
// the events are raised from the system work queue.

static void disconnect_work_handler(struct k_work *work) {
    LOG_INF("raise_connection_notification: false");
    is_connected = false;
    raise_is_connected_notification((struct is_connected_notification){.value = false});
}

static K_WORK_DEFINE(disconnect_work, disconnect_work_handler);

static void on_disconnect_timer(struct k_timer *dummy) { k_work_submit(&disconnect_work); }

K_TIMER_DEFINE(disconnect_timer, on_disconnect_timer, NULL);

static uint8_t last_hid_volume = 0;
static uint8_t last_raised_volume = 0;

static void raise_volume_if_changed(void) {
    // prevent raising event with the same value multiple times
    if (last_raised_volume != last_hid_volume) {
        last_raised_volume = last_hid_volume;
        LOG_INF("raise_volume_notification %i", last_hid_volume);
        raise_volume_notification((struct volume_notification){.value = last_hid_volume});
    }
}

static void volume_work_handler(struct k_work *work) { raise_volume_if_changed(); }

static K_WORK_DEFINE(volume_work, volume_work_handler);

static void on_volume_timer(struct k_timer *dummy) { k_work_submit(&volume_work); }

K_TIMER_DEFINE(volume_timer, on_volume_timer, NULL);

static uint8_t sanitize_percent(uint8_t value) {
    return value <= 100 ? value : CLAUDE_USAGE_UNKNOWN;
}

static void process_raw_hid_data(uint8_t *data) {
    uint8_t data_type = data[0];
    if (data_type != _VOLUME && data_type != _CLAUDE_USAGE && data_type != _LOCAL_TIME) {
        // ignore other qmk-hid-host messages (time, layout, media, ...)
        return;
    }

    LOG_INF("display_process_raw_hid_data - received data_type %u", data_type);

    // raise disconnect notification after 65 seconds of inactivity
    k_timer_start(&disconnect_timer, K_SECONDS(65), K_NO_WAIT);
    if (!is_connected) {
        LOG_INF("raise_connection_notification: true");
        is_connected = true;
        raise_is_connected_notification((struct is_connected_notification){.value = true});
    }

    switch (data_type) {
    case _VOLUME:
        last_hid_volume = data[1];

        // debounce volume change events
        if (k_timer_status_get(&volume_timer) > 0 || k_timer_remaining_get(&volume_timer) == 0) {
            k_timer_start(&volume_timer, K_MSEC(150), K_NO_WAIT);
            raise_volume_if_changed();
        }
        break;

    case _CLAUDE_USAGE:
        raise_claude_usage_notification((struct claude_usage_notification){
            .five_hour = sanitize_percent(data[1]),
            .seven_day = sanitize_percent(data[2]),
        });
        break;

    case _LOCAL_TIME:
        focus_timer_set_local_time((uint32_t)data[1] | (uint32_t)data[2] << 8 |
                                   (uint32_t)data[3] << 16 | (uint32_t)data[4] << 24);
        break;
    }
}

static int raw_hid_received_event_listener(const zmk_event_t *eh) {
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event) {
        process_raw_hid_data(event->data);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(process_raw_hid_event, raw_hid_received_event_listener);
ZMK_SUBSCRIPTION(process_raw_hid_event, raw_hid_received_event);
