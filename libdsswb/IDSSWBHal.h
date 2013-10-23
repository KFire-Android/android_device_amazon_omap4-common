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

#ifndef IDSSWB_HAL_H
#define IDSSWB_HAL_H

#include <utils/Errors.h>
#include <binder/IInterface.h>
#include <binder/Parcel.h>

#include <hardware/gralloc.h>
#include <hardware/hwcomposer.h>

namespace android {

typedef struct wb_capture_config {
    /* Transformation to apply to the buffer during composition. */
    int transform;

    /* Area of the source to consider, the origin is the top-left corner of the screen. */
    hwc_rect_t sourceCrop;

    /* Where to capture the sourceCrop into the buffer. The sourceCrop is scaled using
     * linear filtering to the captureFrame. The origin is the top-left corner of the buffer.
     */
    hwc_rect_t captureFrame;
} wb_capture_config_t;

class IDSSWBHal: public IInterface
{
public:
    DECLARE_META_INTERFACE(DSSWBHal);

    virtual status_t acquireWB(int *wbHandlePtr) = 0;
    virtual status_t releaseWB(int wbHandle) = 0;
    virtual status_t registerBuffer(int wbHandle, int bufIndex, buffer_handle_t handle) = 0;
    virtual status_t registerBuffers(int wbHandle, int numBuffers, buffer_handle_t handles[]) = 0;
    virtual status_t queue(int wbHandle, int bufIndex) = 0;
    virtual status_t dequeue(int wbHandle, int *bufIndex) = 0;
    virtual status_t cancelBuffer(int wbHandle, int *bufIndex) = 0;
    virtual status_t setConfig(int wbHandle, const wb_capture_config_t &config) = 0;
    virtual status_t getConfig(int wbHandle, wb_capture_config_t *config) = 0;
};

class BnDSSWBHal: public BnInterface<IDSSWBHal>
{
public:
    virtual status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags = 0);
};

} // namespace android

#endif // IDSSWB_HAL_H
