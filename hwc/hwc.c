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
#include <malloc.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>

#include <cutils/properties.h>
#include <cutils/log.h>
#include <cutils/native_handle.h>
#define HWC_REMOVE_DEPRECATED_VERSIONS 1
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <hardware_legacy/uevent.h>
#include <system/graphics.h>
#include <utils/Timers.h>
#include <EGL/egl.h>
#include <edid_parser.h>
#include <DSSWBHal.h>
#include <linux/omapfb.h>

#include "hwc_dev.h"
#include "color_fmt.h"
#include "blitter.h"
#include "display.h"
#include "dsscomp.h"
#include "dump.h"
#include "sw_vsync.h"
#include "utils.h"

static bool debug = false;
static bool debug_post2 = false;

static void showfps(void)
{
    static int framecount = 0;
    static int lastframecount = 0;
    static nsecs_t lastfpstime = 0;
    static float fps = 0;
    char value[PROPERTY_VALUE_MAX];

    property_get("debug.hwc.showfps", value, "0");
    if (!atoi(value)) {
        return;
    }

    framecount++;
    if (!(framecount & 0x7)) {
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        nsecs_t diff = now - lastfpstime;
        fps = ((framecount - lastframecount) * (float)(s2ns(1))) / diff;
        lastfpstime = now;
        lastframecount = framecount;
        ALOGI("%d Frames, %f FPS", framecount, fps);
    }
}

static uint32_t add_scaling_score(uint32_t score,
                                  uint32_t xres, uint32_t yres, uint32_t refresh,
                                  uint32_t ext_xres, uint32_t ext_yres,
                                  uint32_t mode_xres, uint32_t mode_yres, uint32_t mode_refresh)
{
    uint32_t area = xres * yres;
    uint32_t ext_area = ext_xres * ext_yres;
    uint32_t mode_area = mode_xres * mode_yres;

    /* prefer to upscale (1% tolerance) [0..1] (insert after 1st bit) */
    int upscale = (ext_xres >= xres * 99 / 100 && ext_yres >= yres * 99 / 100);
    score = (((score & ~1) | upscale) << 1) | (score & 1);

    /* pick minimum scaling [0..16] */
    if (ext_area > area)
        score = (score << 5) | (16 * area / ext_area);
    else
        score = (score << 5) | (16 * ext_area / area);

    /* pick smallest leftover area [0..16] */
    score = (score << 5) | ((16 * ext_area + (mode_area >> 1)) / mode_area);

    /* adjust mode refresh rate */
    mode_refresh += mode_refresh % 6 == 5;

    /* prefer same or higher frame rate */
    upscale = (mode_refresh >= refresh);
    score = (score << 1) | upscale;

    /* pick closest frame rate */
    if (mode_refresh > refresh)
        score = (score << 8) | (240 * refresh / mode_refresh);
    else
        score = (score << 8) | (240 * mode_refresh / refresh);

    return score;
}

int set_best_hdmi_mode(omap_hwc_device_t *hwc_dev, int disp, uint32_t xres, uint32_t yres, float xpy)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;

    display_t *display = hwc_dev->displays[disp];
    hdmi_display_t *hdmi = NULL;
    bool avoid_mode_change = true;

    if (display->role == DISP_ROLE_PRIMARY) {
        hdmi = &((primary_hdmi_display_t*)display)->hdmi;
    } else if (display->role == DISP_ROLE_EXTERNAL) {
        hdmi = &((external_hdmi_display_t*)display)->hdmi;
        avoid_mode_change = ((external_hdmi_display_t*)display)->avoid_mode_change;
    } else
        return -ENODEV;

    struct dsscomp_display_info *info = &display->fb_info;
    if (info->timings.x_res * info->timings.y_res == 0 || xres * yres == 0)
        return -EINVAL;

    uint32_t i, best = ~0, best_score = 0;
    uint32_t ext_fb_xres, ext_fb_yres;
    for (i = 0; i < info->modedb_len; i++) {
        uint32_t score = 0;
        struct dsscomp_videomode *mode = &hdmi->mode_db[i];
        uint32_t mode_xres = mode->xres;
        uint32_t mode_yres = mode->yres;
        uint32_t ext_width = info->width_in_mm;
        uint32_t ext_height = info->height_in_mm;

        if (mode->vmode & FB_VMODE_INTERLACED)
            mode_yres /= 2;

        if (mode->flag & FB_FLAG_RATIO_4_3) {
            ext_width = 4;
            ext_height = 3;
        } else if (mode->flag & FB_FLAG_RATIO_16_9) {
            ext_width = 16;
            ext_height = 9;
        }

        if (!mode_xres || !mode_yres)
            continue;

        get_max_dimensions(xres, yres, xpy, mode_xres, mode_yres,
                           ext_width, ext_height, &ext_fb_xres, &ext_fb_yres);

        /* we need to ensure that even TILER2D buffers can be scaled */
        if (!mode->pixclock ||
            (mode->vmode & ~FB_VMODE_INTERLACED) ||
            !can_dss_scale(hwc_dev, xres, yres, ext_fb_xres, ext_fb_yres, true,
                           info, 1000000000 / mode->pixclock))
            continue;

        /* prefer CEA modes */
        if (mode->flag & (FB_FLAG_RATIO_4_3 | FB_FLAG_RATIO_16_9))
            score = 1;

        /* prefer the same mode as we use for mirroring to avoid mode change */
        score = (score << 1) | (i == ~hdmi->video_mode_ix && avoid_mode_change);

        score = add_scaling_score(score, xres, yres, 60, ext_fb_xres, ext_fb_yres,
                                  mode_xres, mode_yres, mode->refresh ? : 1);

        ALOGD("#%d: %dx%d %dHz", i, mode_xres, mode_yres, mode->refresh);
        if (debug)
            ALOGD("  score=0x%x adj.res=%dx%d", score, ext_fb_xres, ext_fb_yres);
        if (best_score < score) {
            hdmi->width = ext_width;
            hdmi->height = ext_height;
            best = i;
            best_score = score;
        }
    }

    if (~best) {
        ALOGD("picking #%d", best);
        /* only reconfigure on change */
        if (hdmi->video_mode_ix != ~best) {
            int err = setup_dsscomp_display(hwc_dev, display->mgr_ix, &hdmi->mode_db[best]);
            if (err)
                return err;

            hdmi->video_mode_ix = ~best;
        }
    } else {
        hdmi->width = info->width_in_mm;
        hdmi->height = info->height_in_mm;

        get_max_dimensions(xres, yres, xpy, info->timings.x_res, info->timings.y_res,
                           hdmi->width, hdmi->height, &ext_fb_xres, &ext_fb_yres);

        if (!info->timings.pixel_clock ||
            !can_dss_scale(hwc_dev, xres, yres, ext_fb_xres, ext_fb_yres, true,
                           info, info->timings.pixel_clock)) {
            ALOGW("DSS scaler cannot support HDMI cloning");
            return -1;
        }
    }

    return 0;
}

static void reserve_overlays_for_displays(omap_hwc_device_t *hwc_dev)
{
    display_t *primary_display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    uint32_t ovl_ix_base = OMAP_DSS_GFX;
    uint32_t max_overlays = MAX_DSS_OVERLAYS;
    uint32_t num_nonscaling_overlays = NUM_NONSCALING_OVERLAYS;

    /* If FB is not same resolution as LCD don't use GFX overlay. */
    if (primary_display->transform.scaling) {
        ovl_ix_base = OMAP_DSS_VIDEO1;
        max_overlays -= num_nonscaling_overlays;
        num_nonscaling_overlays = 0;
    }

    /*
     * We cannot atomically switch overlays from one display to another. First, they
     * have to be disabled, and the disabling has to take effect on the current display.
     * We keep track of the available number of overlays here.
     */
    uint32_t max_primary_overlays = max_overlays - hwc_dev->dsscomp.last_ext_ovls;
    uint32_t max_external_overlays = max_overlays - hwc_dev->dsscomp.last_int_ovls;

    composition_t *primary_comp = &primary_display->composition;

    primary_comp->tiler1d_slot_size = hwc_dev->dsscomp.limits.tiler1d_slot_size;
    primary_comp->ovl_ix_base = ovl_ix_base;
    primary_comp->wanted_ovls = max_overlays;
    primary_comp->avail_ovls = max_primary_overlays;
    primary_comp->scaling_ovls = primary_comp->avail_ovls - num_nonscaling_overlays;
    primary_comp->used_ovls = 0;

    int ext_disp = get_external_display_id(hwc_dev);
    bool mirroring = is_external_display_mirroring(hwc_dev, ext_disp);

    if (hwc_dev->dsscomp.last_ext_ovls || (ext_disp >= 0 && !mirroring)) {
        /* Share available Tiler1D space between primary and external displays. */
        primary_comp->tiler1d_slot_size /= 2;
    }

    if (ext_disp < 0)
        return;

    display_t *ext_display = hwc_dev->displays[ext_disp];

    if (is_wfd_display(hwc_dev, ext_disp)) {
        wfd_display_t *wfd = (wfd_display_t *)ext_display;

        if (mirroring) {
            uint32_t screen_xres = WIDTH(ext_display->transform.region);
            uint32_t screen_yres = HEIGHT(ext_display->transform.region);
            display_config_t *config = &ext_display->configs[ext_display->active_config_ix];

            wfd->wb_mode = decide_dss_wb_capture_mode(config->xres, config->yres, screen_xres, screen_yres);
        } else {
            /* Presentation or legacy docking mode */
            wfd->wb_mode = OMAP_WB_MEM2MEM_MODE;
        }

        if (wfd->wb_mode == OMAP_WB_CAPTURE_MODE)
            return;
    }

    /*
     * For primary display we must reserve at least one overlay for FB, plus an extra
     * overlay for each protected layer.
     */
    layer_statistics_t *primary_layer_stats = &primary_display->layer_stats;
    uint32_t min_primary_overlays = MIN(1 + primary_layer_stats->protected, max_overlays);

    /* Share available overlays between primary and external displays. */
    primary_comp->wanted_ovls = MAX(max_overlays / 2, min_primary_overlays);
    primary_comp->avail_ovls = MIN(max_primary_overlays, primary_comp->wanted_ovls);

    /*
     * We may not have enough overlays on the external display. We "reserve" them here but
     * may not do external composition for the first frame while the overlays required for
     * it are cleared.
     */
    composition_t *ext_comp = &ext_display->composition;

    ext_comp->tiler1d_slot_size = hwc_dev->dsscomp.limits.tiler1d_slot_size - primary_comp->tiler1d_slot_size;
    ext_comp->wanted_ovls = max_overlays - primary_comp->wanted_ovls;
    ext_comp->avail_ovls = MIN(max_external_overlays, ext_comp->wanted_ovls);
    ext_comp->scaling_ovls = ext_comp->avail_ovls;
    ext_comp->used_ovls = 0;
    ext_comp->ovl_ix_base = MAX_DSS_OVERLAYS - ext_comp->avail_ovls;

    if (mirroring) {
        /*
         * If mirroring, we are limited on primary composition by number of available external
         * overlays. We should be able to clone all primary overlays to external. Still we
         * should not go below min_primary_overlays to sustain the primary composition. This
         * may result in some overlays not being cloned to external display.
         */
        if (ext_comp->avail_ovls && primary_comp->avail_ovls > ext_comp->avail_ovls)
            primary_comp->avail_ovls = MAX(min_primary_overlays, ext_comp->avail_ovls);
    }
}

static int clone_dss_overlay(omap_hwc_device_t *hwc_dev, int ix, int ext_disp)
{
    display_t *ext_display = hwc_dev->displays[ext_disp];
    composition_t *ext_comp = &ext_display->composition;
    composition_t *primary_comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &primary_comp->comp_data.dsscomp_data;
    struct dss2_ovl_info *ovl = &dsscomp->ovls[dsscomp->num_ovls];

    if (dsscomp->num_ovls >= MAX_DSS_OVERLAYS) {
        ALOGE("**** cannot clone overlay #%d. using all %d overlays.", ix, dsscomp->num_ovls);
        return -EBUSY;
    }

    memcpy(ovl, dsscomp->ovls + ix, sizeof(*ovl));

    ovl->cfg.ix = ext_comp->ovl_ix_base + ext_comp->used_ovls;
    ovl->cfg.mgr_ix = ext_display->mgr_ix;

    /*
     * Here the assumption is that overlay0 is the one attached to FB.
     * Hence this clone_overlay call is for FB cloning (provided use_sgx is true).
     *
     * For the external displays whose transform is the same as that of primary
     * display, ion_handles would be NULL hence the below logic doesn't execute.
     */
    struct ion_handle *ion_handle = get_external_display_ion_fb_handle(hwc_dev);
    if (ix == 0 && ion_handle && primary_comp->use_sgx) {
        ovl->addressing = OMAP_DSS_BUFADDR_ION;
        ovl->ba = (int)ion_handle;
    } else {
        ovl->addressing = OMAP_DSS_BUFADDR_OVL_IX;
        ovl->ba = ix;
    }

    /* Use distinct z values (to simplify z-order checking) */
    ovl->cfg.zorder += primary_comp->used_ovls;

    dsscomp->num_ovls++;
    ext_comp->used_ovls++;

    return 0;
}

static void setup_framebuffer(omap_hwc_device_t *hwc_dev, int disp, int ovl_ix, int zorder)
{
    display_t *display = hwc_dev->displays[disp];
    composition_t *comp = &display->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    display_config_t *config = &display->configs[display->active_config_ix];
    struct dss2_ovl_info *fb_ovl = &dsscomp->ovls[0];
    uint32_t i;

    setup_dss_overlay(config->xres, config->yres,
                      hwc_dev->fb_dev[disp]->base.format,
                      true,   /* FB is always premultiplied */
                      zorder,
                      fb_ovl);

    fb_ovl->cfg.mgr_ix = hwc_dev->displays[disp]->mgr_ix;
    fb_ovl->cfg.ix = ovl_ix;
    fb_ovl->cfg.pre_mult_alpha = 1;
    fb_ovl->addressing = OMAP_DSS_BUFADDR_LAYER_IX;

    if (comp->use_sgx) {
        /* Add an empty buffer list entry for SGX FB */
        fb_ovl->ba = comp->num_buffers;
        comp->buffers[comp->num_buffers] = NULL;
        comp->num_buffers++;
    } else {
        /*
         * Blitter FB will be inserted in OMAPLFB at position 0. All buffer references in
         * dss2_ovl_info have to be updated to accommodate for that.
         */
        fb_ovl->ba = 0;
        for (i = 1; i < dsscomp->num_ovls; i++)
            dsscomp->ovls[i].ba += 1;
    }
}

/*
 * We're using "implicit" synchronization, so make sure we aren't passing any
 * sync object descriptors around.
 */
static void check_sync_fds_for_display(int disp, hwc_display_contents_1_t *list)
{
    //ALOGD("checking sync FDs");
    if (disp < 0 || disp >= MAX_DISPLAYS || !list)
        return;

    if (list->retireFenceFd >= 0) {
        ALOGW("retireFenceFd[%u] was %d", disp, list->retireFenceFd);
        list->retireFenceFd = -1;
    }

    unsigned int j;
    for (j = 0; j < list->numHwLayers; j++) {
        hwc_layer_1_t* layer = &list->hwLayers[j];
        if (layer->acquireFenceFd >= 0) {
            ALOGW("acquireFenceFd[%u][%u] was %d, closing", disp, j, layer->acquireFenceFd);
            close(layer->acquireFenceFd);
            layer->acquireFenceFd = -1;
        }
        if (layer->releaseFenceFd >= 0) {
            ALOGW("releaseFenceFd[%u][%u] was %d", disp, j, layer->releaseFenceFd);
            layer->releaseFenceFd = -1;
        }
    }
}

static void setup_wb_capture(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    wfd_display_t *wfd = (wfd_display_t *)display;
    display_t *primary_display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];

    composition_t *comp;
    if (is_external_display_mirroring(hwc_dev, disp))
        comp = &primary_display->composition;
    else
        comp = &display->composition;

    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    struct omap_hwc_blit_data *blit_data = &comp->comp_data.blit_data;

    if (!display->blanked)
        wfd->use_wb = wb_capture_layer(&wfd->wb_layer);
    else
        wfd->use_wb = false;

    if (wfd->use_wb) {
        ALOGV("setup_wb_capture: layer is captured, handle = %p", wfd->wb_layer.handle);
        comp->buffers[comp->num_buffers] = wfd->wb_layer.handle;

        struct dss2_ovl_info *ovl = &dsscomp->ovls[dsscomp->num_ovls];
        int mgr_ix = (wfd->wb_mode == OMAP_WB_CAPTURE_MODE) ? primary_display->mgr_ix : display->mgr_ix;

        /* z-order doesn't matter for WB */
        adjust_dss_overlay_to_layer(&wfd->wb_layer, 0, ovl);

        ovl->cfg.mgr_ix = mgr_ix;
        ovl->cfg.ix = OMAP_DSS_WB;
        ovl->addressing = OMAP_DSS_BUFADDR_LAYER_IX;
        ovl->ba = comp->num_buffers + (blit_data->rgz_items > 0 ? 1 : 0);
        ovl->cfg.wb_source = mgr_ix;
        ovl->cfg.wb_mode = wfd->wb_mode;

        if (ovl->cfg.wb_mode == OMAP_WB_MEM2MEM_MODE) {
            /* Video overlays will take care of scaling - no need to scale on WB */
            ovl->cfg.crop = ovl->cfg.win;

            primary_display_t *primary = get_primary_display_info(hwc_dev);

            if (primary) {
                ovl->cfg.rotation = primary->orientation;

                if (primary->orientation & 1) {
                    SWAP(ovl->cfg.crop.x, ovl->cfg.crop.y);
                    SWAP(ovl->cfg.crop.w, ovl->cfg.crop.h);
                }
            }
        }

        dsscomp->mode = DSSCOMP_SETUP_DISPLAY_CAPTURE;
        dsscomp->num_ovls++;
        comp->num_buffers++;

        wfd->wb_sync_id = dsscomp->sync_id;
    } else {
        /*
         * We cannot capture this composition, so we have to disable all overlays
         * configured for M2M mode capture.
         */
        if (wfd->wb_mode == OMAP_WB_MEM2MEM_MODE) {
            int i;
            for (i = 0; i < dsscomp->num_ovls; i++)
                if (dsscomp->ovls[i].cfg.mgr_ix == display->mgr_ix)
                    dsscomp->ovls[i].cfg.enabled = 0;
        }
    }
}

static void mirror_primary_composition(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    composition_t *comp = &display->composition;
    wfd_display_t *wfd = is_wfd_display(hwc_dev, disp) ? (wfd_display_t*)display : NULL;
    hdmi_display_t *hdmi = is_hdmi_display(hwc_dev, disp) ? (hdmi_display_t*)display : NULL;
    hwc_display_contents_1_t *list = display->contents;

    /* Mirror the layers using primary display composition */
    display_t *primary_display = hwc_dev->displays[HWC_DISPLAY_PRIMARY];
    composition_t *primary_comp = &primary_display->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &primary_comp->comp_data.dsscomp_data;
    uint32_t i, ix;

    /* Prevent SurfaceFlinger composition for external display */
    for (i = 0; i < list->numHwLayers; i++) {
        hwc_layer_1_t *layer = &list->hwLayers[i];
        if (layer->compositionType == HWC_FRAMEBUFFER_TARGET)
            continue;

        layer->compositionType = HWC_OVERLAY;
    }

    if (primary_display->blanked)
        return;

    if (wfd && wfd->wb_mode == OMAP_WB_CAPTURE_MODE) {
        setup_wb_capture(hwc_dev, disp);
        return;
    }

    /* If display is blanked or not configured drop compositions */
    if (display->blanked || (hdmi && hdmi->video_mode_ix == 0))
        return;

    /* Mirror all layers */
    for (ix = 0; ix < primary_comp->used_ovls; ix++) {
        if (clone_dss_overlay(hwc_dev, ix, disp))
            break;
    }

    setup_dsscomp_manager(hwc_dev, disp);

    hwc_dev->dsscomp.last_ext_ovls = comp->used_ovls;

    if (wfd && wfd->wb_mode == OMAP_WB_MEM2MEM_MODE)
        setup_wb_capture(hwc_dev, disp);
}

static int hwc_prepare_for_display(omap_hwc_device_t *hwc_dev, int disp)
{
    if (!is_valid_display(hwc_dev, disp))
        return -ENODEV;

    if (!is_supported_display(hwc_dev, disp) || !is_active_display(hwc_dev, disp))
        return 0;

    display_t *display = hwc_dev->displays[disp];
    hdmi_display_t *hdmi = is_hdmi_display(hwc_dev, disp) ? (hdmi_display_t*)display : NULL;
    hwc_display_contents_1_t *list = display->contents;
    composition_t *comp = &display->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    layer_statistics_t *layer_stats = &display->layer_stats;

    if (is_external_display_mirroring(hwc_dev, disp)) {
        mirror_primary_composition(hwc_dev, disp);

        return 0;
    }

    memset(dsscomp, 0x0, sizeof(*dsscomp));
    dsscomp->mode = DSSCOMP_SETUP_DISPLAY;
    dsscomp->sync_id = hwc_dev->dsscomp.sync_id++;
    comp->num_buffers = 0;

    /*
     * The following priorities are used for different compositing HW:
     * 1 - BLITTER (policy = ALL)
     * 2 - DSSCOMP
     * 3 - BLITTER (policy = DEFAULT)
     * 4 - SGX
     */

    /* Check if we can blit everything */
    bool blit_all = (get_blitter_policy(hwc_dev, disp) == BLT_POLICY_ALL) && blit_layers(hwc_dev, list);

    if (blit_all) {
        comp->use_sgx = false;
        comp->swap_rb = false;
    } else  {
        if (can_dss_render_all_layers(hwc_dev, disp)) {
            /* All layers can be handled by the DSS -- don't use SGX for composition */
            comp->use_sgx = false;
            comp->swap_rb = layer_stats->bgr != 0;
        } else {
            /* Use SGX for composition plus first 3 layers that are DSS renderable */
            comp->use_sgx = true;
            /* Only LCD can display BGR layers */
            comp->swap_rb = is_lcd_display(hwc_dev, disp) && is_bgr_format(hwc_dev->fb_dev[disp]->base.format);
        }
    }

    /* setup DSS overlays */
    int z = 0;
    int fb_z = blit_all ? 0 : -1;
    bool scaled_gfx = false;
    uint32_t ovl_ix = comp->ovl_ix_base;
    uint32_t mem1d_used = 0;
    uint32_t i;

    /*
     * If the SGX is used or we are going to blit something we need a framebuffer and an overlay
     * for it. Reserve GFX for FB and begin using VID1 for DSS overlay layers.
     */
    bool needs_fb = comp->use_sgx || blit_all;
    if (needs_fb) {
        dsscomp->num_ovls++;
        ovl_ix++;
    }

    for (i = 0; i < list->numHwLayers && !blit_all; i++) {
        hwc_layer_1_t *layer = &list->hwLayers[i];

        if (dsscomp->num_ovls < comp->avail_ovls &&
            can_dss_render_layer(hwc_dev, disp, layer) &&
            (!hwc_dev->force_sgx ||
            /* render protected layers via DSS */
            is_protected_layer(layer) ||
            is_upscaled_nv12_layer(hwc_dev, layer)) &&
            mem1d_used + get_required_mem1d_size(layer) <= comp->tiler1d_slot_size &&
            /* can't have a transparent overlay in the middle of the framebuffer stack */
            !(is_blended_layer(layer) && fb_z >= 0)) {

            /* render via DSS overlay */
            mem1d_used += get_required_mem1d_size(layer);
            layer->compositionType = HWC_OVERLAY;

            /*
             * This hint will not be used in vanilla ICS, but maybe in JellyBean,
             * it is useful to distinguish between blts and true overlays.
             */
            layer->hints |= HWC_HINT_TRIPLE_BUFFER;

            /* Clear FB above all opaque layers if rendering via SGX */
            if (comp->use_sgx && !is_blended_layer(layer))
                layer->hints |= HWC_HINT_CLEAR_FB;

            comp->buffers[comp->num_buffers] = layer->handle;

            adjust_dss_overlay_to_layer(layer, z, &dsscomp->ovls[dsscomp->num_ovls]);

            dsscomp->ovls[dsscomp->num_ovls].cfg.ix = ovl_ix;
            dsscomp->ovls[dsscomp->num_ovls].cfg.mgr_ix = display->mgr_ix;
            dsscomp->ovls[dsscomp->num_ovls].addressing = OMAP_DSS_BUFADDR_LAYER_IX;
            dsscomp->ovls[dsscomp->num_ovls].ba = comp->num_buffers;

            /* Ensure GFX overlay is never scaled */
            if (ovl_ix == OMAP_DSS_GFX) {
                scaled_gfx = is_scaled_layer(layer) || is_nv12_layer(layer);
            } else if (scaled_gfx && !is_scaled_layer(layer) && !is_nv12_layer(layer)) {
                /* Swap GFX overlay with this one. If GFX is used it's always at index 0. */
                dsscomp->ovls[dsscomp->num_ovls].cfg.ix = dsscomp->ovls[0].cfg.ix;
                dsscomp->ovls[0].cfg.ix = ovl_ix;
                scaled_gfx = false;
            }

            dsscomp->num_ovls++;
            comp->num_buffers++;
            ovl_ix++;
            z++;
        } else if (comp->use_sgx) {
            if (fb_z < 0) {
                /* NOTE: we are not handling transparent cutout for now */
                fb_z = z;
                z++;
            } else {
                /* move fb z-order up (by lowering dss layers) */
                while (fb_z < z - 1)
                    dsscomp->ovls[1 + fb_z++].cfg.zorder--;
            }
        }
    }

    /* If scaling GFX (e.g. only 1 scaled surface) use a VID overlay */
    if (scaled_gfx)
        dsscomp->ovls[0].cfg.ix = ovl_ix;

    if (get_blitter_policy(hwc_dev, disp) == BLT_POLICY_DEFAULT) {
        /*
         * As long as we keep blitting on consecutive frames keep the regionizer
         * state, if this is not possible the regionizer state is unreliable and
         * we need to reset its state.
         */
        if (comp->use_sgx) {
            if (blit_layers(hwc_dev, list)) {
                comp->use_sgx = 0;
            }
        } else
            release_blitter();
    }

    /* If the SGX is not used and there is blit data we need a framebuffer and
     * a DSS pipe well configured for it
     */
    if (needs_fb) {
        /* assign a z-layer for fb */
        if (fb_z < 0) {
            if (layer_stats->count)
                ALOGW("Should have assigned z-layer for fb");
            fb_z = z++;
        }

        setup_framebuffer(hwc_dev, disp, comp->ovl_ix_base, fb_z);
    }

    comp->used_ovls = dsscomp->num_ovls;
    if (disp == HWC_DISPLAY_PRIMARY)
        hwc_dev->dsscomp.last_int_ovls = comp->used_ovls;
    else
        hwc_dev->dsscomp.last_ext_ovls = comp->used_ovls;

    setup_dsscomp_manager(hwc_dev, disp);

    if (is_wfd_display(hwc_dev, disp))
        setup_wb_capture(hwc_dev, disp);

    /* If display is blanked or not configured drop compositions */
    if (display->blanked || (hdmi && hdmi->video_mode_ix == 0))
        dsscomp->num_ovls = 0;

    if (debug)
        dump_prepare_info(hwc_dev, disp);

    return 0;
}

static int hwc_prepare(struct hwc_composer_device_1 *dev, size_t numDisplays,
        hwc_display_contents_1_t** displays)
{
    if (!numDisplays || displays == NULL) {
        return 0;
    }

    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *)dev;
    int err = 0;
    int disp_err;
    uint32_t i;

    pthread_mutex_lock(&hwc_dev->lock);

    detect_virtual_displays(hwc_dev, numDisplays, displays);
    set_display_contents(hwc_dev, numDisplays, displays);

    for (i = 0; i < numDisplays; i++) {
        if (is_active_display(hwc_dev, i)) {
            display_t *display = hwc_dev->displays[i];
            hwc_display_contents_1_t *contents;

            if (display->update_transform) {
                disp_err = setup_display_tranfsorm(hwc_dev, i);
                if (!err && disp_err)
                    err = disp_err;
            }

            if (is_external_display_mirroring(hwc_dev, i))
                contents = hwc_dev->displays[HWC_DISPLAY_PRIMARY]->contents;
            else
                contents = display->contents;

            gather_layer_statistics(hwc_dev, i, contents);
        }
    }

    reserve_overlays_for_displays(hwc_dev);
    reset_blitter(hwc_dev);

    for (i = 0; i < numDisplays; i++) {
        if (displays[i]) {
            disp_err = hwc_prepare_for_display(hwc_dev, i);
            if (!err && disp_err)
                err = disp_err;
        }
    }

    /*
     * The display transform application has to be separated from prepare() loop so that in case
     * of mirroring we clone original overlay configuration. Otherwise cloned overlays will have
     * both primary and external display transform applied, which is not intended.
     */
    for (i = 0; i < numDisplays; i++) {
        if (is_active_display(hwc_dev, i)) {
            disp_err = apply_display_transform(hwc_dev, i);
            if (!err && disp_err)
                err = disp_err;

            disp_err = validate_display_composition(hwc_dev, i);
            if (!err && disp_err)
                err = disp_err;
        }
    }

    pthread_mutex_unlock(&hwc_dev->lock);

    return err;
}

static int hwc_set_for_display(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *list,
        bool *invalidate)
{
    if (!is_valid_display(hwc_dev, disp))
        return list ? -ENODEV : 0;

    if (!is_supported_display(hwc_dev, disp))
        return 0;

    display_t *display = hwc_dev->displays[disp];
    layer_statistics_t *layer_stats = &display->layer_stats;
    composition_t *comp = &display->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    blitter_config_t *blitter = &hwc_dev->blitter;

    if (disp != HWC_DISPLAY_PRIMARY) {
        if (comp->wanted_ovls && (comp->avail_ovls < comp->wanted_ovls) &&
                (layer_stats->protected || !comp->avail_ovls))
            *invalidate = true;
    }

    if (is_external_display_mirroring(hwc_dev, disp))
        return 0;

    hwc_display_t dpy = NULL;
    hwc_surface_t sur = NULL;
    if (list != NULL) {
        dpy = list->dpy;
        sur = list->sur;
    }

    static bool first_set = true;
    if (first_set) {
        reset_primary_display(hwc_dev);
        first_set = false;
    }

    if (debug)
        dump_set_info(hwc_dev, disp, list);

    int err = 0;

    /* The list can be NULL which means HWC is temporarily disabled. However, if dpy and sur
     * are NULL it means we're turning the screen off.
     */
    if (dpy && sur) {
        if (comp->use_sgx) {
            buffer_handle_t framebuffer = NULL;
            if (layer_stats->framebuffer) {
                /* Layer with HWC_FRAMEBUFFER_TARGET should be last in the list. The buffer handle
                 * is updated by SurfaceFlinger after prepare() call, so FB slot has to be updated
                 * in set().
                 */
                framebuffer = list->hwLayers[list->numHwLayers - 1].handle;
            }

            if (framebuffer) {
                comp->buffers[dsscomp->ovls[0].ba] = framebuffer;
            } else {
                ALOGE("set[%d]: No buffer is provided for GL composition", disp);
                return -EFAULT;
            }
        }

        //dump_dsscomp(dsscomp);

        if (debug_post2)
            dump_post2(hwc_dev, disp);

        err = hwc_dev->fb_dev[disp]->Post2((framebuffer_device_t *)hwc_dev->fb_dev[disp],
                             comp->buffers, comp->num_buffers,
                             dsscomp, sizeof(comp->comp_data) + get_blitter_data_size(hwc_dev));

        if (disp == HWC_DISPLAY_PRIMARY)
            showfps();

        int ext_disp = get_external_display_id(hwc_dev);

        if (is_wfd_display(hwc_dev, ext_disp)) {
            display_t *ext_display = hwc_dev->displays[ext_disp];
            if (disp == ext_disp || (disp == HWC_DISPLAY_PRIMARY && ext_display->mode == DISP_MODE_LEGACY)) {
                wfd_display_t *wfd = (wfd_display_t*)ext_display;

                if (wfd->use_wb) {
                    ALOGV("wb capture started, handle = %p, sync_id = %d", wfd->wb_layer.handle, wfd->wb_sync_id);
                    wb_capture_started(wfd->wb_layer.handle, wfd->wb_sync_id);
                }
            }
        }
    }

    if (err) {
        ALOGE("set[%d]: Failed to post composition %08x (%d)", disp, dsscomp->sync_id, err);
        dump_set_info(hwc_dev, disp, list);
        dump_dsscomp(dsscomp);
        dump_post2(hwc_dev, disp);
    }

    check_sync_fds_for_display(disp, list);

    return err;
}


static int hwc_set(struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (!numDisplays || displays == NULL) {
        ALOGD("set: empty display list");
        return 0;
    }

    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t*)dev;

    pthread_mutex_lock(&hwc_dev->lock);

    bool invalidate = false;
    uint32_t i;
    int err = 0;

    for (i = 0; i < numDisplays; i++) {
        int disp_err = hwc_set_for_display(hwc_dev, i, displays[i], &invalidate);
        if (!err && disp_err)
            err = disp_err;
    }

    /* Signal the event thread that a post has happened */
    write(hwc_dev->pipe_fds[1], "s", 1);

    if (hwc_dev->force_sgx > 0)
        hwc_dev->force_sgx--;

    pthread_mutex_unlock(&hwc_dev->lock);

    if (invalidate && hwc_dev->procs && hwc_dev->procs->invalidate)
        hwc_dev->procs->invalidate(hwc_dev->procs);

    return err;
}

static void hwc_dump(struct hwc_composer_device_1 *dev, char *buff, int buff_len)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *)dev;
    struct dump_buf log = {
        .buf = buff,
        .buf_len = buff_len,
    };

    dump_hwc_info(hwc_dev, &log);

    pthread_mutex_lock(&hwc_dev->lock);

    int i;
    for (i = 0; i < MAX_DISPLAYS; i++) {
        if (hwc_dev->displays[i])
            dump_display(hwc_dev, &log, i);
    }

    pthread_mutex_unlock(&hwc_dev->lock);
}

static int hwc_device_close(hw_device_t* device)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) device;;

    if (hwc_dev) {
        close_dsscomp(hwc_dev);

        int i;
        for (i = 0; i < MAX_DISPLAYS; i++) {
            if (hwc_dev->fb_fd[i] >= 0)
                close(hwc_dev->fb_fd[i]);
        }

        /* pthread will get killed when parent process exits */
        pthread_mutex_destroy(&hwc_dev->lock);
        free_displays(hwc_dev);
        free(hwc_dev);
    }

    return 0;
}

static int open_fb_hal(IMG_framebuffer_device_public_t *fb_dev[MAX_DISPLAYS])
{
    const struct hw_module_t *psModule;
    IMG_gralloc_module_public_t *psGrallocModule;
    int err, i;

    err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &psModule);
    psGrallocModule = (IMG_gralloc_module_public_t *) psModule;

    if(err)
        goto err_out;

    if (strcmp(psGrallocModule->base.common.author, "Imagination Technologies")) {
        err = -EINVAL;
        goto err_out;
    }

    for (i = 0; i < MAX_DISPLAYS; i++)
        fb_dev[i] = NULL;

    fb_dev[0] = psGrallocModule->psFbDevice[0];
    fb_dev[1] = psGrallocModule->psFbDevice[1];

    return 0;

err_out:
    ALOGE("Composer HAL failed to load compatible Graphics HAL");
    return err;
}

static void handle_hotplug(omap_hwc_device_t *hwc_dev)
{
    bool state = hwc_dev->ext_disp_state;
    bool hotplug = false;

    pthread_mutex_lock(&hwc_dev->lock);

    if (is_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY)) {
        ALOGI("Primary HDMI display is %splugged", state ? "" : "un");

        if (state) {
            configure_primary_hdmi_display(hwc_dev);
        } else {
            hdmi_display_t *hdmi = (hdmi_display_t *)hwc_dev->displays[HWC_DISPLAY_PRIMARY];
            hdmi->video_mode_ix = 0;
        }
    } else {
        if (state) {
            int err = add_external_hdmi_display(hwc_dev);
            if (err) {
                remove_external_hdmi_display(hwc_dev);
                pthread_mutex_unlock(&hwc_dev->lock);
                return;
            }
        } else
            remove_external_hdmi_display(hwc_dev);

        display_t *ext_display = hwc_dev->displays[HWC_DISPLAY_EXTERNAL];
        ALOGI("external display changed (state=%d, mirror={%s tform=%ddeg%s}, tv=%d", state,
             is_external_display_mirroring(hwc_dev, HWC_DISPLAY_EXTERNAL) ? "mirror enabled" : "mirror disabled",
             ext_display ? ext_display->transform.rotation * 90 : -1,
             ext_display ? ext_display->transform.hflip ? "+hflip" : "" : "",
             is_hdmi_display(hwc_dev, HWC_DISPLAY_EXTERNAL));

        hotplug = true;
    }

    pthread_mutex_unlock(&hwc_dev->lock);

    /* hwc_dev->procs is set right after the device is opened, but there is
     * still a race condition where a hotplug event might occur after the open
     * but before the procs are registered. */
    if (hwc_dev->procs) {
        if (hotplug && hwc_dev->procs->hotplug) {
            hwc_dev->procs->hotplug(hwc_dev->procs, HWC_DISPLAY_EXTERNAL, state);
        } else {
            if (hwc_dev->procs->invalidate)
                hwc_dev->procs->invalidate(hwc_dev->procs);
        }
    }
}

static void handle_uevents(omap_hwc_device_t *hwc_dev, const char *buff, int len)
{
    int hdmi;
    int vsync;
    int state = 0;
    uint64_t timestamp = 0;
    const char *s = buff;

    hdmi = !strcmp(s, "change@/devices/virtual/switch/hdmi");
    vsync = !strcmp(s, "change@/devices/platform/omapfb") ||
        !strcmp(s, "change@/devices/virtual/switch/omapfb-vsync");

    if (!vsync && !hdmi)
       return;

    s += strlen(s) + 1;

    while(*s) {
        if (!strncmp(s, "SWITCH_STATE=", strlen("SWITCH_STATE=")))
            state = atoi(s + strlen("SWITCH_STATE="));
        else if (!strncmp(s, "SWITCH_TIME=", strlen("SWITCH_TIME=")))
            timestamp = strtoull(s + strlen("SWITCH_TIME="), NULL, 0);
        else if (!strncmp(s, "VSYNC=", strlen("VSYNC=")))
            timestamp = strtoull(s + strlen("VSYNC="), NULL, 0);

        s += strlen(s) + 1;
        if (s - buff >= len)
            break;
    }

    if (vsync) {
        if (hwc_dev->procs)
            hwc_dev->procs->vsync(hwc_dev->procs, 0, timestamp);
    } else {
        hwc_dev->ext_disp_state = state == 1;
        handle_hotplug(hwc_dev);
    }
}

static void *hdmi_thread(void *data)
{
    omap_hwc_device_t *hwc_dev = data;
    static char uevent_desc[4096];
    struct pollfd fds[2];
    bool invalidate = false;
    int timeout;
    int err;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    uevent_init();

    fds[0].fd = uevent_get_fd();
    fds[0].events = POLLIN;
    fds[1].fd = hwc_dev->pipe_fds[0];
    fds[1].events = POLLIN;

    timeout = hwc_dev->idle ? hwc_dev->idle : -1;

    memset(uevent_desc, 0, sizeof(uevent_desc));

    do {
        err = poll(fds, hwc_dev->idle ? 2 : 1, timeout);

        if (err == 0) {
            if (hwc_dev->idle) {
                if (hwc_dev->procs) {
                    pthread_mutex_lock(&hwc_dev->lock);
                    invalidate = hwc_dev->dsscomp.last_int_ovls > 1 && !hwc_dev->force_sgx;
                    if (invalidate) {
                        hwc_dev->force_sgx = 2;
                    }
                    pthread_mutex_unlock(&hwc_dev->lock);

                    if (invalidate) {
                        hwc_dev->procs->invalidate(hwc_dev->procs);
                        timeout = -1;
                    }
                }

                continue;
            }
        }

        if (err == -1) {
            if (errno != EINTR)
                ALOGE("event error: %m");
            continue;
        }

        if (hwc_dev->idle && fds[1].revents & POLLIN) {
            char c;
            read(hwc_dev->pipe_fds[0], &c, 1);
            if (!hwc_dev->force_sgx)
                timeout = hwc_dev->idle ? hwc_dev->idle : -1;
        }

        if (fds[0].revents & POLLIN) {
            /* keep last 2 zeroes to ensure double 0 termination */
            int len = uevent_next_event(uevent_desc, sizeof(uevent_desc) - 2);
            handle_uevents(hwc_dev, uevent_desc, len);
        }
    } while (1);

    return NULL;
}

static void hwc_registerProcs(struct hwc_composer_device_1* dev,
                                    hwc_procs_t const* procs)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) dev;

    hwc_dev->procs = (typeof(hwc_dev->procs)) procs;
}

static int hwc_query(struct hwc_composer_device_1* dev, int what, int* value)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) dev;

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // we don't support the background layer yet
        value[0] = 0;
        break;
    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = 1000000000.0 / hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.fps;
        break;
    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static int hwc_eventControl(struct hwc_composer_device_1* dev,
        int dpy, int event, int enabled)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) dev;

    switch (event) {
    case HWC_EVENT_VSYNC:
    {
        int val = !!enabled;
        int err;

        primary_display_t *primary = get_primary_display_info(hwc_dev);
        if (!primary)
            return -ENODEV;

        if (primary->use_sw_vsync) {
            if (enabled)
                start_sw_vsync(hwc_dev);
            else
                stop_sw_vsync();
            return 0;
        }

        err = ioctl(hwc_dev->fb_fd[HWC_DISPLAY_PRIMARY], OMAPFB_ENABLEVSYNC, &val);
        if (err < 0)
            return -errno;

        return 0;
    }
    default:
        return -EINVAL;
    }
}

static int hwc_blank(struct hwc_composer_device_1 *dev, int disp, int blank)
{
    omap_hwc_device_t *hwc_dev = (omap_hwc_device_t *) dev;

    pthread_mutex_lock(&hwc_dev->lock);

    if (!is_valid_display(hwc_dev, disp)) {
        pthread_mutex_unlock(&hwc_dev->lock);
        return -ENODEV;
    }

    /*
     * We're using an older method of screen blanking based on early_suspend in the kernel.
     * No need to do anything here except updating the display state.
     */
    display_t *display = hwc_dev->displays[disp];
    int err = 0;

    display->blanked = blank;

    if (disp == HWC_DISPLAY_PRIMARY) {
        int ext_disp = get_external_display_id(hwc_dev);
        if (is_wfd_display(hwc_dev, ext_disp)) {
            /*
             * SurfaceFlinger doesn't issues blanking commands for virtual displays. In order to
             * simulate blanking of WFD display we need to call blank_display() explicitly.
             */
            if (blank)
                err = blank_display(hwc_dev, ext_disp);
            else
                err = unblank_display(hwc_dev, ext_disp);
        }
    }

    pthread_mutex_unlock(&hwc_dev->lock);

    return err;
}

static int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp, uint32_t* configs, size_t* numConfigs)
{
    return get_display_configs((omap_hwc_device_t *)dev, disp, configs, numConfigs);
}

static int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
                                    uint32_t config, const uint32_t* attributes, int32_t* values)
{
    return get_display_attributes((omap_hwc_device_t *)dev, disp, config, attributes, values);
}

static int hwc_device_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    omap_hwc_module_t *hwc_mod = (omap_hwc_module_t *)module;
    omap_hwc_device_t *hwc_dev;
    int err = 0, i;

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        return -EINVAL;
    }

    if (!hwc_mod->fb_dev[HWC_DISPLAY_PRIMARY]) {
        err = open_fb_hal(hwc_mod->fb_dev);
        if (err)
            return err;

        if (!hwc_mod->fb_dev[HWC_DISPLAY_PRIMARY]) {
            ALOGE("Framebuffer HAL not opened before HWC");
            return -EFAULT;
        }
        hwc_mod->fb_dev[HWC_DISPLAY_PRIMARY]->bBypassPost = 1;
    }

    hwc_dev = (omap_hwc_device_t *)malloc(sizeof(*hwc_dev));
    if (hwc_dev == NULL)
        return -ENOMEM;

    memset(hwc_dev, 0, sizeof(*hwc_dev));

    hwc_dev->base.common.tag = HARDWARE_DEVICE_TAG;
    hwc_dev->base.common.version = HWC_DEVICE_API_VERSION_1_1;

    hwc_dev->base.common.module = (hw_module_t *)module;
    hwc_dev->base.common.close = hwc_device_close;
    hwc_dev->base.prepare = hwc_prepare;
    hwc_dev->base.set = hwc_set;
    hwc_dev->base.eventControl = hwc_eventControl;
    hwc_dev->base.blank = hwc_blank;
    hwc_dev->base.dump = hwc_dump;
    hwc_dev->base.registerProcs = hwc_registerProcs;
    hwc_dev->base.getDisplayConfigs = hwc_getDisplayConfigs;
    hwc_dev->base.getDisplayAttributes = hwc_getDisplayAttributes;
    hwc_dev->base.query = hwc_query;

    for (i = 0; i < MAX_DISPLAYS; i++) {
        hwc_dev->fb_dev[i] = hwc_mod->fb_dev[i];
        hwc_dev->fb_fd[i] = -EINVAL;
    }

    *device = &hwc_dev->base.common;

    err = init_dsscomp(hwc_dev);
    if (err)
        goto done;

    hwc_dev->fb_fd[HWC_DISPLAY_PRIMARY] = open("/dev/graphics/fb0", O_RDWR);
    if (hwc_dev->fb_fd[HWC_DISPLAY_PRIMARY] < 0) {
        ALOGE("failed to open fb (%d)", errno);
        err = -errno;
        goto done;
    }

    err = init_primary_display(hwc_dev);
    if (err)
        goto done;

    if (!is_hdmi_display(hwc_dev, HWC_DISPLAY_PRIMARY)) {
        hwc_dev->fb_fd[HWC_DISPLAY_EXTERNAL] = open("/dev/graphics/fb1", O_RDWR);
        if (hwc_dev->fb_fd[HWC_DISPLAY_EXTERNAL] < 0) {
            ALOGE("failed to open hdmi fb (%d)", errno);
            err = -errno;
            goto done;
        }
    }

    if (pipe(hwc_dev->pipe_fds) == -1) {
        ALOGE("failed to event pipe (%d): %m", errno);
        err = -errno;
        goto done;
    }

    if (pthread_mutex_init(&hwc_dev->lock, NULL)) {
        ALOGE("failed to create mutex (%d): %m", errno);
        err = -errno;
        goto done;
    }

    if (pthread_create(&hwc_dev->hdmi_thread, NULL, hdmi_thread, hwc_dev)) {
        ALOGE("failed to create HDMI listening thread (%d): %m", errno);
        err = -errno;
        goto done;
    }

    /* get debug properties */
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.hwc.rgb_order", value, "1");
    hwc_dev->flags_rgb_order = atoi(value);
    property_get("debug.hwc.nv12_only", value, "0");
    hwc_dev->flags_nv12_only = atoi(value);
    property_get("debug.hwc.idle", value, "250");
    hwc_dev->idle = atoi(value);

    /* read switch state */
    int sw_fd = open("/sys/class/switch/hdmi/state", O_RDONLY);
    if (sw_fd >= 0) {
        char value;
        if (read(sw_fd, &value, 1) == 1)
            hwc_dev->ext_disp_state = value == '1';
        close(sw_fd);
    }

    handle_hotplug(hwc_dev);

    ALOGI("open_device(rgb_order=%d nv12_only=%d)",
        hwc_dev->flags_rgb_order, hwc_dev->flags_nv12_only);

    err = init_blitter(hwc_dev);
    if (err)
        goto done;

    err = wb_open();
    if (err)
        goto done;

    property_get("persist.hwc.upscaled_nv12_limit", value, "2.");
    sscanf(value, "%f", &hwc_dev->upscaled_nv12_limit);
    if (hwc_dev->upscaled_nv12_limit < 0. || hwc_dev->upscaled_nv12_limit > 2048.) {
        ALOGW("Invalid upscaled_nv12_limit (%s), setting to 2.", value);
        hwc_dev->upscaled_nv12_limit = 2.;
    }

done:
    if (err && hwc_dev)
        hwc_device_close((hw_device_t*)hwc_dev);

    return err;
}

static struct hw_module_methods_t module_methods = {
    .open = hwc_device_open,
};

omap_hwc_module_t HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag =                  HARDWARE_MODULE_TAG,
            .module_api_version =   HWC_MODULE_API_VERSION_0_1,
            .hal_api_version =      HARDWARE_HAL_API_VERSION,
            .id =                   HWC_HARDWARE_MODULE_ID,
            .name =                 "OMAP 44xx Hardware Composer HAL",
            .author =               "Texas Instruments",
            .methods =              &module_methods,
        },
    },
};
