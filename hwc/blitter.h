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

#ifndef __BLITTER__
#define __BLITTER__

#include <stdint.h>
#include <stdbool.h>

#include <hardware/hwcomposer.h>

#include "rgz_2d.h"

typedef struct hwc_display_contents_1 hwc_display_contents_1_t;
typedef struct omap_hwc_device omap_hwc_device_t;

enum blt_policy {
    BLT_POLICY_DISABLED = 0,
    BLT_POLICY_DEFAULT = 1,    /* Default blit policy */
    BLT_POLICY_ALL,            /* Test mode to attempt to blit all */
};

enum blt_mode {
    BLT_MODE_PAINT = 0,    /* Attempt to blit layer by layer */
    BLT_MODE_REGION = 1,   /* Attempt to blit layers via regions */
};

struct blitter_config {
    bool debug;

    enum blt_mode mode;
    enum blt_policy policy;
};
typedef struct blitter_config blitter_config_t;

int init_blitter(omap_hwc_device_t *hwc_dev);
uint32_t get_blitter_policy(omap_hwc_device_t *hwc_dev, int disp);
void reset_blitter(omap_hwc_device_t *hwc_dev);
void release_blitter(void);
bool blit_layers(omap_hwc_device_t *hwc_dev, hwc_display_contents_1_t *contents);
uint32_t get_blitter_data_size(omap_hwc_device_t *hwc_dev);

#endif
