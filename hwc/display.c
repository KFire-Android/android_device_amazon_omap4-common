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

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <DSSWBHal.h>

#include <linux/fb.h>
#include <video/dsscomp.h>
#ifdef USE_LIBION_TI
#include <ion_ti/ion.h>
#else
#include <ion.h>
#endif

#include "hwc_dev.h"
#include "color_fmt.h"
#include "display.h"
#include "dsscomp.h"
#include "layer.h"
#include "sw_vsync.h"
#include "utils.h"

#define LCD_DISPLAY_CONFIGS 1
#define LCD_DISPLAY_FPS 60
#define LCD_DISPLAY_DEFAULT_DPI 150

/* Currently SF cannot handle more than 1 config */
#define HDMI_DISPLAY_CONFIGS 1
#define HDMI_DISPLAY_FPS 60
#define HDMI_DISPLAY_DEFAULT_DPI 75

/* Currently SF cannot handle more than 1 config */
#define WFD_DISPLAY_CONFIGS 1
#define WFD_DISPLAY_FPS 60
#define WFD_DISPLAY_DEFAULT_DPI 75

#define MAX_DISPLAY_ID (MAX_DISPLAYS - 1)
#define INCH_TO_MM 25.4f

/* Used by property settings */
enum {
    EXT_ROTATION    = 3,        /* rotation while mirroring */
    EXT_HFLIP       = (1 << 2), /* flip l-r on output (after rotation) */
};

static void free_display(display_t *display)
{
    if (display) {
        if (display->configs)
            free(display->configs);
        if (display->composition.buffers)
            free(display->composition.buffers);

        free(display);
    }
}

static void remove_display(omap_hwc_device_t *hwc_dev, int disp)
{
    free_display(hwc_dev->displays[disp]);
    hwc_dev->displays[disp] = NULL;
}

static int allocate_display(size_t display_data_size, uint32_t max_configs, display_t **new_display)
{
    int err = 0;

    display_t *display = (display_t *)malloc(display_data_size);
    if (display == NULL) {
        err = -ENOMEM;
        goto err_out;
    }

    memset(display, 0, display_data_size);

    display->num_configs = max_configs;
    size_t config_data_size = sizeof(*display->configs) * display->num_configs;
    display->configs = (display_config_t *)malloc(config_data_size);
    if (display->configs == NULL) {
        err = -ENOMEM;
        goto err_out;
    }

    memset(display->configs, 0, config_data_size);

    /* Allocate the maximum buffers that we can receive from HWC */
    display->composition.buffers = malloc(sizeof(buffer_handle_t) * MAX_COMPOSITION_BUFFERS);
    if (!display->composition.buffers) {
        err = -ENOMEM;
        goto err_out;
    }

    memset(display->composition.buffers, 0, sizeof(buffer_handle_t) * MAX_COMPOSITION_BUFFERS);

err_out:

    if (err) {
        ALOGE("Failed to allocate display (configs = %d)", max_configs);
        free_display(display);
    } else {
        *new_display = display;
    }

    return err;
}

static int free_tiler2d_buffers(external_hdmi_display_t *display)
{
    int i;
    for (i = 0; i < EXTERNAL_DISPLAY_BACK_BUFFERS; i++) {
        ion_free(display->ion_fd, display->ion_handles[i]);
        display->ion_handles[i] = NULL;
    }

    return 0;
}

static int allocate_tiler2d_buffers(omap_hwc_device_t *hwc_dev, external_hdmi_display_t *display)
{
    int ret, i;
    size_t stride;

    for (i = 0; i < EXTERNAL_DISPLAY_BACK_BUFFERS; i++) {
        if (display->ion_handles[i])
            return 0;
    }

    IMG_framebuffer_device_public_t *fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY];
    for (i = 0; i < EXTERNAL_DISPLAY_BACK_BUFFERS; i++) {
        ret = ion_alloc_tiler(display->ion_fd, fb_dev->base.width, fb_dev->base.height,
                              TILER_PIXEL_FMT_32BIT, 0, &display->ion_handles[i], &stride);
        if (ret)
            goto handle_error;

        ALOGI("ion handle[%d][%p]", i, display->ion_handles[i]);
    }

    return 0;

handle_error:

    free_tiler2d_buffers(display);
    return -ENOMEM;
}

static int get_display_info(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *contents,
                            hwc_display_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->dpy = disp;

    if (!(contents->flags & HWC_EXTENDED_API) || !hwc_dev->procs || !hwc_dev->procs->extension_cb)
        return -EACCES;

    int err = hwc_dev->procs->extension_cb(hwc_dev->procs, HWC_EXTENDED_OP_DISPLAYINFO,
            (void **) &info, sizeof(*info));

    if (err)
        err = -ENODEV;

    return err;
}

static void setup_config(display_config_t *config, int xres, int yres, struct dsscomp_display_info *info,
                         int default_fps, int default_dpi)
{
    config->xres = xres;
    config->yres = yres;
    config->fps = default_fps;

    if (info->width_in_mm && info->height_in_mm) {
        config->xdpi = (int)(config->xres * INCH_TO_MM) / info->width_in_mm;
        config->ydpi = (int)(config->yres * INCH_TO_MM) / info->height_in_mm;
    } else {
        config->xdpi = default_dpi;
        config->ydpi = default_dpi;
    }
}

static void setup_lcd_config(display_config_t *config, int xres, int yres, struct dsscomp_display_info *info)
{
    setup_config(config, xres, yres, info, LCD_DISPLAY_FPS, LCD_DISPLAY_DEFAULT_DPI);
}

static void setup_hdmi_config(display_config_t *config, int xres, int yres, struct dsscomp_display_info *info)
{
    setup_config(config, xres, yres, info, HDMI_DISPLAY_FPS, HDMI_DISPLAY_DEFAULT_DPI);
}

static void setup_wfd_config(display_config_t *config, hwc_display_info_t *info)
{
    config->xres = info->width;
    config->yres = info->height;
    config->fps = WFD_DISPLAY_FPS;
    config->xdpi = WFD_DISPLAY_DEFAULT_DPI;
    config->ydpi = WFD_DISPLAY_DEFAULT_DPI;
}

static int init_primary_lcd_display(omap_hwc_device_t *hwc_dev, uint32_t xres, uint32_t yres, struct dsscomp_display_info *info)
{
    int err;

    err = allocate_display(sizeof(primary_lcd_display_t), LCD_DISPLAY_CONFIGS, &hwc_dev->displays[HWC_DISPLAY_PRIMARY]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];

    setup_lcd_config(&display->configs[0], xres, yres, info);

    display->type = DISP_TYPE_LCD;

    return 0;
}

static int init_primary_hdmi_display(omap_hwc_device_t *hwc_dev, uint32_t xres, uint32_t yres, struct dsscomp_display_info *info)
{
    int err;

    err = allocate_display(sizeof(primary_hdmi_display_t), HDMI_DISPLAY_CONFIGS, &hwc_dev->displays[HWC_DISPLAY_PRIMARY]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];

    /*
     * At startup primary HDMI display may be connected or not. To make sure that SurfaceFlinger
     * behavior does not depend on that, we always pretend the worst case (the display is
     * disconnected). All parameters derived from display info (dpi and xpi) should be replaced
     * by default values. It's enough to override physical panel dimensions with 0 to achieve that.
     */
    info->width_in_mm = 0;
    info->height_in_mm = 0;

    setup_hdmi_config(&display->configs[0], xres, yres, info);

    display->type = DISP_TYPE_HDMI;

    return 0;
}

static void update_primary_display_orientation(omap_hwc_device_t *hwc_dev) {
    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    hwc_display_info_t display_info;

    int err = get_display_info(hwc_dev, HWC_DISPLAY_PRIMARY, display->contents, &display_info);
    if (err)
        return;

    primary_display_t *primary = get_primary_display_info(hwc_dev);
    if (!primary)
        return;

    if (primary->orientation != display_info.orientation) {
        primary->orientation = display_info.orientation;

        int ext_disp = get_external_display_id(hwc_dev);
        if (is_external_display_mirroring(hwc_dev, ext_disp))
            hwc_dev->displays[ext_disp]->update_transform = true;
    }
}

static void set_primary_display_transform_matrix(omap_hwc_device_t *hwc_dev)
{
    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];

    /* Create primary display translation matrix */
    int lcd_w = display->fb_info.timings.x_res;
    int lcd_h = display->fb_info.timings.y_res;
    IMG_framebuffer_device_public_t *fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY];
    int orig_w = fb_dev->base.width;
    int orig_h = fb_dev->base.height;
    hwc_rect_t region = {.left = 0, .top = 0, .right = orig_w, .bottom = orig_h};
    display_transform_t *transform = &display->transform;

    transform->region = region;
    transform->rotation = ((lcd_w > lcd_h) ^ (orig_w > orig_h)) ? 1 : 0;
    transform->scaling = ((lcd_w != orig_w) || (lcd_h != orig_h));

    ALOGI("Transforming FB (%dx%d) => (%dx%d) rot%d", orig_w, orig_h, lcd_w, lcd_h, transform->rotation);

    /* Reorientation matrix is:
       m = (center-from-target-center) * (scale-to-target) * (mirror) * (rotate) * (center-to-original-center) */
    memcpy(transform->matrix, unit_matrix, sizeof(unit_matrix));
    translate_matrix(transform->matrix, -(orig_w >> 1), -(orig_h >> 1));
    rotate_matrix(transform->matrix, transform->rotation);

    if (transform->rotation & 1)
         SWAP(orig_w, orig_h);

    scale_matrix(transform->matrix, orig_w, lcd_w, orig_h, lcd_h);
    translate_matrix(transform->matrix, lcd_w >> 1, lcd_h >> 1);
}

static void set_external_display_transform_matrix(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    display_transform_t *transform = &display->transform;
    int orig_xres = WIDTH(transform->region);
    int orig_yres = HEIGHT(transform->region);
    float orig_center_x = transform->region.left + orig_xres / 2.0f;
    float orig_center_y = transform->region.top + orig_yres / 2.0f;

    /* Reorientation matrix is:
       m = (center-from-target-center) * (scale-to-target) * (mirror) * (rotate) * (center-to-original-center) */

    memcpy(transform->matrix, unit_matrix, sizeof(unit_matrix));
    translate_matrix(transform->matrix, -orig_center_x, -orig_center_y);
    rotate_matrix(transform->matrix, transform->rotation);
    if (transform->hflip)
        scale_matrix(transform->matrix, 1, -1, 1, 1);

    primary_display_t *primary = get_primary_display_info(hwc_dev);
    if (!primary)
        return;

    float xpy = primary->xpy;

    if (transform->rotation & 1) {
        SWAP(orig_xres, orig_yres);
        xpy = 1.0f / xpy;
    }

    /* get target size */
    uint32_t adj_xres, adj_yres;
    uint32_t width, height;
    int xres, yres;

    if (is_hdmi_display(hwc_dev, disp)) {
        hdmi_display_t *hdmi = &((external_hdmi_display_t*)display)->hdmi;
        width = hdmi->width;
        height = hdmi->height;
        xres = hdmi->mode_db[~hdmi->video_mode_ix].xres;
        yres = hdmi->mode_db[~hdmi->video_mode_ix].yres;
    } else {
        display_config_t *config = &display->configs[display->active_config_ix];
        width = 0;
        height = 0;
        xres = config->xres;
        yres = config->yres;

        if (is_wfd_display(hwc_dev, disp) && (primary->orientation & 1)) {
            /*
             * We are going to do rotation on WB overlay that uses TILER2D buffer. In case of 90
             * degree rotation the cloned overlays should be placed on rotated view of the external
             * display, so the external transform matrix has to be calculated accordingly.
             */
            SWAP(xres, yres);
        }
    }

    display->transform.scaling = ((xres != orig_xres) || (yres != orig_yres));

    get_max_dimensions(orig_xres, orig_yres, xpy,
                       xres, yres, width, height,
                       &adj_xres, &adj_yres);

    scale_matrix(transform->matrix, orig_xres, adj_xres, orig_yres, adj_yres);
    translate_matrix(transform->matrix, xres >> 1, yres >> 1);
}

static int setup_external_display_transform(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];

    if (display->mode == DISP_MODE_PRESENTATION) {
        display_config_t *config = &display->configs[display->active_config_ix];
        struct hwc_rect src_region = { .right = config->xres, .bottom = config->yres };
        display->transform.region = src_region;
    } else {
        primary_display_t *primary = get_primary_display_info(hwc_dev);
        if (!primary)
            return -ENODEV;

        display->transform.region = primary->mirroring_region;
    }

    uint32_t xres = WIDTH(display->transform.region);
    uint32_t yres = HEIGHT(display->transform.region);

    if (!(xres && yres))
        return -EINVAL;

    int rot_flip = (yres > xres) ? 3 : 0;
    display->transform.rotation = rot_flip & EXT_ROTATION;
    display->transform.hflip = (rot_flip & EXT_HFLIP) > 0;

    if (display->transform.rotation & 1)
        SWAP(xres, yres);

    if (is_hdmi_display(hwc_dev, disp)) {
        primary_display_t *primary = get_primary_display_info(hwc_dev);
        if (!primary || set_best_hdmi_mode(hwc_dev, disp, xres, yres, primary->xpy))
            return -ENODEV;
    }

    set_external_display_transform_matrix(hwc_dev, disp);

    if (is_hdmi_display(hwc_dev, disp) && display->transform.rotation) {
        /*
         * Allocate backup buffers for FB rotation. This is required only if the FB transform is
         * different from that of the external display and the FB is not in TILER2D space.
         */
        external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t*)display;
        if (ext_hdmi->ion_fd < 0 && (hwc_dev->dsscomp.limits.fbmem_type != DSSCOMP_FBMEM_TILER2D)) {
            ext_hdmi->ion_fd = ion_open();
            if (ext_hdmi->ion_fd >= 0) {
                allocate_tiler2d_buffers(hwc_dev, ext_hdmi);
            } else {
                ALOGE("Failed to open ion driver (%d)", errno);
            }
        }
    }

    return 0;
}

static int add_virtual_wfd_display(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *contents)
{
    int err;

    hwc_display_info_t display_info;
    err = get_display_info(hwc_dev, disp, contents, &display_info);
    if (err)
        return err;

    primary_display_t *primary = get_primary_display_info(hwc_dev);
    if (!primary)
        return -ENODEV;

    err = allocate_display(sizeof(external_wfd_display_t), WFD_DISPLAY_CONFIGS, &hwc_dev->displays[disp]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[disp];

    setup_wfd_config(&display->configs[0], &display_info);

    display->type = DISP_TYPE_WFD;
    display->role = DISP_ROLE_EXTERNAL;
    display->mode = DISP_MODE_INVALID;
    display->mgr_ix = 1;
    display->blanked = hwc_dev->displays[HWC_DISPLAY_PRIMARY]->blanked;
    display->update_transform = true;

    // HACK: WFD display does not have its own FB device, so instead we use FB of external HDMI display
    hwc_dev->fb_dev[disp] = hwc_dev->fb_dev[HWC_DISPLAY_EXTERNAL];

    return 0;
}

static int update_virtual_display(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *contents)
{
    hwc_display_info_t display_info;
    int err = get_display_info(hwc_dev, disp, contents, &display_info);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[disp];
    display_config_t *config = &display->configs[display->active_config_ix];

    if ((uint32_t)config->xres != display_info.width || (uint32_t)config->yres != display_info.height) {
        setup_wfd_config(config, &display_info);

        display->update_transform = true;
    }

    return 0;
}

static int capture_black_frame(omap_hwc_device_t *hwc_dev, int disp)
{
    wfd_display_t *wfd = (wfd_display_t *)hwc_dev->displays[disp];
    int attempt = 0;
    int got_buffer;
    int err;

    while (!(got_buffer = wb_capture_layer(&wfd->wb_layer)) && attempt < 5) {
        struct timespec sleep_time;
        sleep_time.tv_sec = 0;
        sleep_time.tv_nsec = 10000000; /* 10ms */
        nanosleep(&sleep_time, NULL);
    }

    if (!got_buffer || !wfd->wb_layer.handle) {
        ALOGE("Failed get a buffer");
        return -ENODEV;
    }

    gralloc_module_t *gralloc_module;
    hwc_layer_1_t *layer = &wfd->wb_layer;
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;
    void *buffer_ptr[MAX_SUB_ALLOCS] = {0};

    err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (hw_module_t const **)&gralloc_module);
    ALOGE_IF(err, "Failed to get gralloc module instance (%d)", err);

    if (!err) {
        err = gralloc_module->lock(gralloc_module, layer->handle, GRALLOC_USAGE_SW_WRITE_RARELY,
                0, 0, handle->iWidth, handle->iHeight, (void **)buffer_ptr);
        ALOGE_IF(err, "Failed to lock buffer %p (%d)", layer->handle, err);
    }

    if (!err) {
        int stride = get_stride_from_format(handle->iFormat, handle->iWidth);
        int i;

        for (i = 0; i < handle->iHeight; i++) {
            char *line = (char *)buffer_ptr[0] + i * stride;
            memset(line, 0x00, handle->iWidth);
        }

        for (i = 0; i < handle->iHeight / 2; i++) {
            char *line = (char *)buffer_ptr[0] + (handle->iHeight + i) * stride;
            memset(line, 0x80, handle->iWidth);
        }

        err = gralloc_module->unlock(gralloc_module, layer->handle);
        ALOGE_IF(err, "Failed to unlock buffer %p (%d)", layer->handle, err);
    }

    wb_capture_started(layer->handle, 0);

    return err;
}

static int init_hdmi_display(omap_hwc_device_t *hwc_dev, int disp)
{
    hdmi_display_t *hdmi = (hdmi_display_t*)hwc_dev->displays[disp];
    if (!hdmi)
        return -ENODEV;

    uint32_t mode_db_len = sizeof(hdmi->mode_db) / sizeof(hdmi->mode_db[0]);
    int err = get_dsscomp_display_mode_db(hwc_dev, hwc_dev->displays[disp]->mgr_ix, hdmi->mode_db, &mode_db_len);
    if (!err)
        hdmi->base.fb_info.modedb_len = mode_db_len;

    return err;
}

int init_primary_display(omap_hwc_device_t *hwc_dev)
{
    if (hwc_dev->displays[HWC_DISPLAY_PRIMARY]) {
        ALOGE("Display %d is already connected", HWC_DISPLAY_PRIMARY);
        return -EBUSY;
    }

    int err;
    struct dsscomp_display_info fb_info;
    err = get_dsscomp_display_info(hwc_dev, HWC_DISPLAY_PRIMARY, &fb_info);
    if (err)
        return err;

    IMG_framebuffer_device_public_t *fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY];
    uint32_t xres = fb_dev->base.width;
    uint32_t yres = fb_dev->base.height;

    switch (fb_info.channel) {
        case OMAP_DSS_CHANNEL_LCD:
        case OMAP_DSS_CHANNEL_LCD2:
            err = init_primary_lcd_display(hwc_dev, xres, yres, &fb_info);
            break;
        case OMAP_DSS_CHANNEL_DIGIT:
            err = init_primary_hdmi_display(hwc_dev, xres, yres, &fb_info);
            break;
        default:
            return -ENODEV;
    }

    if (err)
        return -ENODEV;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    display->fb_info = fb_info;
    display->role = DISP_ROLE_PRIMARY;
    display->mode = DISP_MODE_PRESENTATION;
    display->mgr_ix = 0;
    display->blanked = true;

    set_primary_display_transform_matrix(hwc_dev);

    primary_display_t *primary = get_primary_display_info(hwc_dev);

    if (!primary) {
        remove_display(hwc_dev, HWC_DISPLAY_PRIMARY);
        return -ENODEV;
    }

    if (use_sw_vsync()) {
        init_sw_vsync(hwc_dev);
        primary->use_sw_vsync = true;
    }

    if (fb_info.timings.x_res && fb_info.height_in_mm) {
        primary->xpy = (float) fb_info.width_in_mm / fb_info.timings.x_res /
                               fb_info.height_in_mm * fb_info.timings.y_res;
    } else {
        /* Use default value in case some of requested display parameters missing */
        primary->xpy = 1.0f;
    }

    /* get the board specific clone properties */
    /* eg: 0:0:1280:720 */
    char value[PROPERTY_VALUE_MAX];
    if (property_get("persist.hwc.mirroring.region", value, "") <= 0 ||
        sscanf(value, "%d:%d:%d:%d",
           &primary->mirroring_region.left, &primary->mirroring_region.top,
           &primary->mirroring_region.right, &primary->mirroring_region.bottom) != 4 ||
           primary->mirroring_region.left >= primary->mirroring_region.right ||
           primary->mirroring_region.top >= primary->mirroring_region.bottom) {
        struct hwc_rect fb_region = { .right = xres, .bottom = yres };
        primary->mirroring_region = fb_region;
    }
    ALOGI("clone region is set to (%d,%d) to (%d,%d)",
            primary->mirroring_region.left, primary->mirroring_region.top,
            primary->mirroring_region.right, primary->mirroring_region.bottom);

    return 0;
}

int configure_primary_hdmi_display(omap_hwc_device_t *hwc_dev)
{
    uint32_t xres = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.width;
    uint32_t yres = hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.height;
    int err;

    err = init_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY);
    if (err)
        return err;

    primary_display_t *primary = get_primary_display_info(hwc_dev);
    if (!primary)
        return -ENODEV;

    err = set_best_hdmi_mode(hwc_dev, HWC_DISPLAY_PRIMARY, xres, yres, primary->xpy);
    if (err) {
        ALOGE("Failed to set HDMI mode");
        return err;
    }

    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    display->update_transform = true;

    err = get_dsscomp_display_info(hwc_dev, HWC_DISPLAY_PRIMARY, &display->fb_info);
    if (err)
        return err;

    return 0;
}

void reset_primary_display(omap_hwc_device_t *hwc_dev)
{
    int ret;
    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    if (!display)
        return;

    /* Remove bootloader image from the screen as blank/unblank does not change the composition */
    struct dsscomp_setup_dispc_data data = {
        .num_mgrs = 1
    };

    data.mgrs[0].alpha_blending = 1;

    ret = ioctl(hwc_dev->dsscomp.fd, DSSCIOC_SETUP_DISPC, &data);
    if (ret)
        ALOGW("Failed to remove bootloader image");

    /* Blank and unblank fd to make sure display is properly programmed on boot.
     * This is needed because the bootloader can not be trusted.
     */
    blank_display(hwc_dev, HWC_DISPLAY_PRIMARY);
    unblank_display(hwc_dev, HWC_DISPLAY_PRIMARY);
}

primary_display_t *get_primary_display_info(omap_hwc_device_t *hwc_dev)
{
    display_t *display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    primary_display_t *primary = NULL;

    if (is_lcd_display(hwc_dev, HWC_DISPLAY_PRIMARY))
        primary = &((primary_lcd_display_t*)display)->primary;
    else if (is_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY))
        primary = &((primary_hdmi_display_t*)display)->primary;

    return primary;
}

int add_external_hdmi_display(omap_hwc_device_t *hwc_dev)
{
    if (hwc_dev->displays[HWC_DISPLAY_EXTERNAL]) {
        ALOGE("Display %d is already connected", HWC_DISPLAY_EXTERNAL);
        return -EBUSY;
    }

    int err;
    struct dsscomp_display_info info;
    err = get_dsscomp_display_info(hwc_dev, HWC_DISPLAY_EXTERNAL, &info);
    if (err)
        return err;

    err = allocate_display(sizeof(external_hdmi_display_t), HDMI_DISPLAY_CONFIGS, &hwc_dev->displays[HWC_DISPLAY_EXTERNAL]);
    if (err)
        return err;

    display_t *display = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    display->fb_info = info;
    display->type = DISP_TYPE_HDMI;
    display->role = DISP_ROLE_EXTERNAL;
    display->mgr_ix = 1;

    /* SurfaceFlinger currently doesn't unblank external display on reboot.
     * Unblank HDMI display by default.
     * See SurfaceFlinger::readyToRun() function.
     */
    display->blanked = false;
    display->update_transform = true;

    IMG_framebuffer_device_public_t *fb_dev = hwc_dev->fb_dev[HWC_DISPLAY_EXTERNAL];
    uint32_t xres = fb_dev->base.width;
    uint32_t yres = fb_dev->base.height;

    // TODO: Verify that HDMI supports xres x yres
    // TODO: Set HDMI resolution? What if we need to do docking of 1080p i.s.o. Presentation?

    setup_hdmi_config(&display->configs[0], xres, yres, &display->fb_info);

    external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t*)display;
    ext_hdmi->ion_fd = -1;

    /* check set props */
    char value[PROPERTY_VALUE_MAX];
    property_get("persist.hwc.avoid_mode_change", value, "1");
    ext_hdmi->avoid_mode_change = atoi(value) > 0;

    err = init_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL);
    if (err) {
        remove_external_hdmi_display(hwc_dev);
        return err;
    }

    return 0;
}

void remove_external_hdmi_display(omap_hwc_device_t *hwc_dev)
{
    display_t *display = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
    if (!display) {
        ALOGW("Failed to remove non-existent display %d", HWC_DISPLAY_EXTERNAL);
        return;
    }

    external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t*)display;
    if (ext_hdmi->ion_fd >= 0 && (hwc_dev->dsscomp.limits.fbmem_type != DSSCOMP_FBMEM_TILER2D)) {
        /* free tiler 2D buffer on detach */
        free_tiler2d_buffers(ext_hdmi);
        ion_close(ext_hdmi->ion_fd);
    }

    remove_display(hwc_dev, HWC_DISPLAY_EXTERNAL);
}

struct ion_handle *get_external_display_ion_fb_handle(omap_hwc_device_t *hwc_dev)
{
    external_hdmi_display_t *ext_hdmi = (external_hdmi_display_t *)hwc_dev->displays[HWC_DISPLAY_EXTERNAL];

    if (ext_hdmi) {
        struct dsscomp_setup_dispc_data *dsscomp = &hwc_dev->displays[HWC_DISPLAY_EXTERNAL]->composition.comp_data.dsscomp_data;

        return ext_hdmi->ion_handles[dsscomp->sync_id % EXTERNAL_DISPLAY_BACK_BUFFERS];
    } else {
        return NULL;
    }
}

void detect_virtual_displays(omap_hwc_device_t *hwc_dev, size_t num_displays, hwc_display_contents_1_t **displays) {
    size_t i;
    int err;

    if (num_displays > MAX_DISPLAYS)
        num_displays = MAX_DISPLAYS;

    for (i = HWC_DISPLAY_EXTERNAL + 1; i < num_displays; i++) {
        if (displays[i]) {
            if (!hwc_dev->displays[i]) {
                int ext_disp = get_external_display_id(hwc_dev);
                err = add_virtual_wfd_display(hwc_dev, i, displays[i]);
                if (err)
                    ALOGE("Failed to connect virtual display %d (%d)", i, err);
                else
                    ALOGI("Virtual display %d has been connected", i);
                /* HDMI and WFD display can't work together. Disable WFD display. */
                if (is_hdmi_display(hwc_dev, ext_disp)) {
                    ALOGE("Disable virtual display %d because HDMI display is already connected", i);
                    disable_display(hwc_dev, i);
                }
            } else {
                err = update_virtual_display(hwc_dev, i, displays[i]);
                if (err)
                    ALOGE("Failed to update virtual display %d (%d)", i, err);
            }
        } else {
            if (hwc_dev->displays[i]) {
                remove_display(hwc_dev, i);

                ALOGI("Virtual display %d has been disconnected", i);
            }
        }
    }
}

static int get_layer_stack(omap_hwc_device_t *hwc_dev, int disp, uint32_t *stack)
{
    hwc_layer_stack_t stackInfo = {.dpy = disp};
    void *param = &stackInfo;
    int err = hwc_dev->procs->extension_cb(hwc_dev->procs, HWC_EXTENDED_OP_LAYERSTACK, &param, sizeof(stackInfo));
    if (err)
        return err;

    *stack = stackInfo.stack;

    return 0;
}

static uint32_t get_display_mode(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return DISP_MODE_INVALID;

    if (disp == HWC_DISPLAY_PRIMARY)
        return DISP_MODE_PRESENTATION;

    display_t *display = hwc_dev->displays[disp];

    if (display->type == DISP_TYPE_UNKNOWN)
        return DISP_MODE_INVALID;

    if (!display->contents)
        return DISP_MODE_INVALID;

    if (!(display->contents->flags & HWC_EXTENDED_API) || !hwc_dev->procs || !hwc_dev->procs->extension_cb)
        return DISP_MODE_LEGACY;

    uint32_t primaryStack, stack;
    int err;

    err = get_layer_stack(hwc_dev, HWC_DISPLAY_PRIMARY, &primaryStack);
    if (err)
        return DISP_MODE_INVALID;

    err = get_layer_stack(hwc_dev, disp, &stack);
    if (err)
        return DISP_MODE_INVALID;

    if (stack != primaryStack)
        return DISP_MODE_PRESENTATION;

    return DISP_MODE_LEGACY;
}


void set_display_contents(omap_hwc_device_t *hwc_dev, size_t num_displays, hwc_display_contents_1_t **displays) {
    size_t i;

    if (num_displays > MAX_DISPLAYS)
        num_displays = MAX_DISPLAYS;

    for (i = 0; i < num_displays; i++) {
        if (hwc_dev->displays[i]) {
            display_t *display = hwc_dev->displays[i];
            display->contents = displays[i];

            if (i != HWC_DISPLAY_PRIMARY) {
                uint32_t mode = get_display_mode(hwc_dev, i);

                if (display->mode != mode) {
                    display->mode = mode;
                    display->update_transform = true;
                }
            }
        }
    }

    for ( ; i < MAX_DISPLAYS; i++) {
        if (hwc_dev->displays[i])
            hwc_dev->displays[i]->contents = NULL;
    }

    update_primary_display_orientation(hwc_dev);
}

int get_external_display_id(omap_hwc_device_t *hwc_dev)
{
    size_t i;
    int disp = -1;

    for (i = HWC_DISPLAY_EXTERNAL; i < MAX_DISPLAYS; i++) {
        if (hwc_dev->displays[i] && hwc_dev->displays[i]->type != DISP_TYPE_UNKNOWN) {
            disp = i;
            break;
        }
    }

    return disp;
}

int get_display_configs(omap_hwc_device_t *hwc_dev, int disp, uint32_t *configs, size_t *numConfigs)
{
    if (!numConfigs)
        return -EINVAL;

    if (*numConfigs == 0)
        return 0;

    if (!configs || !is_valid_display(hwc_dev, disp))
        return -EINVAL;

    display_t *display = hwc_dev->displays[disp];
    size_t num = display->num_configs;
    uint32_t c;

    if (num > *numConfigs)
        num = *numConfigs;

    for (c = 0; c < num; c++)
        configs[c] = c;

    *numConfigs = num;

    return 0;
}

int get_display_attributes(omap_hwc_device_t *hwc_dev, int disp, uint32_t cfg, const uint32_t *attributes, int32_t *values)
{
    if (!attributes || !values)
        return 0;

    if (!is_valid_display(hwc_dev, disp))
        return -EINVAL;

    display_t *display = hwc_dev->displays[disp];

    if (cfg >= display->num_configs)
        return -EINVAL;

    const uint32_t *attribute = attributes;
    int32_t *value = values;
    display_config_t *config = &display->configs[cfg];

    while (*attribute != HWC_DISPLAY_NO_ATTRIBUTE) {
        switch (*attribute) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            *value = 1000000000 / config->fps;
            break;
        case HWC_DISPLAY_WIDTH:
            *value = config->xres;
            break;
        case HWC_DISPLAY_HEIGHT:
            *value = config->yres;
            break;
        case HWC_DISPLAY_DPI_X:
            *value = 1000 * config->xdpi;
            break;
        case HWC_DISPLAY_DPI_Y:
            *value = 1000 * config->ydpi;
            break;
        }

        attribute++;
        value++;
    }

    return 0;
}

bool is_valid_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (disp < 0 || disp > MAX_DISPLAY_ID || !hwc_dev->displays[disp])
        return false;

    return true;
}

bool is_supported_display(omap_hwc_device_t *hwc_dev, int disp)
{
    return is_valid_display(hwc_dev, disp) && hwc_dev->displays[disp]->type != DISP_TYPE_UNKNOWN;
}

bool is_active_display(omap_hwc_device_t *hwc_dev, int disp)
{
    return is_valid_display(hwc_dev, disp) && hwc_dev->displays[disp]->contents;
}

bool is_lcd_display(omap_hwc_device_t *hwc_dev, int disp)
{
    return is_valid_display(hwc_dev, disp) && hwc_dev->displays[disp]->type == DISP_TYPE_LCD;
}

bool is_hdmi_display(omap_hwc_device_t *hwc_dev, int disp)
{
    return is_valid_display(hwc_dev, disp) && hwc_dev->displays[disp]->type == DISP_TYPE_HDMI;
}

bool is_wfd_display(omap_hwc_device_t *hwc_dev, int disp)
{
    return is_valid_display(hwc_dev, disp) && hwc_dev->displays[disp]->type == DISP_TYPE_WFD;
}

bool is_external_display_mirroring(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_active_display(hwc_dev, disp))
        return false;

    if (hwc_dev->displays[disp]->mode == DISP_MODE_LEGACY)
        return true;

    return false;
}

int blank_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;

    display_t *display = hwc_dev->displays[disp];
    int err = 0;

    display->blanked = true;

    switch (display->type) {
    case DISP_TYPE_LCD:
    case DISP_TYPE_HDMI:
        if (hwc_dev->fb_fd[disp] >= 0)
            err = ioctl(hwc_dev->fb_fd[disp], FBIOBLANK, FB_BLANK_POWERDOWN);
        else
            err = -ENODEV;
        break;
    case DISP_TYPE_WFD:
        err = capture_black_frame(hwc_dev, disp);
        break;
    default:
        err = -ENODEV;
        break;
    }

    if (err)
        ALOGW("Failed to blank display %d (%d)", disp, err);

    return err;
}

int unblank_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;

    display_t *display = hwc_dev->displays[disp];
    int err = 0;

    display->blanked = false;

    switch (display->type) {
    case DISP_TYPE_LCD:
    case DISP_TYPE_HDMI:
        if (hwc_dev->fb_fd[disp] >= 0)
            err = ioctl(hwc_dev->fb_fd[disp], FBIOBLANK, FB_BLANK_UNBLANK);
        else
            err = -ENODEV;
        break;
    case DISP_TYPE_WFD:
        break;
    default:
        err = -ENODEV;
        break;
    }

    if (err)
        ALOGW("Failed to unblank display %d (%d)", disp, err);

    return err;
}

int disable_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;
    /* We can remove display from composition by changing its type to unknown.
     *
     * HACK: Changing active display type is safe here because the only operation we are
     * going to do on this display is remove. At the moment removing does not depend on
     * display type.
     */
    hwc_dev->displays[disp]->type = DISP_TYPE_UNKNOWN;

    return 0;
}

int setup_display_tranfsorm(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;

    display_t *display = hwc_dev->displays[disp];
    int err = 0;

    if (display->role == DISP_ROLE_PRIMARY)
        set_primary_display_transform_matrix(hwc_dev);
    else
        err = setup_external_display_transform(hwc_dev, disp);

    if (!err)
        display->update_transform = false;

    return err;
}

int apply_display_transform(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;

    display_t *display = hwc_dev->displays[disp];
    display_transform_t *transform = &display->transform;

    if (!transform->rotation && !transform->hflip && !transform->scaling)
        return 0;

    composition_t *comp;
    if (is_external_display_mirroring(hwc_dev, disp))
        comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
    else
        comp = &display->composition;

    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    uint32_t i;

    for (i = 0; i < dsscomp->num_ovls; i++) {
        if (dsscomp->ovls[i].cfg.mgr_ix == display->mgr_ix && dsscomp->ovls[i].cfg.ix != OMAP_DSS_WB)
            adjust_dss_overlay_to_display(hwc_dev, disp, &dsscomp->ovls[i]);
    }

    return 0;
}

int validate_display_composition(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;

    /* Mirrored composition is included in the primary one -- no need to check */
    if (is_external_display_mirroring(hwc_dev, disp))
        return 0;

    return validate_dss_composition(hwc_dev, &hwc_dev->displays[disp]->composition.comp_data.dsscomp_data);
}

void free_displays(omap_hwc_device_t *hwc_dev)
{
    /* Make sure that we don't leak ION memory that might be allocated by external display */
    if (hwc_dev->displays[HWC_DISPLAY_EXTERNAL]) {
        if (is_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL))
            remove_external_hdmi_display(hwc_dev);
    }

    int i;
    for (i = 0; i < MAX_DISPLAYS; i++)
        free_display(hwc_dev->displays[i]);
}
