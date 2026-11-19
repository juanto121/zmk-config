/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include "eye.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <nice_view_claude/hid.h>
#include <nice_view_claude/focus_timer.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

// Creation (and so draw) order: later canvases cover earlier ones where they overlap.
enum widget_children {
    WIDGET_TOP = 0,
    WIDGET_MIDDLE,
    WIDGET_HID,
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

// Flaming eye shown in the middle section while the focus timer is idle.
// Only the first EYE_ROWS rows of the middle canvas are on screen.
#define EYE_ROWS 64
BUILD_ASSERT(sizeof(lv_color_t) == 1, "eye rendering assumes 1-byte (1-bit) colours");
static struct eye_state eye;

// Rotates a finished canvas into place: 90 degrees, plus another 180 when the
// screen is mounted upside down. A square canvas with 1-byte pixels turns 180
// degrees by reversing its buffer, which keeps the 90-degree pixel alignment.
static void finish_canvas(lv_obj_t *canvas, lv_color_t cbuf[]) {
    rotate_canvas(canvas, cbuf);
#if IS_ENABLED(CONFIG_NICE_VIEW_CLAUDE_UPSIDE_DOWN)
    for (int i = 0, j = CANVAS_SIZE * CANVAS_SIZE - 1; i < j; i++, j--) {
        lv_color_t tmp = cbuf[i];
        cbuf[i] = cbuf[j];
        cbuf[j] = tmp;
    }
    lv_obj_invalidate(canvas);
#endif
}

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, WIDGET_TOP);

    lv_draw_label_dsc_t label_dsc;
#ifdef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_LEFT);
#else
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_RIGHT);
#endif
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Keyboard halves' battery, two 12px rows on the left: "L 82%" / "R 79%".
    // Vertically this canvas sits at the bottom of the screen and the HID canvas
    // covers its first 40 rows, so only the last 28 are visible.
#ifdef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    const lv_coord_t battery_y = 0;
#else
    const lv_coord_t battery_y = 40;
#endif
    lv_draw_label_dsc_t label_battery;
    init_label_dsc(&label_battery, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_LEFT);
    char battery_text[8];
    if (state->battery_left == 0) {
        strcpy(battery_text, "L --");
    } else {
        snprintf(battery_text, sizeof(battery_text), "L %u%%", state->battery_left);
    }
    lv_canvas_draw_text(canvas, 0, battery_y, 44, &label_battery, battery_text);
    if (state->battery_right == 0) {
        strcpy(battery_text, "R --");
    } else {
        snprintf(battery_text, sizeof(battery_text), "R %u%%", state->battery_right);
    }
    lv_canvas_draw_text(canvas, 0, battery_y + 14, 44, &label_battery, battery_text);

    // Draw output status
    char output_text[10] = {};

    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            if (state->active_profile_connected) {
                strcat(output_text, LV_SYMBOL_WIFI);
            } else {
                strcat(output_text, LV_SYMBOL_CLOSE);
            }
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }
        break;
    }

#ifdef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    lv_canvas_draw_text(canvas, 0, 50, CANVAS_SIZE, &label_dsc, output_text);
#else
    lv_canvas_draw_text(canvas, 0, battery_y, CANVAS_SIZE, &label_dsc, output_text);

    // Rotate canvas
    finish_canvas(canvas, cbuf);
#endif
}

// One usage row: small window label on the left ("5h" / "7d"), percentage right-aligned.
// Widths (Montserrat): "7d" @12px ~16px, "100%" @18px ~46px, so both fit in 68px.
static void draw_usage_row(lv_obj_t *canvas, lv_coord_t y, const char *name, uint8_t percent) {
    lv_draw_label_dsc_t label_name;
    init_label_dsc(&label_name, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t label_value;
    init_label_dsc(&label_value, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_RIGHT);

    char value[8] = {};
    if (percent == CLAUDE_USAGE_UNKNOWN) {
        strcpy(value, "--%");
    } else {
        snprintf(value, sizeof(value), "%u%%", percent);
    }

    // 12px text sits 4px lower so its baseline lines up with the 18px value
    lv_canvas_draw_text(canvas, 0, y + 4, 20, &label_name, name);
    lv_canvas_draw_text(canvas, 18, y, CANVAS_SIZE - 18, &label_value, value);
}

static void draw_hid(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, WIDGET_HID);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_volume;
    init_label_dsc(&label_volume, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    if (state->is_connected) {
        char volume[10] = {};
        if (state->volume == 100) {
            sprintf(volume, LV_SYMBOL_VOLUME_MAX " %i%%", state->volume);
        } else if (state->volume == 0) {
            sprintf(volume, LV_SYMBOL_MUTE " %i%%", state->volume);
        } else {
            sprintf(volume, LV_SYMBOL_VOLUME_MID " %i%%", state->volume);
        }
        // Claude usage first, volume underneath
        draw_usage_row(canvas, 0, "5h", state->usage_five_hour);
        draw_usage_row(canvas, 23, "7d", state->usage_seven_day);
        lv_canvas_draw_text(canvas, 0, 47, CANVAS_SIZE, &label_volume, volume);
    } else {
        lv_draw_label_dsc_t label_big;
        init_label_dsc(&label_big, LVGL_FOREGROUND, &lv_font_montserrat_22, LV_TEXT_ALIGN_CENTER);
        lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_volume, "HID");
        lv_canvas_draw_text(canvas, 0, 23, CANVAS_SIZE, &label_big, "not");
        lv_canvas_draw_text(canvas, 0, 50, CANVAS_SIZE, &label_volume, "found");
    }


#ifndef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    // Rotate canvas
    finish_canvas(canvas, cbuf);
#endif
}

static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, WIDGET_MIDDLE);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    lv_draw_label_dsc_t label_small;
    init_label_dsc(&label_small, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_CENTER);
    char sessions[12];
    snprintf(sessions, sizeof(sessions), "Today %u", state->focus_sessions_today);

    if (state->focus_phase != FOCUS_TIMER_IDLE) {
        // Focus timer replaces the eye while running:
        //   FOCUS / BREAK / LONG BRK (12px), mm:ss (18px), Today N (12px)
        const char *phase_name = state->focus_phase == FOCUS_TIMER_FOCUS ? "FOCUS"
                                 : state->focus_long_break                ? "LONG BRK"
                                                                          : "BREAK";
        lv_canvas_draw_text(canvas, 0, 6, CANVAS_SIZE, &label_small, phase_name);

        char remaining[8];
        snprintf(remaining, sizeof(remaining), "%u:%02u", state->focus_remaining_s / 60,
                 state->focus_remaining_s % 60);
        lv_canvas_draw_text(canvas, 0, 20, CANVAS_SIZE, &label_dsc, remaining);

        lv_canvas_draw_text(canvas, 0, 46, CANVAS_SIZE, &label_small, sessions);
    } else {
        // Render the eye straight into the canvas buffer (1 byte per pixel),
        // then map 0/1 to the background/foreground colours in place.
        uint8_t *px = (uint8_t *)cbuf;
        eye_render(&eye, px, CANVAS_SIZE, EYE_ROWS, CANVAS_SIZE);
        const lv_color_t fg = LVGL_FOREGROUND;
        const lv_color_t bg = LVGL_BACKGROUND;
        for (int i = 0; i < EYE_ROWS * CANVAS_SIZE; i++) {
            cbuf[i] = px[i] ? fg : bg;
        }

        // today's completed sessions under the eye (below its lowest flames)
        if (state->focus_sessions_today > 0) {
            lv_canvas_draw_rect(canvas, 0, 51, CANVAS_SIZE, EYE_ROWS - 51, &rect_black_dsc);
            lv_canvas_draw_text(canvas, 0, 51, CANVAS_SIZE, &label_small, sessions);
        }
    }

#ifndef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    // Rotate canvas
    finish_canvas(canvas, cbuf);
#endif
}

// Advances the eye animation; runs on the display work queue like the other redraws.
static void eye_tick(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(eye_work, eye_tick);

static void eye_tick(struct k_work *work) {
    eye_step(&eye);
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (widget->state.focus_phase == FOCUS_TIMER_IDLE) {
            draw_middle(widget->obj, widget->cbuf2, &widget->state);
        }
    }
    k_work_schedule_for_queue(zmk_display_work_q(), &eye_work, K_MSEC(1000 / EYE_FPS));
}

// Keyboard halves' battery levels (the dongle's own battery is not shown).
struct peripheral_battery_state {
    uint8_t left;
    uint8_t right;
};

static void peripheral_battery_update_cb(struct peripheral_battery_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.battery_left = state.left;
        widget->state.battery_right = state.right;
        draw_top(widget->obj, widget->cbuf, &widget->state);
    }
}

static struct peripheral_battery_state peripheral_battery_get_state(const zmk_event_t *_eh) {
    // Read both levels on every event so the pair is always consistent.
    uint8_t levels[2] = {0, 0};
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    for (uint8_t source = 0; source < ARRAY_SIZE(levels); source++) {
        if (zmk_split_central_get_peripheral_battery_level(source, &levels[source]) != 0) {
            levels[source] = 0;
        }
    }
#endif
#if IS_ENABLED(CONFIG_NICE_VIEW_CLAUDE_SWAP_PERIPHERALS)
    return (struct peripheral_battery_state){.left = levels[1], .right = levels[0]};
#else
    return (struct peripheral_battery_state){.left = levels[0], .right = levels[1]};
#endif
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_battery, struct peripheral_battery_state,
                            peripheral_battery_update_cb, peripheral_battery_get_state)
ZMK_SUBSCRIPTION(widget_peripheral_battery, zmk_peripheral_battery_state_changed);

static void focus_timer_update_cb(struct focus_timer_state_changed timer) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.focus_phase = timer.phase;
        widget->state.focus_remaining_s = timer.remaining_s;
        widget->state.focus_long_break = timer.long_break;
        widget->state.focus_sessions_today = timer.sessions_today;
        draw_middle(widget->obj, widget->cbuf2, &widget->state);
    }
}

static struct focus_timer_state_changed focus_timer_widget_get_state(const zmk_event_t *eh) {
    const struct focus_timer_state_changed *ev = as_focus_timer_state_changed(eh);
    return ev != NULL ? *ev : focus_timer_get_state();
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_focus_timer, struct focus_timer_state_changed,
                            focus_timer_update_cb, focus_timer_widget_get_state)
ZMK_SUBSCRIPTION(widget_focus_timer, focus_timer_state_changed);

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

static struct is_connected_notification get_is_hid_connected(const zmk_event_t *eh) {
    struct is_connected_notification *notification = as_is_connected_notification(eh);
    if (notification) {
        return *notification;
    }
    return (struct is_connected_notification){.value = false};
}

static void is_hid_connected_update_cb(struct is_connected_notification is_connected) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.is_connected = is_connected.value;

        draw_hid(widget->obj, widget->cbuf_hid, &widget->state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_is_connected, struct is_connected_notification,
                            is_hid_connected_update_cb, get_is_hid_connected)
ZMK_SUBSCRIPTION(widget_is_connected, is_connected_notification);

static struct claude_usage_notification get_claude_usage(const zmk_event_t *eh) {
    struct claude_usage_notification *notification = as_claude_usage_notification(eh);
    if (notification) {
        return *notification;
    }
    return (struct claude_usage_notification){.five_hour = CLAUDE_USAGE_UNKNOWN,
                                              .seven_day = CLAUDE_USAGE_UNKNOWN};
}

static void claude_usage_update_cb(struct claude_usage_notification usage) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.usage_five_hour = usage.five_hour;
        widget->state.usage_seven_day = usage.seven_day;

        draw_hid(widget->obj, widget->cbuf_hid, &widget->state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_claude_usage, struct claude_usage_notification,
                            claude_usage_update_cb, get_claude_usage)
ZMK_SUBSCRIPTION(widget_claude_usage, claude_usage_notification);

static struct volume_notification get_volume(const zmk_event_t *eh) {
    struct volume_notification *notification = as_volume_notification(eh);
    if (notification) {
        return *notification;
    }
    return (struct volume_notification){.value = 0};
}

static void volume_update_cb(struct volume_notification volume) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.volume = volume.value;

        draw_hid(widget->obj, widget->cbuf_hid, &widget->state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_volume, struct volume_notification, volume_update_cb, get_volume)
ZMK_SUBSCRIPTION(widget_volume, volume_notification);

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->state.usage_five_hour = CLAUDE_USAGE_UNKNOWN;
    widget->state.usage_seven_day = CLAUDE_USAGE_UNKNOWN;

    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *hid = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(hid, widget->cbuf_hid, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

#ifdef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(hid, LV_ALIGN_TOP_LEFT, 46, 0);
    lv_obj_align(middle, LV_ALIGN_TOP_RIGHT, 20, -12);
#else
    // Read top to bottom: eye (rows 0-63), HID canvas (64-131: usage, volume),
    // battery (132-159). The HID canvas is drawn last and covers the eye
    // canvas's 4 blank bottom rows and the battery canvas's first 40.
#if IS_ENABLED(CONFIG_NICE_VIEW_CLAUDE_UPSIDE_DOWN)
    // The top of the screen is at x = 0.
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(hid, LV_ALIGN_TOP_LEFT, 64, 0);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
#else
    // The top of the screen is at x = 160.
    lv_obj_align(middle, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_align(hid, LV_ALIGN_TOP_LEFT, 28, 0);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);
#endif
#endif

    eye_init(&eye, k_cycle_get_32());

    sys_slist_append(&widgets, &widget->node);
    widget_peripheral_battery_init();
    widget_focus_timer_init();
    widget_output_status_init();
    widget_is_connected_init();
    widget_volume_init();
    widget_claude_usage_init();

    k_work_schedule_for_queue(zmk_display_work_q(), &eye_work, K_MSEC(1000 / EYE_FPS));

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
