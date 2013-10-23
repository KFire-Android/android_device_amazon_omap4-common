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

#ifndef __LAYER__
#define __LAYER__

#include <stdint.h>
#include <stdbool.h>

typedef struct hwc_display_contents_1 hwc_display_contents_1_t;
typedef struct omap_hwc_device omap_hwc_device_t;
typedef struct hwc_layer_1 hwc_layer_1_t;

struct layer_statistics {
    uint32_t count;
    uint32_t composable;
    uint32_t composable_mask;
    uint32_t scaled;
    uint32_t rgb;
    uint32_t bgr;
    uint32_t nv12;
    uint32_t dockable;
    uint32_t protected;
    uint32_t framebuffer;
    uint32_t mem1d_total;
};
typedef struct layer_statistics layer_statistics_t;

bool is_dockable_layer(const hwc_layer_1_t *layer);
bool is_protected_layer(const hwc_layer_1_t *layer);

bool is_blended_layer(const hwc_layer_1_t *layer);
bool is_rgb_layer(const hwc_layer_1_t *layer);
bool is_bgr_layer(const hwc_layer_1_t *layer);
bool is_nv12_layer(const hwc_layer_1_t *layer);

bool is_scaled_layer(const hwc_layer_1_t *layer);
bool is_upscaled_nv12_layer(omap_hwc_device_t *hwc_dev, const hwc_layer_1_t *layer);

uint32_t get_required_mem1d_size(const hwc_layer_1_t *layer);

bool is_composable_layer(omap_hwc_device_t *hwc_dev, int disp, const hwc_layer_1_t *layer);

void gather_layer_statistics(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *contents);

#endif
