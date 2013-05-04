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

#ifndef __HWC_DEV__
#define __HWC_DEV__

#include <stdint.h>
#include <stdbool.h>

#include <pthread.h>

#include <hardware/hwcomposer.h>
#include <linux/bltsville.h>
#include <video/dsscomp.h>
#include <video/omap_hwc.h>

#include "hal_public.h"
#include "blitter.h"
#include "display.h"
#include "dsscomp.h"

struct omap_hwc_module {
    hwc_module_t base;

    /* currently we use only two FB devices, but decalring for MAX_DISPLAYS */
    IMG_framebuffer_device_public_t *fb_dev[MAX_DISPLAYS];
};
typedef struct omap_hwc_module omap_hwc_module_t;

struct omap_hwc_device {
    /* static data */
    hwc_composer_device_1_t base;
    hwc_procs_t *procs;
    pthread_t hdmi_thread;
    pthread_mutex_t lock;

    /* currently we use only two FB devices, but decalring for MAX_DISPLAYS */
    IMG_framebuffer_device_public_t *fb_dev[MAX_DISPLAYS];

    int fb_fd[MAX_DISPLAYS];     /* file descriptor for /dev/fbx */
    int pipe_fds[2];             /* pipe to event thread */

    int flags_rgb_order;
    int flags_nv12_only;
    float upscaled_nv12_limit;

    int force_sgx;
    int idle;

    dsscomp_state_t dsscomp;
    blitter_config_t blitter;

    display_t *displays[MAX_DISPLAYS];
    bool ext_disp_state;
};
typedef struct omap_hwc_device omap_hwc_device_t;

int set_best_hdmi_mode(omap_hwc_device_t *hwc_dev, int disp, uint32_t xres, uint32_t yres, float xpy);

#endif
