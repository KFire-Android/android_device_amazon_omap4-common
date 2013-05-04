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

#ifndef __DSSCOMP__
#define __DSSCOMP__

#include <stdint.h>

#include <video/dsscomp.h>

#define MAX_DSS_MANAGERS 3
#define MAX_DSS_OVERLAYS 4
#define NUM_NONSCALING_OVERLAYS 1

typedef struct omap_hwc_device omap_hwc_device_t;
typedef struct hwc_layer_1 hwc_layer_1_t;

struct dsscomp_state {
    int fd;              /* file descriptor for /dev/dsscomp */

    struct dsscomp_platform_info limits;

    uint32_t sync_id;

    int last_ext_ovls;   /* # of overlays on external/internal display for last composition */
    int last_int_ovls;
};
typedef struct dsscomp_state dsscomp_state_t;

int init_dsscomp(omap_hwc_device_t *hwc_dev);
void close_dsscomp(omap_hwc_device_t *hwc_dev);

int get_dsscomp_display_info(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_display_info *info);
int get_dsscomp_display_mode_db(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_videomode *mode_db, uint32_t *mode_db_len);
int setup_dsscomp_display(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_videomode *mode);

void setup_dsscomp_manager(omap_hwc_device_t *hwc_dev, int disp);

bool can_dss_scale(omap_hwc_device_t *hwc_dev, uint32_t src_w, uint32_t src_h,
                   uint32_t dst_w, uint32_t dst_h, bool is_2d,
                   struct dsscomp_display_info *dis, uint32_t pclk);

bool can_dss_render_all_layers(omap_hwc_device_t *hwc_dev, int disp);
bool can_dss_render_layer(omap_hwc_device_t *hwc_dev, int disp, hwc_layer_1_t *layer);

uint32_t decide_dss_wb_capture_mode(uint32_t src_xres, uint32_t src_yres, uint32_t dst_xres, uint32_t dst_yres);

void setup_dss_overlay(int width, int height, uint32_t format, bool blended, int zorder, struct dss2_ovl_info *ovl);
void adjust_dss_overlay_to_layer(hwc_layer_1_t const *layer, int zorder, struct dss2_ovl_info *ovl);
void adjust_dss_overlay_to_display(omap_hwc_device_t *hwc_dev, int disp, struct dss2_ovl_info *ovl);

int validate_dss_composition(omap_hwc_device_t *hwc_dev, struct dsscomp_setup_dispc_data *dsscomp);

#endif
