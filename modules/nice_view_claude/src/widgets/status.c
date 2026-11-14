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
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <nice_view_claude/hid.h>
#include <nice_view_claude/focus_timer.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

enum widget_children {
    WIDGET_TOP = 0,
    WIDGET_HID,
    WIDGET_MIDDLE,
    WIDGET_BOTTOM,
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

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
    // Only ~28px of this canvas is visible (the HID canvas overlaps the rest).
    lv_draw_label_dsc_t label_battery;
    init_label_dsc(&label_battery, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_LEFT);
    char battery_text[8];
    if (state->battery_left == 0) {
        strcpy(battery_text, "L --");
    } else {
        snprintf(battery_text, sizeof(battery_text), "L %u%%", state->battery_left);
    }
    lv_canvas_draw_text(canvas, 0, 0, 44, &label_battery, battery_text);
    if (state->battery_right == 0) {
        strcpy(battery_text, "R --");
    } else {
        snprintf(battery_text, sizeof(battery_text), "R %u%%", state->battery_right);
    }
    lv_canvas_draw_text(canvas, 0, 14, 44, &label_battery, battery_text);

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
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc, output_text);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
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
        lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_volume, volume);

        draw_usage_row(canvas, 24, "5h", state->usage_five_hour);
        draw_usage_row(canvas, 47, "7d", state->usage_seven_day);
    } else {
        lv_draw_label_dsc_t label_big;
        init_label_dsc(&label_big, LVGL_FOREGROUND, &lv_font_montserrat_22, LV_TEXT_ALIGN_CENTER);
        lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_volume, "HID");
        lv_canvas_draw_text(canvas, 0, 23, CANVAS_SIZE, &label_big, "not");
        lv_canvas_draw_text(canvas, 0, 50, CANVAS_SIZE, &label_volume, "found");
    }


#ifndef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    // Rotate canvas
    rotate_canvas(canvas, cbuf);
#endif
}

static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, WIDGET_MIDDLE);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_arc_dsc_t arc_dsc;
    init_arc_dsc(&arc_dsc, LVGL_FOREGROUND, 2);
    lv_draw_arc_dsc_t arc_dsc_filled;
    init_arc_dsc(&arc_dsc_filled, LVGL_FOREGROUND, 9);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_dsc_black;
    init_label_dsc(&label_dsc_black, LVGL_BACKGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    if (state->focus_phase != FOCUS_TIMER_IDLE) {
        // Focus timer replaces the profile circle while running:
        //   FOCUS / BREAK (12px)  over  mm:ss (18px)
        lv_draw_label_dsc_t label_phase;
        init_label_dsc(&label_phase, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_CENTER);
        lv_canvas_draw_text(canvas, 0, 6, CANVAS_SIZE, &label_phase,
                            state->focus_phase == FOCUS_TIMER_FOCUS ? "FOCUS" : "BREAK");

        char remaining[8];
        snprintf(remaining, sizeof(remaining), "%u:%02u", state->focus_remaining_s / 60,
                 state->focus_remaining_s % 60);
        lv_canvas_draw_text(canvas, 0, 20, CANVAS_SIZE, &label_dsc, remaining);
    } else {
        // Draw circle
        lv_canvas_draw_arc(canvas, 34, 25, 13, 0, 360, &arc_dsc);

        char label[4];
        snprintf(label, sizeof(label), "%i", state->active_profile_index + 1);
        lv_canvas_draw_text(canvas, 26, 15, 16, &label_dsc, label);
    }

#ifndef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    // Rotate canvas
    rotate_canvas(canvas, cbuf);
#endif
}

#ifndef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, WIDGET_BOTTOM);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw layer
    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        char text[10] = {};

        sprintf(text, "LAYER %i", state->layer_index);

        lv_canvas_draw_text(canvas, 0, 5, CANVAS_SIZE, &label_dsc, text);
    } else {
        lv_canvas_draw_text(canvas, 0, 5, CANVAS_SIZE, &label_dsc, state->layer_label);
    }

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}
#endif

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

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;

#ifdef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    lv_obj_t *bottom = lv_obj_get_child(widget->obj, WIDGET_BOTTOM);
    if (state.label == NULL || strlen(state.label) == 0) {
        char text[10] = {};
        sprintf(text, "LAYER %i", state.index);
        lv_label_set_text(bottom, text);
    } else {
        lv_label_set_text(bottom, state.label);
    }
#else
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
#endif
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

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

    lv_obj_t *hid = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(hid, widget->cbuf_hid, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

#ifdef CONFIG_NICE_VIEW_CLAUDE_HORIZONTAL
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(hid, LV_ALIGN_TOP_LEFT, 46, 0);
    lv_obj_align(middle, LV_ALIGN_TOP_RIGHT, 20, -12);

    lv_obj_t *bottom = lv_label_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_RIGHT, 0, 3);
    lv_obj_set_style_text_color(bottom, LVGL_FOREGROUND, 0);
#else
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_align(hid, LV_ALIGN_TOP_LEFT, 64, 0);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, -4, 0);

    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, -44, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
#endif

    sys_slist_append(&widgets, &widget->node);
    widget_peripheral_battery_init();
    widget_focus_timer_init();
    widget_output_status_init();
    widget_layer_status_init();
    widget_is_connected_init();
    widget_volume_init();
    widget_claude_usage_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
