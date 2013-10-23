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
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>

#include <cutils/properties.h>
#include <cutils/log.h>
#include <hardware/hwcomposer.h>

#include <video/dsscomp.h>

#include "hwc_dev.h"
#include "color_fmt.h"
#include "blitter.h"
#include "display.h"
#include "dump.h"

enum {
    BUFFER_MAPPING_BLITTER,
    BUFFER_MAPPING_DSSCOMP
};

static void get_buffer_mapping(omap_hwc_device_t *hwc_dev, int disp, uint32_t *buffer_type)
{
    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    struct omap_hwc_blit_data *blit_data = &comp->comp_data.blit_data;
    uint32_t i;

    for (i = 0; i < comp->num_buffers; i++) {
        buffer_type[i] = BUFFER_MAPPING_BLITTER;
    }

    for (i = 0; i < dsscomp->num_ovls; i++) {
        if (dsscomp->ovls[i].addressing == OMAP_DSS_BUFADDR_LAYER_IX) {
            int ix = dsscomp->ovls[i].ba;
            if (blit_data->rgz_items) {
                if (ix > 0)
                    buffer_type[ix - 1] = BUFFER_MAPPING_DSSCOMP;
            } else {
                buffer_type[ix] = BUFFER_MAPPING_DSSCOMP;
            }
        }
    }
}

static void dump_printf(dump_buf_t *buf, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    buf->len += vsnprintf(buf->buf + buf->len, buf->buf_len - buf->len, fmt, ap);
    va_end(ap);
}

void dump_hwc_info(omap_hwc_device_t *hwc_dev, dump_buf_t *log)
{
    dump_printf(log, "OMAP HWC %d.%d:\n",
                     (hwc_dev->base.common.version >> 24) & 0xff,
                     (hwc_dev->base.common.version >> 16) & 0xff);
    dump_printf(log, "  idle timeout: %dms\n", hwc_dev->idle);

    blitter_config_t *blitter = &hwc_dev->blitter;

    if (blitter->policy != BLT_POLICY_DISABLED) {
        dump_printf(log, "  blitter:\n");
        dump_printf(log, "    policy: %s, mode: %s\n",
                         blitter->policy == BLT_POLICY_DEFAULT ? "default" :
                         blitter->policy == BLT_POLICY_ALL ? "all" : "unknown",
                         blitter->mode == BLT_MODE_PAINT ? "paint" : "regionize");
    }
}

void dump_display(omap_hwc_device_t *hwc_dev, dump_buf_t *log, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    display_config_t *config = &display->configs[display->active_config_ix];
    int device_xres = config->xres;
    int device_yres = config->yres;

    if (display->type == DISP_TYPE_LCD) {
        device_xres = display->fb_info.timings.x_res;
        device_yres = display->fb_info.timings.y_res;
    } else if (display->type == DISP_TYPE_HDMI) {
        hdmi_display_t *hdmi = (hdmi_display_t*)display;
        device_xres = hdmi->mode_db[~hdmi->video_mode_ix].xres;
        device_yres = hdmi->mode_db[~hdmi->video_mode_ix].yres;
    }

    char device_resolution[32] = {'\0'};
    if (config->xres != device_xres || config->yres != device_yres) {
        sprintf(device_resolution, " => %dx%d", device_xres, device_yres);
    }

    dump_printf(log, "  display[%d]: %s %dx%d%s\n",
                     disp,
                     display->type == DISP_TYPE_LCD ? "LCD" :
                     display->type == DISP_TYPE_HDMI ? "HDMI" :
                     display->type == DISP_TYPE_WFD ? "WFD" : "unknown",
                     config->xres, config->yres, device_resolution);

    if (display->mode == DISP_MODE_LEGACY) {
        dump_printf(log, "    legacy mode\n");
        return;
    }

    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    int i;

    for (i = 0; i < dsscomp->num_ovls; i++) {
        struct dss2_ovl_cfg *cfg = &dsscomp->ovls[i].cfg;

        dump_printf(log, "    layer[%d]:\n", i);
        dump_printf(log, "      enabled:%s buff:%p %dx%d stride:%d\n",
                         cfg->enabled ? "true" : "false", comp->buffers[i],
                         cfg->width, cfg->height, cfg->stride);
        dump_printf(log, "      src:(%d,%d) %dx%d dst:(%d,%d) %dx%d ix:%d@%d z:%d\n",
                         cfg->crop.x, cfg->crop.y, cfg->crop.w, cfg->crop.h,
                         cfg->win.x, cfg->win.y, cfg->win.w, cfg->win.h,
                         cfg->ix, cfg->mgr_ix, cfg->zorder);
    }
}

void dump_layer(const hwc_layer_1_t *layer)
{
    dump_layer_ext(layer, false);
}

void dump_layer_ext(const hwc_layer_1_t *layer, bool invalid_layer)
{
    ALOGD("\t%stype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
         invalid_layer ? ">> " : "",
         layer->compositionType, layer->flags, layer->handle, layer->transform, layer->blending,
         layer->sourceCrop.left, layer->sourceCrop.top, layer->sourceCrop.right, layer->sourceCrop.bottom,
         layer->displayFrame.left, layer->displayFrame.top, layer->displayFrame.right, layer->displayFrame.bottom);
    if (layer->handle) {
        IMG_native_handle_t *h = (IMG_native_handle_t *)layer->handle;
        ALOGD("%s%d*%d(%s)", invalid_layer ? "\t>> " : "\t   ", h->iWidth, h->iHeight, HAL_FMT(h->iFormat));
    }
}

void dump_layers_ext(const hwc_layer_1_t *list, uint32_t num_layers, uint32_t invalid_layer_ix)
{
    uint32_t i;
    for (i = 0; i < num_layers; i++) {
        const hwc_layer_1_t *layer = &list[i];
        ALOGD("Layer %d", i);
        dump_layer_ext(layer, invalid_layer_ix == i);
    }
}

void dump_prepare_info(omap_hwc_device_t *hwc_dev, int disp)
{
    display_t *display = hwc_dev->displays[disp];
    composition_t *comp = &display->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    layer_statistics_t *layer_stats = &display->layer_stats;

    const char *comp_type = "all-OVL";
    if (comp->use_sgx)
        comp_type = "SGX+OVL";
    else if (comp->comp_data.blit_data.rgz_items)
        comp_type = "BLT+OVL";

    const char *ext_type = "";
    const char *ext_mode = "OFF+";
    int ext_rotation = 0;
    const char *ext_hflip = "";
    int ext_ovls = 0;
    int ext_disp = get_external_display_id(hwc_dev);

    if (is_valid_display(hwc_dev, ext_disp)) {
        display_t *ext_display = hwc_dev->displays[ext_disp];

        if (is_hdmi_display(hwc_dev, ext_disp))
            ext_type = "TV+";
        else if (is_wfd_display(hwc_dev, ext_disp))
            ext_type = "WFD+";

        ext_mode = is_external_display_mirroring(hwc_dev, ext_disp) ? "mirror+" : "present+";
        ext_rotation = ext_display->transform.rotation * 90;

        if (ext_display->transform.hflip)
            ext_hflip = "+hflip";

        ext_ovls = ext_display->composition.avail_ovls;
    }

    ALOGD("prepare[%d] : %08x - %s (layers=%d, comp=%d, scaled=%d, RGB=%d, BGR=%d, NV12=%d) (ext=%s%s%ddeg%s, ovls=%d/%d, last=%d/%d)",
        disp, dsscomp->sync_id, comp_type,
        layer_stats->count, layer_stats->composable, layer_stats->scaled,
        layer_stats->rgb, layer_stats->bgr, layer_stats->nv12,
        ext_type, ext_mode, ext_rotation, ext_hflip,
        ext_ovls, comp->avail_ovls, hwc_dev->dsscomp.last_ext_ovls, hwc_dev->dsscomp.last_int_ovls);
}

void dump_set_info(omap_hwc_device_t *hwc_dev, int disp, hwc_display_contents_1_t *list)
{
    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    struct omap_hwc_blit_data *blit_data = &comp->comp_data.blit_data;

    char logbuf[1024];
    struct dump_buf log = {
        .buf = logbuf,
        .buf_len = sizeof(logbuf),
    };
    uint32_t i, j;

    dump_printf(&log, "set[%d] : H{", disp);

    for (i = 0; list && i < list->numHwLayers; i++) {
        if (i)
            dump_printf(&log, " ");

        hwc_layer_1_t *layer = &list->hwLayers[i];
        IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;

        const char *hw = "SGX";
        if (layer->compositionType == HWC_OVERLAY)
            hw = layer->hints & HWC_HINT_TRIPLE_BUFFER ? "DSS" : "BV2D";
        dump_printf(&log, "%p:%s,", handle, hw);

        if ((layer->flags & HWC_SKIP_LAYER) || !handle) {
            dump_printf(&log, "SKIP");
            continue;
        }

        if (layer->flags & HWC_HINT_CLEAR_FB)
            dump_printf(&log, "CLR,");

        dump_printf(&log, "%d*%d(%s)", handle->iWidth, handle->iHeight, HAL_FMT(handle->iFormat));

        if (layer->transform)
            dump_printf(&log, "~%d", layer->transform);
    }

    dump_printf(&log, "} D{");

    for (i = 0; i < dsscomp->num_ovls; i++) {
        if (i)
            dump_printf(&log, " ");

        dump_printf(&log, "%d@%d=", dsscomp->ovls[i].cfg.ix, dsscomp->ovls[i].cfg.mgr_ix);

        if (dsscomp->ovls[i].cfg.enabled)
            dump_printf(&log, "%08x:%d*%d,%s",
                        dsscomp->ovls[i].ba,
                        dsscomp->ovls[i].cfg.width,
                        dsscomp->ovls[i].cfg.height,
                        DSS_FMT(dsscomp->ovls[i].cfg.color_mode));
        else
            dump_printf(&log, "-");
    }

    uint32_t buffer_type[MAX_COMPOSITION_BUFFERS];
    get_buffer_mapping(hwc_dev, disp, buffer_type);

    dump_printf(&log, "} L{");

    for (i = 0, j = 0; i < comp->num_buffers; i++) {
        if (buffer_type[i] == BUFFER_MAPPING_DSSCOMP)
            dump_printf(&log, "%s%p", j++ ? " " : "", comp->buffers[i]);
    }

    if (blit_data->rgz_items) {
        dump_printf(&log, "} B{");
        for (i = 0, j = 0; i < comp->num_buffers; i++) {
            if (buffer_type[i] == BUFFER_MAPPING_BLITTER)
                dump_printf(&log, "%s%p", j++ ? " " : "", comp->buffers[i]);
        }
    }

    dump_printf(&log, "}%s", comp->use_sgx ? " swap" : "");

    ALOGD("%s", log.buf);
}

void dump_dsscomp(struct dsscomp_setup_dispc_data *d)
{
    uint32_t i;

    ALOGD("[%08x] set: %c%c%c %d ovls",
         d->sync_id,
         (d->mode & DSSCOMP_SETUP_MODE_APPLY) ? 'A' : '-',
         (d->mode & DSSCOMP_SETUP_MODE_DISPLAY) ? 'D' : '-',
         (d->mode & DSSCOMP_SETUP_MODE_CAPTURE) ? 'C' : '-',
         d->num_ovls);

    for (i = 0; i < d->num_mgrs; i++) {
        struct dss2_mgr_info *mi = &d->mgrs[i];
        ALOGD(" (dis%d alpha=%d col=%08x ilace=%d)",
             mi->ix,
             mi->alpha_blending, mi->default_color,
             mi->interlaced);
    }

    for (i = 0; i < d->num_ovls; i++) {
        struct dss2_ovl_info *oi = &d->ovls[i];
        struct dss2_ovl_cfg *c = &oi->cfg;
        char writeback[20] = {'\0'};
        if (c->ix == OMAP_DSS_WB)
            sprintf(writeback, "wb(%s@%s%d) => ",
                    c->wb_mode == OMAP_WB_MEM2MEM_MODE ? "m2m" : "cap",
                    c->wb_source < OMAP_WB_GFX ? "mgr" : "ovl",
                    c->wb_source < OMAP_WB_GFX ? c->wb_source : c->wb_source - OMAP_WB_GFX);
        if (c->zonly)
            ALOGD("ovl%d@%d(%s z%d)",
                 c->ix, c->mgr_ix, c->enabled ? "ON" : "off", c->zorder);
        else
            ALOGD("ovl%d@%d(%s z%d %s%s *%d%% %s%d*%d:%d,%d+%d,%d rot%d%s => %d,%d+%d,%d %p/%p|%d)",
                 c->ix, c->mgr_ix, c->enabled ? "ON" : "off", c->zorder, DSS_FMT(c->color_mode),
                 c->pre_mult_alpha ? " premult" : "",
                 (c->global_alpha * 100 + 128) / 255,
                 writeback,
                 c->width, c->height, c->crop.x, c->crop.y,
                 c->crop.w, c->crop.h,
                 c->rotation, c->mirror ? "+mir" : "",
                 c->win.x, c->win.y, c->win.w, c->win.h,
                 (void *) oi->ba, (void *) oi->uv, c->stride);
    }
}

void dump_post2(omap_hwc_device_t *hwc_dev, int disp)
{
    composition_t *comp = &hwc_dev->displays[disp]->composition;
    struct dsscomp_setup_dispc_data *dsscomp = &comp->comp_data.dsscomp_data;
    struct omap_hwc_blit_data *blit_data = &comp->comp_data.blit_data;
    uint32_t buffer_type[MAX_COMPOSITION_BUFFERS];
    uint32_t i;

    get_buffer_mapping(hwc_dev, disp, buffer_type);

    for (i = 0; i < comp->num_buffers; i++) {
        ALOGI("buf[%d] hndl %p => %s", i, comp->buffers[i], buffer_type[i] == BUFFER_MAPPING_DSSCOMP ? "dss" : "blt");
    }

    for (i = 0; i < dsscomp->num_ovls; i++) {
        char ba[32];
        switch (dsscomp->ovls[i].addressing) {
        case OMAP_DSS_BUFADDR_LAYER_IX:
            if (blit_data->rgz_items)
                if (i == 0)
                    sprintf(ba, "bltfb");
                else
                    sprintf(ba, "buf%d", dsscomp->ovls[i].ba - 1);
            else
                sprintf(ba, "buf%d", dsscomp->ovls[i].ba);
            break;
        case OMAP_DSS_BUFADDR_OVL_IX:
            sprintf(ba, "ovl%d", dsscomp->ovls[i].ba);
            break;
        case OMAP_DSS_BUFADDR_ION:
            sprintf(ba, "%p", (void *)dsscomp->ovls[i].ba);
            break;
        default:
            sprintf(ba, "%d", dsscomp->ovls[i].ba);
            break;
        }

        ALOGI("ovl[%d] ba %s", i, ba);
    }
}
