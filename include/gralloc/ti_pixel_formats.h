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

#ifndef _TI_PIXEL_FORMATS_H_
#define _TI_PIXEL_FORMATS_H_

/**
 * pixel format definitions
 */

enum {
    /*
     * 0x100 - 0x1FF
     *
     * This range is reserved for pixel formats that are specific to the HAL
     * implementation.  Implementations can use any value in this range to
     * communicate video pixel formats between their HAL modules.  These formats
     * must not have an alpha channel.  Additionally, an EGLimage created from a
     * gralloc buffer of one of these formats must be supported for use with the
     * GL_OES_EGL_image_external OpenGL ES extension.
     */

    /*
     * These are vendor specific pixel formats, by (informal) convention IMGTec
     * formats start from the top of the range, TI formats start from the bottom
     */
    HAL_PIXEL_FORMAT_TI_NV12    = 0x100,
    HAL_PIXEL_FORMAT_TI_UNUSED  = 0x101,
    HAL_PIXEL_FORMAT_TI_NV12_1D = 0x102,
    HAL_PIXEL_FORMAT_TI_Y8      = 0x103,
    HAL_PIXEL_FORMAT_TI_Y16     = 0x104,
    HAL_PIXEL_FORMAT_TI_UYVY    = 0x105,
    HAL_PIXEL_FORMAT_BGRX_8888  = 0x1FF,
};

#endif /* _TI_PIXEL_FORMATS_H_ */
