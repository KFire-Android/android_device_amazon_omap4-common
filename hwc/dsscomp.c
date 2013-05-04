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
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <cutils/log.h>

#include <video/dsscomp.h>

#include "hwc_dev.h"
#include "color_fmt.h"
#include "display.h"
#include "dsscomp.h"
#include "layer.h"
#include "utils.h"

#define MAX_MODE_DB_LENGTH 32

#define WB_CAPTURE_MAX_UPSCALE 1.0f
#define WB_CAPTURE_MAX_DOWNSCALE 0.5f

/*
 * This tolerance threshold controls decision of whether to use WB in CAPTURE or in MEM2MEM mode
 * when setting up primary display mirroring.
 */
#define WB_ASPECT_RATIO_TOLERANCE 0.15f

static void append_manager(struct dsscomp_setup_dispc_data *dsscomp, int mgr_ix, bool swap_rb)
{
    dsscomp->mgrs[dsscomp->num_mgrs].ix = mgr_ix;
    dsscomp->mgrs[dsscomp->num_mgrs].alpha_blending = 1;
    dsscomp->mgrs[dsscomp->num_mgrs].swap_rb = swap_rb;
    dsscomp->num_mgrs++;
}

int init_dsscomp(omap_hwc_device_t *hwc_dev)
{
    dsscomp_state_t *dsscomp = &hwc_dev->dsscomp;
    int err = 0;

    dsscomp->fd = open("/dev/dsscomp", O_RDWR);
    if (dsscomp->fd < 0) {
        ALOGE("Failed to open dsscomp (%d)", errno);
        return -errno;
    }

    int ret = ioctl(dsscomp->fd, DSSCIOC_QUERY_PLATFORM, &dsscomp->limits);
    if (ret) {
        ALOGE("Failed to get platform limits (%d)", errno);
        close_dsscomp(hwc_dev);
        return -errno;
    }

    return err;
}

void close_dsscomp(omap_hwc_device_t *hwc_dev)
{
    if (hwc_dev->dsscomp.fd >= 0) {
        close(hwc_dev->dsscomp.fd);
        hwc_dev->dsscomp.fd = 0;
    }
}

int get_dsscomp_display_info(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_display_info *info)
{
    memset(info, 0, sizeof(*info));

    info->ix = mgr_ix;

    int ret = ioctl(hwc_dev->dsscomp.fd, DSSCIOC_QUERY_DISPLAY, info);
    if (ret) {
        ALOGE("Failed to get display %d info (%d)", mgr_ix, errno);
        return -errno;
    }

    return 0;
}

int get_dsscomp_display_mode_db(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_videomode *mode_db, uint32_t *mode_db_len)
{
    struct query {
        struct dsscomp_display_info info;
        struct dsscomp_videomode modedb[MAX_MODE_DB_LENGTH];
    } query;

    memset(&query, 0, sizeof(query));

    query.info.ix = mgr_ix;
    query.info.modedb_len = *mode_db_len;

    if (query.info.modedb_len > MAX_MODE_DB_LENGTH)
        query.info.modedb_len = MAX_MODE_DB_LENGTH;

    int ret = ioctl(hwc_dev->dsscomp.fd, DSSCIOC_QUERY_DISPLAY, &query.info);
    if (ret) {
        ALOGE("Failed to get display %d mode database (%d)", mgr_ix, errno);
        return -errno;
    }

    memcpy(mode_db, query.modedb, sizeof(query.modedb[0]) * query.info.modedb_len);

    *mode_db_len = query.info.modedb_len;

    return 0;
}

int setup_dsscomp_display(omap_hwc_device_t *hwc_dev, int mgr_ix, struct dsscomp_videomode *mode)
{
    struct dsscomp_setup_display_data data;

    memset(&data, 0, sizeof(data));

    data.ix = mgr_ix;
    data.mode = *mode;

    int ret = ioctl(hwc_dev->dsscomp.fd, DSSCIOC_SETUP_DISPLAY, &data);
    if (ret) {
        ALOGE("Failed to setup display %d (%d)", mgr_ix, errno);
        return -errno;
    }

    return 0;
}

void setup_dsscomp_manager(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    composition_t *comp = &display->composition;
    struct dsscomp_setup_dispc_data *dsscomp;

    if (is_external_display_mirroring(hwc_dev, disp))
        /* When mirroring add second manager to the primary display composition */
        dsscomp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition.comp_data.dsscomp_data;
    else
        dsscomp = &comp->comp_data.dsscomp_data;

    append_manager(dsscomp, display->mgr_ix, comp->swap_rb);

    if (hwc_dev->dsscomp.last_ext_ovls && get_external_display_id(hwc_dev) < 0) {
        append_manager(dsscomp, 1, false);
        hwc_dev->dsscomp.last_ext_ovls = 0;
    }
}

bool can_dss_scale(omap_hwc_device_t *hwc_dev, uint32_t src_w, uint32_t src_h,
                   uint32_t dst_w, uint32_t dst_h, bool is_2d,
                   struct dsscomp_display_info *dis, uint32_t pclk)
{
    struct dsscomp_platform_info *limits = &hwc_dev->dsscomp.limits;
    uint32_t fclk = limits->fclk / 1000;
    uint32_t min_src_w = DIV_ROUND_UP(src_w, is_2d ? limits->max_xdecim_2d : limits->max_xdecim_1d);
    uint32_t min_src_h = DIV_ROUND_UP(src_h, is_2d ? limits->max_ydecim_2d : limits->max_ydecim_1d);

    /* ERRATAs */
    /* cannot render 1-width layers on DSI video mode panels - we just disallow all 1-width LCD layers */
    if (dis->channel != OMAP_DSS_CHANNEL_DIGIT && dst_w < limits->min_width)
        return false;

    /* NOTE: no support for checking YUV422 layers that are tricky to scale */

    /* FIXME: limit vertical downscale well below theoretical limit as we saw display artifacts */
    if (dst_h < src_h / 4)
        return false;

    /* max downscale */
    if (dst_h * limits->max_downscale < min_src_h)
        return false;

    /* for manual panels pclk is 0, and there are no pclk based scaling limits */
    if (!pclk)
        return !(dst_w < src_w / limits->max_downscale / (is_2d ? limits->max_xdecim_2d : limits->max_xdecim_1d));

    /* :HACK: limit horizontal downscale well below theoretical limit as we saw display artifacts */
    if (dst_w * 4 < src_w)
        return false;

    /* max horizontal downscale is 4, or the fclk/pixclk */
    if (fclk > pclk * limits->max_downscale)
        fclk = pclk * limits->max_downscale;

    /* for small parts, we need to use integer fclk/pixclk */
    if (src_w < limits->integer_scale_ratio_limit)
        fclk = fclk / pclk * pclk;

    if ((uint32_t) dst_w * fclk < min_src_w * pclk)
        return false;

    return true;
}

bool can_dss_render_all_layers(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    layer_statistics_t *layer_stats = &display->layer_stats;
    composition_t *comp = &display->composition;
    bool support_bgr = is_lcd_display(hwc_dev, disp);
    bool tform = false;

    int ext_disp = (disp == HWC_DISPLAY_PRIMARY) ? get_external_display_id(hwc_dev) : disp;
    if (is_external_display_mirroring(hwc_dev, ext_disp)) {
        display_t *ext_display = hwc_dev->displays[ext_disp];
        uint32_t ext_composable_mask = ext_display->layer_stats.composable_mask;

        /*
         * Make sure that all layers that are composable on the primary display are also
         * composable on the external.
         */
        if ((layer_stats->composable_mask & ext_composable_mask) != layer_stats->composable_mask)
            return false;

        /* Make sure that all displays that are going to show the composition support BGR input */
        if (support_bgr) {
            int clone = (disp == HWC_DISPLAY_PRIMARY) ? ext_disp : HWC_DISPLAY_PRIMARY;
            support_bgr = is_lcd_display(hwc_dev, clone);
        }

        tform = ext_display->transform.rotation || ext_display->transform.hflip;
    }

    return  !hwc_dev->force_sgx &&
            /* Must have at least one layer if using composition bypass to get sync object */
            layer_stats->composable &&
            layer_stats->composable <= comp->avail_ovls &&
            layer_stats->composable == layer_stats->count &&
            layer_stats->scaled <= comp->scaling_ovls &&
            layer_stats->nv12 <= comp->scaling_ovls &&
            /* Fits into TILER slot */
            layer_stats->mem1d_total <= comp->tiler1d_slot_size &&
            /* We cannot clone non-NV12 transformed layers */
            (!tform || (layer_stats->nv12 == layer_stats->composable)) &&
            /* Only LCD can display BGR */
            (layer_stats->bgr == 0 || (layer_stats->rgb == 0 && support_bgr) || !hwc_dev->flags_rgb_order) &&
            /* If nv12_only flag is set DSS should only render NV12 */
            (!hwc_dev->flags_nv12_only || (layer_stats->bgr == 0 && layer_stats->rgb == 0));
}

bool can_dss_render_layer(omap_hwc_device_t *hwc_dev, int disp, hwc_layer_1_t *layer)
{
    display_t *display = hwc_dev->displays[disp];
    composition_t *comp = &display->composition;
    bool tform = false;

    int ext_disp = (disp == HWC_DISPLAY_PRIMARY) ? get_external_display_id(hwc_dev) : disp;
    if (is_external_display_mirroring(hwc_dev, ext_disp)) {
        display_t *ext_display = hwc_dev->displays[ext_disp];

        if (!is_composable_layer(hwc_dev, ext_disp, layer))
            return false;

        tform = ext_display->transform.rotation || ext_display->transform.hflip;
    }

    return is_composable_layer(hwc_dev, disp, layer) &&
           /* Cannot rotate non-NV12 layers on external display */
           (!tform || is_nv12_layer(layer)) &&
           /* Skip non-NV12 layers if also using SGX (if nv12_only flag is set) */
           (!hwc_dev->flags_nv12_only || (!comp->use_sgx || is_nv12_layer(layer))) &&
           /* Make sure RGB ordering is consistent (if rgb_order flag is set) */
           (!(comp->swap_rb ? is_rgb_layer(layer) : is_bgr_layer(layer)) || !hwc_dev->flags_rgb_order);
}

uint32_t decide_dss_wb_capture_mode(uint32_t src_xres, uint32_t src_yres, uint32_t dst_xres, uint32_t dst_yres)
{
    uint32_t wb_mode = OMAP_WB_CAPTURE_MODE;
    float x_scale_factor = (float)src_xres / dst_xres;
    float y_scale_factor = (float)src_yres / dst_yres;

    if (x_scale_factor > WB_CAPTURE_MAX_UPSCALE || y_scale_factor > WB_CAPTURE_MAX_UPSCALE ||
        x_scale_factor < WB_CAPTURE_MAX_DOWNSCALE || y_scale_factor < WB_CAPTURE_MAX_DOWNSCALE ||
        x_scale_factor < y_scale_factor * (1.f - WB_ASPECT_RATIO_TOLERANCE) ||
        x_scale_factor * (1.f - WB_ASPECT_RATIO_TOLERANCE) > y_scale_factor) {
        wb_mode = OMAP_WB_MEM2MEM_MODE;
    }

    // HACK: Force MEM2MEM mode until switching between MEM2MEM and CAPTURE is properly supported
    wb_mode = OMAP_WB_MEM2MEM_MODE;

    return wb_mode;
}

void setup_dss_overlay(int width, int height, uint32_t format, bool blended, int zorder, struct dss2_ovl_info *ovl)
{
    struct dss2_ovl_cfg *oc = &ovl->cfg;
    /* YUV2RGB conversion */
    const struct omap_dss_cconv_coefs ctbl_bt601_5 = {
        298,  409,    0,  298, -208, -100,  298,    0,  517, 0,
    };

    /* convert color format */
    oc->color_mode = convert_hal_to_dss_format(format, blended);

    if (oc->color_mode == OMAP_DSS_COLOR_NV12)
        oc->cconv = ctbl_bt601_5;

    oc->width = width;
    oc->height = height;
    oc->stride = get_stride_from_format(format, width);

    oc->enabled = 1;
    oc->global_alpha = 255;
    oc->zorder = zorder;
    oc->ix = 0;

    /* defaults for SGX framebuffer renders */
    oc->crop.w = oc->win.w = width;
    oc->crop.h = oc->win.h = height;

    /* for now interlacing and vc1 info is not supplied */
    oc->ilace = OMAP_DSS_ILACE_NONE;
    oc->vc1.enable = 0;
}

void adjust_dss_overlay_to_layer(hwc_layer_1_t const *layer, int zorder, struct dss2_ovl_info *ovl)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;
    struct dss2_ovl_cfg *oc = &ovl->cfg;

    //dump_layer(layer);

    setup_dss_overlay(handle->iWidth, handle->iHeight, handle->iFormat, is_blended_layer(layer), zorder, ovl);

    /* convert transformation - assuming 0-set config */
    if (layer->transform & HWC_TRANSFORM_FLIP_H)
        oc->mirror = 1;
    if (layer->transform & HWC_TRANSFORM_FLIP_V) {
        oc->rotation = 2;
        oc->mirror = !oc->mirror;
    }
    if (layer->transform & HWC_TRANSFORM_ROT_90) {
        oc->rotation += oc->mirror ? -1 : 1;
        oc->rotation &= 3;
    }

    oc->pre_mult_alpha = layer->blending == HWC_BLENDING_PREMULT;

    /* display position */
    oc->win.x = layer->displayFrame.left;
    oc->win.y = layer->displayFrame.top;
    oc->win.w = WIDTH(layer->displayFrame);
    oc->win.h = HEIGHT(layer->displayFrame);

    /* crop */
    oc->crop.x = layer->sourceCrop.left;
    oc->crop.y = layer->sourceCrop.top;
    oc->crop.w = WIDTH(layer->sourceCrop);
    oc->crop.h = HEIGHT(layer->sourceCrop);
}

static int crop_overlay_to_rect(struct hwc_rect vis_rect, struct dss2_ovl_info *ovl)
{
    struct dss2_ovl_cfg *oc = &ovl->cfg;

    struct {
        int xy[2];
        int wh[2];
    } crop, win;
    struct {
        int lt[2];
        int rb[2];
    } vis;
    win.xy[0] = oc->win.x; win.xy[1] = oc->win.y;
    win.wh[0] = oc->win.w; win.wh[1] = oc->win.h;
    crop.xy[0] = oc->crop.x; crop.xy[1] = oc->crop.y;
    crop.wh[0] = oc->crop.w; crop.wh[1] = oc->crop.h;
    vis.lt[0] = vis_rect.left; vis.lt[1] = vis_rect.top;
    vis.rb[0] = vis_rect.right; vis.rb[1] = vis_rect.bottom;

    int c;
    bool swap = oc->rotation & 1;

    /* align crop window with display coordinates */
    if (swap)
        crop.xy[1] -= (crop.wh[1] = -crop.wh[1]);
    if (oc->rotation & 2)
        crop.xy[!swap] -= (crop.wh[!swap] = -crop.wh[!swap]);
    if ((!oc->mirror) ^ !(oc->rotation & 2))
        crop.xy[swap] -= (crop.wh[swap] = -crop.wh[swap]);

    for (c = 0; c < 2; c++) {
        /* see if complete buffer is outside the vis or it is
          fully cropped or scaled to 0 */
        if (win.wh[c] <= 0 || vis.rb[c] <= vis.lt[c] ||
            win.xy[c] + win.wh[c] <= vis.lt[c] ||
            win.xy[c] >= vis.rb[c] ||
            !crop.wh[c ^ swap])
            return -ENOENT;

        /* crop left/top */
        if (win.xy[c] < vis.lt[c]) {
            /* correction term */
            int a = (vis.lt[c] - win.xy[c]) * crop.wh[c ^ swap] / win.wh[c];
            crop.xy[c ^ swap] += a;
            crop.wh[c ^ swap] -= a;
            win.wh[c] -= vis.lt[c] - win.xy[c];
            win.xy[c] = vis.lt[c];
        }
        /* crop right/bottom */
        if (win.xy[c] + win.wh[c] > vis.rb[c]) {
            crop.wh[c ^ swap] = crop.wh[c ^ swap] * (vis.rb[c] - win.xy[c]) / win.wh[c];
            win.wh[c] = vis.rb[c] - win.xy[c];
        }

        if (!crop.wh[c ^ swap] || !win.wh[c])
            return -ENOENT;
    }

    /* realign crop window to buffer coordinates */
    if (oc->rotation & 2)
        crop.xy[!swap] -= (crop.wh[!swap] = -crop.wh[!swap]);
    if ((!oc->mirror) ^ !(oc->rotation & 2))
        crop.xy[swap] -= (crop.wh[swap] = -crop.wh[swap]);
    if (swap)
        crop.xy[1] -= (crop.wh[1] = -crop.wh[1]);

    oc->win.x = win.xy[0]; oc->win.y = win.xy[1];
    oc->win.w = win.wh[0]; oc->win.h = win.wh[1];
    oc->crop.x = crop.xy[0]; oc->crop.y = crop.xy[1];
    oc->crop.w = crop.wh[0]; oc->crop.h = crop.wh[1];

    return 0;
}

void adjust_dss_overlay_to_display(omap_hwc_device_t *hwc_dev, int disp, struct dss2_ovl_info *ovl)
{
    struct dss2_ovl_cfg *oc = &ovl->cfg;
    display_t *display = hwc_dev->displays[disp];
    if (!display)
        return;

    if (crop_overlay_to_rect(display->transform.region, ovl) != 0) {
        oc->enabled = 0;
        return;
    }

    hwc_rect_t win = {oc->win.x, oc->win.y, oc->win.x + oc->win.w, oc->win.y + oc->win.h};

    transform_rect(display->transform.matrix, &win);

    oc->win.x = win.left;
    oc->win.y = win.top;
    oc->win.w = WIDTH(win);
    oc->win.h = HEIGHT(win);

    /* combining transformations: F^a*R^b*F^i*R^j = F^(a+b)*R^(j+b*(-1)^i), because F*R = R^(-1)*F */
    oc->rotation += (oc->mirror ? -1 : 1) * display->transform.rotation;
    oc->rotation &= 3;
    if (display->transform.hflip)
        oc->mirror = !oc->mirror;
}

int validate_dss_composition(omap_hwc_device_t *hwc_dev, struct dsscomp_setup_dispc_data *dsscomp)
{
    /* One extra overlay may be used by DSS WB */
    if (dsscomp->num_ovls > MAX_DSS_OVERLAYS + 1) {
        ALOGE("Used too many overlays (%d)", dsscomp->num_ovls);
        return -ERANGE;
    }

    uint32_t max_z = 0;
    uint32_t z = 0;
    uint32_t ix = 0;
    uint32_t i;
    bool use_wb = false;

    /* Verify all z-orders and overlay indices are distinct */
    for (i = 0; i < dsscomp->num_ovls; i++) {
        struct dss2_ovl_cfg *oc = &dsscomp->ovls[i].cfg;

        if (max_z < oc->zorder)
            max_z = oc->zorder;

        if (oc->ix == OMAP_DSS_WB)
            use_wb = true;

        if ((z & (1 << oc->zorder)) && (oc->ix != OMAP_DSS_WB))
            ALOGW("Used z-order %d multiple times", oc->zorder);

        if (ix & (1 << oc->ix))
            ALOGW("Used ovl index %d multiple times", oc->ix);

        z |= 1 << oc->zorder;
        ix |= 1 << oc->ix;
    }

    if (!use_wb && dsscomp->num_ovls > MAX_DSS_OVERLAYS)
        ALOGW("Used too many overlays (%d)", dsscomp->num_ovls);

    if (max_z != (uint32_t)(dsscomp->num_ovls - (use_wb ? 2 : 1)))
        ALOGW("Used %d z-layers for %d overlays", max_z + 1, dsscomp->num_ovls);

    return 0;
}
