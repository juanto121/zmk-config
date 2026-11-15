/*
 * Flaming eye animation. See eye.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "eye.h"

#include <math.h>
#include <stdbool.h>

// Geometry (pixels), relative to the eye centre.
#define EYE_HALF_WIDTH 28.0f   // almond half width
#define EYE_HALF_HEIGHT 11.0f  // lid height at the centre
#define PUPIL_HALF_WIDTH 2.8f
#define PUPIL_RANGE_X 16.0f
#define PUPIL_RANGE_Y 2.0f
#define FLAME_SPACING 6        // pixels between flame tongues

static uint32_t xorshift(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

// Deterministic per-(flame, time) noise in 0..255.
static uint32_t hash2(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0xC2B2AE3Du;
    h ^= h >> 13;
    return h & 0xFF;
}

static float rand_range(uint32_t *rng, float lo, float hi) {
    return lo + (hi - lo) * (float)(xorshift(rng) & 0xFFFF) / 65535.0f;
}

void eye_init(struct eye_state *s, uint32_t seed) {
    s->px = s->py = 0.0f;
    s->tx = s->ty = 0.0f;
    s->hold = EYE_FPS;
    s->frame = 0;
    s->rng = seed ? seed : 0xC0FFEEu;
}

void eye_step(struct eye_state *s) {
    s->frame++;

    float dx = s->tx - s->px;
    float dy = s->ty - s->py;
    if (fabsf(dx) < 0.4f && fabsf(dy) < 0.4f) {
        if (s->hold > 0) {
            s->hold--;
        } else {
            // pick somewhere new to look; occasionally snap back to the centre
            if ((xorshift(&s->rng) & 7) == 0) {
                s->tx = 0.0f;
                s->ty = 0.0f;
            } else {
                s->tx = rand_range(&s->rng, -PUPIL_RANGE_X, PUPIL_RANGE_X);
                s->ty = rand_range(&s->rng, -PUPIL_RANGE_Y, PUPIL_RANGE_Y);
            }
            s->hold = (uint16_t)rand_range(&s->rng, EYE_FPS / 2, EYE_FPS * 3);
        }
    } else {
        // ease towards the target: a slow, searching sweep
        s->px += dx * 0.22f;
        s->py += dy * 0.22f;
    }
}

// Lid half-height at horizontal offset x (0 outside the almond).
static float lid(float x) {
    float t = x / EYE_HALF_WIDTH;
    return t >= 1.0f || t <= -1.0f ? 0.0f : EYE_HALF_HEIGHT * (1.0f - t * t);
}

// Height of the flame tongue covering column x, above (up) or below the eye.
static float flame(int x, uint32_t tick, bool up) {
    int index = (x + 1000 * FLAME_SPACING) / FLAME_SPACING;
    float centre = (float)(index * FLAME_SPACING - 1000 * FLAME_SPACING) + FLAME_SPACING / 2.0f;
    float distance = fabsf((float)x - centre);

    // tallest in the middle, dying out towards the corners of the eye
    float t = centre / (EYE_HALF_WIDTH - 2.0f);
    float envelope = 1.0f - t * t;
    if (envelope <= 0.0f) {
        return 0.0f;
    }
    uint32_t noise = hash2((uint32_t)index * 2u + (up ? 1u : 0u), tick);
    float height = up ? 7.0f + (float)(noise % 12) : 2.0f + (float)(noise % 5);
    height *= up ? envelope : sqrtf(envelope);

    // tapered tongue; neighbouring tongues overlap at the base into one blaze
    float shape = 1.0f - distance / (FLAME_SPACING / 2.0f + 1.5f);
    return shape > 0.0f ? height * shape * shape : 0.0f;
}

void eye_render(const struct eye_state *s, uint8_t *buf, int width, int height, int stride) {
    const float cx = width / 2.0f;
    const float cy = height / 2.0f + 1.0f;
    const uint32_t tick = s->frame / 2;  // flames flicker at half the frame rate

    // pupil narrows a little as it moves towards the corners (perspective)
    const float pupil_half_width =
        PUPIL_HALF_WIDTH * (1.0f - 0.35f * fabsf(s->px) / EYE_HALF_WIDTH);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float h = lid(dx);
            float ady = fabsf(dy);
            uint8_t on = 0;

            if (ady < h) {
                // inside the eye
                float pdx = fabsf(dx - s->px);
                float pdy = dy - s->py;
                float slit_t = pdy / (EYE_HALF_HEIGHT + 1.0f);
                float slit = pupil_half_width * (1.0f - slit_t * slit_t);
                float r = sqrtf(pdx * pdx + pdy * pdy * 2.0f);

                if (ady > h - 1.2f) {
                    on = 1;  // lid outline
                } else if (slit > 0.0f && pdx < slit) {
                    on = 1;  // slit pupil
                } else if (slit > 0.0f && pdx < slit + 1.5f) {
                    on = 0;  // bright rim around the pupil
                } else if (r < 7.0f) {
                    on = ((x & 1) && (y & 1));           // 25%: glowing core
                } else if (r < 13.0f) {
                    on = ((x + y) & 1) == 0;             // 50%
                } else {
                    on = !((x & 1) == 0 && (y & 1) == 0); // 75%: dark edges
                }
            } else {
                // around the eye: flame tongues
                float beyond = ady - h;
                bool up = dy < 0.0f;
                if (beyond < flame(x - (int)cx, tick, up)) {
                    on = 1;
                }
            }
            buf[y * stride + x] = on;
        }
    }
}
