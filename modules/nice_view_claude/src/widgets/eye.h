/*
 * Flaming eye animation: a lidded eye wreathed in flickering flames whose
 * slit pupil wanders around. Pure math, no LVGL, so it can be compiled and
 * previewed on a desktop.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#define EYE_FPS 8

struct eye_state {
    float px, py;       // pupil offset from the eye centre
    float tx, ty;       // where the pupil is heading
    uint16_t hold;      // frames left to stare at the current target
    uint32_t frame;
    uint32_t rng;
};

void eye_init(struct eye_state *s, uint32_t seed);

// Advance the animation by one frame.
void eye_step(struct eye_state *s);

// Render into buf (width x height, row stride `stride`): 1 = foreground, 0 = background.
void eye_render(const struct eye_state *s, uint8_t *buf, int width, int height, int stride);
