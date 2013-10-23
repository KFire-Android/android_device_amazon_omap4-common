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
#include <fcntl.h>

#include <cutils/properties.h>
#include <cutils/log.h>
#include <hardware/hwcomposer.h>

#include <linux/bltsville.h>
#include <video/dsscomp.h>
#include <video/omap_hwc.h>

#include "hwc_dev.h"
#include "blitter.h"
#include "display.h"

static rgz_t grgz;
static rgz_ext_layer_list_t grgz_ext_layer_list;
static struct bvsurfgeom gscrngeom;

int init_blitter(omap_hwc_device_t *hwc_dev)
{
    blitter_config_t *blitter = &hwc_dev->blitter;
    int err = 0;

    int gc2d_fd = open("/dev/gcioctl", O_RDWR);
    if (gc2d_fd < 0) {
        ALOGI("Unable to open gc-core device (%d), blits disabled", errno);
        blitter->policy = BLT_POLICY_DISABLED;
        blitter->debug = false;
    } else {
        char value[PROPERTY_VALUE_MAX];

        property_get("persist.hwc.bltmode", value, "1");
        blitter->mode = atoi(value);

        property_get("persist.hwc.bltpolicy", value, "1");
        blitter->policy = atoi(value);

        ALOGI("blitter present, blits mode %d, blits policy %d", blitter->mode, blitter->policy);

        property_get("persist.hwc.bltdebug", value, "0");
        blitter->debug = atoi(value);

        rgz_enable_debug_trace(blitter->debug);

        close(gc2d_fd);

        if (rgz_get_screengeometry(hwc_dev->fb_fd[HWC_DISPLAY_PRIMARY], &gscrngeom,
                hwc_dev->fb_dev[HWC_DISPLAY_PRIMARY]->base.format) != 0) {
            err = -EINVAL;
        }
    }

    return err;
}

uint32_t get_blitter_policy(omap_hwc_device_t *hwc_dev, int disp)
{
    blitter_config_t *blitter = &hwc_dev->blitter;
    /*
     * Since we have only one set of framebuffers allocated in kernel, blitter is used only on
     * a single (primary) display.
     */
    if (disp != HWC_DISPLAY_PRIMARY) {
        ALOGI_IF(blitter->debug, "blitter: turning off for non-primary display");
        return BLT_POLICY_DISABLED;
    }

    /*
     * WORKAROUND: Do not blit on idle timeout. DSS consumes more power when reading from blitter
     * FB (VRAM), than when reading from SGX FB (Tiler2D). To minimize the power consumption idle
     * frames should be composed entirely by SGX.
     */
    if (hwc_dev->force_sgx) {
        ALOGI_IF(blitter->debug, "blitter: turning off due to forced SGX composition");
        return BLT_POLICY_DISABLED;
    }

    /* WORKAROUND: Currently blitter is supported only for single display scenarios. */
    if (get_external_display_id(hwc_dev) >= 0) {
        ALOGI_IF(blitter->debug, "blitter: turning off due to external display present");
        return BLT_POLICY_DISABLED;
    }

    return hwc_dev->blitter.policy;
}

void reset_blitter(omap_hwc_device_t *hwc_dev)
{
    /* Blitter is used only in primary display composition */
    composition_t *comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
    struct omap_hwc_blit_data *blit_data = &comp->comp_data.blit_data;

    blit_data->rgz_flags = 0;
    blit_data->rgz_items = 0;
}

void release_blitter(void)
{
    rgz_release(&grgz);
}

bool blit_layers(omap_hwc_device_t *hwc_dev, hwc_display_contents_1_t *contents)
{
    blitter_config_t *blitter = &hwc_dev->blitter;
    ALOGI_IF(blitter->debug, "blitter: trying to blit layers");

    if (!contents) {
        ALOGI_IF(blitter->debug, "blitter: skipped blitting due to empty layer list");
        goto err_out;
    }

    int rgz_in_op;
    int rgz_out_op;

    switch (blitter->mode) {
        case BLT_MODE_PAINT:
            rgz_in_op = RGZ_IN_HWCCHK;
            rgz_out_op = RGZ_OUT_BVCMD_PAINT;
            break;
        case BLT_MODE_REGION:
        default:
            rgz_in_op = RGZ_IN_HWC;
            rgz_out_op = RGZ_OUT_BVCMD_REGION;
            break;
    }

    /* Ignore HWC_FRAMEBUFFER_TARGET layer at the end of the list */
    size_t num_layers = contents->numHwLayers - 1;

    /*
     * Request the layer identities to SurfaceFlinger, first figure out if the
     * operation is supported
     */
    if (!(contents->flags & HWC_EXTENDED_API) || !hwc_dev->procs ||
        hwc_dev->procs->extension_cb(hwc_dev->procs, HWC_EXTENDED_OP_LAYERDATA, NULL, -1) != 0) {
        ALOGI_IF(blitter->debug, "blitter: skipped blitting due to unsupported operation");
        goto err_out;
    }

    /* Check if we have enough space in the extended layer list */
    if ((sizeof(hwc_layer_extended_t) * num_layers) > sizeof(grgz_ext_layer_list)) {
        ALOGI_IF(blitter->debug, "blitter: skipped blitting due to insufficient memory for extended layer list");
        goto err_out;
    }

    uint32_t i;
    for (i = 0; i < num_layers; i++) {
        hwc_layer_extended_t *ext_layer = &grgz_ext_layer_list.layers[i];
        ext_layer->idx = i;
        if (hwc_dev->procs->extension_cb(hwc_dev->procs, HWC_EXTENDED_OP_LAYERDATA,
            (void **) &ext_layer, sizeof(hwc_layer_extended_t)) != 0) {
            ALOGI_IF(blitter->debug, "blitter: skipped blitting due to unsupported operation on layer");
            goto err_out;
        }
    }

    rgz_in_params_t in = {
        .op = rgz_in_op,
        .data = {
            .hwc = {
                .dstgeom = &gscrngeom,
                .layers = contents->hwLayers,
                .extlayers = grgz_ext_layer_list.layers,
                .layerno = num_layers
            }
        }
    };

    /*
     * This means if all the layers marked for the FRAMEBUFFER cannot be
     * blitted, do not blit, for e.g. SKIP layers
     */
    if (rgz_in(&in, &grgz) != RGZ_ALL) {
        ALOGI_IF(blitter->debug, "blitter: skipped blitting because we cannot blit some of the requested layers");
        goto err_out;
    }

    uint32_t count = 0;
    for (i = 0; i < num_layers; i++) {
        if (contents->hwLayers[i].compositionType != HWC_OVERLAY) {
            count++;
        }
    }

    rgz_out_params_t out = {
        .op = rgz_out_op,
        .data = {
            .bvc = {
                .dstgeom = &gscrngeom,
                .noblend = 0,
            }
        }
    };

    if (rgz_out(&grgz, &out) != 0) {
        ALOGE("Failed generating blits");
        goto err_out;
    }

    /* This is a special situation where the regionizer decided no blits are
     * needed for this frame but there are blit buffers to synchronize with. Can
     * happen only if the regionizer is enabled otherwise it's likely a bug
     */
    if (rgz_out_op != RGZ_OUT_BVCMD_REGION && out.data.bvc.out_blits == 0 && out.data.bvc.out_nhndls > 0) {
        ALOGE("Regionizer invalid output blit_num %d, post2_blit_buffers %d",
                out.data.bvc.out_blits, out.data.bvc.out_nhndls);
        goto err_out;
    }

    /* Blitter is used only in primary display composition */
    composition_t *comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
    struct omap_hwc_blit_data *blit_data = &comp->comp_data.blit_data;

    blit_data->rgz_flags |= HWC_BLT_FLAG_USE_FB;
    blit_data->rgz_items = out.data.bvc.out_blits;

    for (i = 0; i < (uint32_t)out.data.bvc.out_nhndls; i++) {
        //ALOGI("blit buffers[%d] = %p", bufoff, out.data.bvc.out_hndls[i]);
        comp->buffers[comp->num_buffers++] = out.data.bvc.out_hndls[i];
    }

    struct rgz_blt_entry *res_blit_ops = (struct rgz_blt_entry *) out.data.bvc.cmdp;
    memcpy(blit_data->rgz_blts, res_blit_ops, sizeof(*res_blit_ops) * out.data.bvc.cmdlen);

    ALOGI_IF(blitter->debug, "blitter: blt struct sz %d", sizeof(*res_blit_ops) * out.data.bvc.cmdlen);
    ALOGE_IF(out.data.bvc.out_blits != out.data.bvc.cmdlen, "blit_num != out.data.bvc.cmdlen, %d != %d",
            out.data.bvc.out_blits, out.data.bvc.cmdlen);

    /* all layers will be rendered without SGX help either via DSS or blitter */
    for (i = 0; i < num_layers; i++) {
        if (contents->hwLayers[i].compositionType != HWC_OVERLAY) {
            contents->hwLayers[i].compositionType = HWC_OVERLAY;
            //ALOGI("blitting layer %d", i);
            contents->hwLayers[i].hints &= ~HWC_HINT_TRIPLE_BUFFER;
        }

        contents->hwLayers[i].hints &= ~HWC_HINT_CLEAR_FB;
    }

    ALOGI_IF(blitter->debug, "blitter: layers were blitted successfully");
    return true;

err_out:
    release_blitter();
    ALOGI_IF(blitter->debug, "blitter: layers were not blitted");
    return false;
}

uint32_t get_blitter_data_size(omap_hwc_device_t *hwc_dev)
{
    /* Blitter is used only in primary display composition */
    composition_t *comp = &hwc_dev->displays[HWC_DISPLAY_PRIMARY]->composition;
    struct omap_hwc_blit_data *blit_data = &comp->comp_data.blit_data;

    return blit_data->rgz_items * sizeof(struct rgz_blt_entry);
}
