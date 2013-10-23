/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include <hardware/hwcomposer.h>

#include "utils.h"

#define ASPECT_RATIO_TOLERANCE 0.02f

const transform_matrix unit_matrix = { { 1., 0., 0. }, { 0., 1., 0. } };

void translate_matrix(transform_matrix m, float dx, float dy)
{
    m[0][2] += dx;
    m[1][2] += dy;
}

static inline void scale_vector(float m[3], int from, int to)
{
    m[0] = m[0] * to / from;
    m[1] = m[1] * to / from;
    m[2] = m[2] * to / from;
}

inline void scale_matrix(transform_matrix m, int x_from, int x_to, int y_from, int y_to)
{
    scale_vector(m[0], x_from, x_to);
    scale_vector(m[1], y_from, y_to);
}

void rotate_matrix(transform_matrix m, int quarter_turns)
{
    if (quarter_turns & 2)
        scale_matrix(m, 1, -1, 1, -1);
    if (quarter_turns & 1) {
        float q;
        q = m[0][0]; m[0][0] = -m[1][0]; m[1][0] = q;
        q = m[0][1]; m[0][1] = -m[1][1]; m[1][1] = q;
        q = m[0][2]; m[0][2] = -m[1][2]; m[1][2] = q;
    }
}

static int round_float(float x)
{
    /* int truncates towards 0 */
    return (int) (x < 0 ? x - 0.5 : x + 0.5);
}

void transform_rect(transform_matrix m, hwc_rect_t *rect)
{
    int rect_width = WIDTH(*rect);
    int rect_height = HEIGHT(*rect);
    float x, y, w, h;

    x = m[0][0] * rect->left + m[0][1] * rect->top + m[0][2];
    y = m[1][0] * rect->left + m[1][1] * rect->top + m[1][2];
    w = m[0][0] * rect_width + m[0][1] * rect_height;
    h = m[1][0] * rect_width + m[1][1] * rect_height;

    rect->left = round_float(w > 0 ? x : x + w);
    rect->top = round_float(h > 0 ? y : y + h);

    /* Compensate position rounding error by adjusting layer size */
    w += w > 0 ? x - rect->left : rect->left - (x + w);
    h += h > 0 ? y - rect->top : rect->top - (y + h);

    rect->right = rect->left + round_float(w > 0 ? w : -w);
    rect->bottom = rect->top + round_float(h > 0 ? h : -h);
}

void get_max_dimensions(uint32_t orig_xres, uint32_t orig_yres,
                        float xpy,
                        uint32_t scr_xres, uint32_t scr_yres,
                        uint32_t scr_width, uint32_t scr_height,
                        uint32_t *adj_xres, uint32_t *adj_yres)
{
    /* Assume full screen (largest size) */
    *adj_xres = scr_xres;
    *adj_yres = scr_yres;

    /* Assume 1:1 pixel ratios if none supplied */
    if (!scr_width || !scr_height) {
        scr_width = scr_xres;
        scr_height = scr_yres;
    }

    /* Trim to keep aspect ratio */
    float x_factor = orig_xres * xpy * scr_height;
    float y_factor = orig_yres *       scr_width;

    /* Allow for tolerance so we avoid scaling if framebuffer is standard size */
    if (x_factor < y_factor * (1.f - ASPECT_RATIO_TOLERANCE))
        *adj_xres = (uint32_t) (x_factor * *adj_xres / y_factor + 0.5);
    else if (x_factor * (1.f - ASPECT_RATIO_TOLERANCE) > y_factor)
        *adj_yres = (uint32_t) (y_factor * *adj_yres / x_factor + 0.5);
}
