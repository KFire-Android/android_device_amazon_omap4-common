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

#ifndef __DUMP__
#define __DUMP__

#include <stdbool.h>

struct dump_buf {
    char *buf;
    int buf_len;
    int len;
};
typedef struct dump_buf dump_buf_t;

struct dsscomp_setup_dispc_data;
typedef struct hwc_layer_1 hwc_layer_1_t;
typedef struct hwc_display_contents_1 hwc_display_contents_1_t;
typedef struct omap_hwc_device omap_hwc_device_t;

/* dumpsys SurfaceFlinger */
void dump_hwc_info(omap_hwc_device_t *hwc_dev, dump_buf_t *log);
void dump_display(omap_hwc_device_t *hwc_dev, dump_buf_t *log, int disp);

/* Internal debug */
void dump_layer(const hwc_layer_1_t *layer);
void dump_layer_ext(const hwc_layer_1_t *layer, bool invalid_layer);
void dump_layers_ext(const hwc_layer_1_t *list, uint32_t num_layers, uint32_t invalid_layer_ix);
void dump_prepare_info(omap_hwc_device_t *hwc_dev, int disp);
void dump_set_info(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *list);
void dump_dsscomp(struct dsscomp_setup_dispc_data *d);
void dump_post2(omap_hwc_device_t *hwc_dev, int disp);

#endif
