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

//#define LOG_NDEBUG 0
#define LOG_TAG "DSSWBHal"
#include <utils/Log.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <video/dsscomp.h>
#include <string.h>

#include <cutils/atomic.h>
#include <utils/misc.h>

#include <binder/IServiceManager.h>
#include <utils/Errors.h>
#include <utils/String8.h>
#include <utils/SystemClock.h>
#include <utils/Vector.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <gralloc/ti_pixel_formats.h>

#include "DSSWBHal.h"

namespace android {

static DSSWBHal* gDSSWBHal;

DSSWBHal::DSSWBHal() {
    ALOGV("DSSWBHal constructor");
    mWBHandle = 0;
    mGrallocModule = NULL;

    memset(&mConfig, 0, sizeof(mConfig));
}

status_t DSSWBHal::initialize() {
    status_t err;

    err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t**)&mGrallocModule);
    if (err) {
        ALOGE("unable to open gralloc module %d", err);
        return err;
    }

    mDssCompFd = open("/dev/dsscomp", O_RDWR);
    if (mDssCompFd < 0) {
        ALOGE("failed to open dsscomp (%d)", errno);
        err = -errno;
        return err;
    }

    srand(time(NULL));

    return NO_ERROR;
}

DSSWBHal::~DSSWBHal() {
    ALOGV("DSSWBHal destructor");
}

status_t DSSWBHal::instantiate() {
    ALOGV("DSSWBHal::instantiate");

    if (!gDSSWBHal) {
        gDSSWBHal = new DSSWBHal();

        status_t err = gDSSWBHal->initialize();
        if (err) {
            delete gDSSWBHal;
            gDSSWBHal = NULL;
            return err;
        }

        err = defaultServiceManager()->addService(String16("hardware.dsswb"), gDSSWBHal);
        if (err) {
            delete gDSSWBHal;
            gDSSWBHal = NULL;
            return err;
        }
    }

    return NO_ERROR;
}

buffer_handle_t DSSWBHal::processQueue() {
    ALOGV("DSSWBHal::processQueue");
    AutoMutex lock(mLock);

    if (mQueueList.empty()) {
        return NULL;
    }

    // remove the top buffer from queued list
    List<int>::iterator it;
    it = mQueueList.begin();
    int bufIndex = *it;

    // erase this buffer from queued list and update status
    mQueueList.erase(it);
    mBufferSlots.editItemAt(bufIndex).state = BufferSlot::WRITEBACK;

    // add this buffer to writeback list and give to hwc for capture
    mWritebackList.push_back(bufIndex);

    ALOGV("processqueue returns index %d handle %p\n", bufIndex, mBufferSlots[bufIndex].handle);
    return mBufferSlots[bufIndex].handle;
}

void DSSWBHal::captureStarted(buffer_handle_t handle, int syncId) {
    AutoMutex lock(mLock);
    for (List<int>::iterator it = mWritebackList.begin(); it != mWritebackList.end(); ++it) {
        if (mBufferSlots[*it].handle == handle) {
            // move this buffer from writeback to dequeue list and signal dequeue
            mBufferSlots.editItemAt(*it).syncId = syncId;
            mDequeueList.push_back(*it);
            mWritebackList.erase(it);
            mDequeueCondition.signal();
            break;
        }
    }
}

void DSSWBHal::getConfig(wb_capture_config_t *config) {
    AutoMutex lock(mLock);

    getConfigLocked(config);
}

bool DSSWBHal::capturePending() {
    // TODO: can we capture display only if any layers are changing?
    return mWBHandle ? true : false;
}

status_t DSSWBHal::acquireWB(int *wbHandlePtr) {
    ALOGV("DSSWBHal::acquireWB");
    AutoMutex lock(mLock);
    if (mWBHandle != 0)
        return ALREADY_EXISTS;

    // assign dynamic value to make WB session secure
    mWBHandle = rand();
    *wbHandlePtr = mWBHandle;

    return NO_ERROR;
}

status_t DSSWBHal::releaseWB(int wbHandle) {
    ALOGV("DSSWBHal::releaseWB");
    AutoMutex lock(mLock);
    status_t err;

    if (wbHandle != mWBHandle)
        return PERMISSION_DENIED;

    // clear the queue and dequeue lists
    mQueueList.clear();
    mDequeueList.clear();

    // reset member variables at end of session
    for (Vector<BufferSlot>::iterator it = mBufferSlots.begin(); it != mBufferSlots.end(); ++it) {
        if (it->state != BufferSlot::FREE) {
            if (it->state != BufferSlot::DEQUEUED)
                ALOGW("unregistering buffer that is still being used (state = %d)", it->state);

            err = mGrallocModule->unregisterBuffer(mGrallocModule, it->handle);
            if (err != 0)
                ALOGW("unable to unregister buffer from SF allocator");
            native_handle_close(it->handle);
            native_handle_delete(it->handle);
        }
    }

    mBufferSlots.clear();

    mWBHandle = 0;

    return NO_ERROR;
}

status_t DSSWBHal::registerBuffer(int wbHandle, int bufIndex, buffer_handle_t handle) {
    ALOGV("DSSWBHal::registerBuffer");
    AutoMutex lock(mLock);

    if (wbHandle != mWBHandle)
        return PERMISSION_DENIED;

    if (bufIndex < 0)
        return BAD_VALUE;

    return registerBufferLocked(bufIndex, handle);
}

status_t DSSWBHal::registerBuffers(int wbHandle, int numBuffers, buffer_handle_t handles[]) {
    ALOGV("DSSWBHal::registerBuffers");
    AutoMutex lock(mLock);

    if (wbHandle != mWBHandle)
        return PERMISSION_DENIED;

    if (handles == NULL || numBuffers <= 0)
        return BAD_VALUE;

    // allow buffer registration only once per WB session
    // TODO: allow multiple registrations of buffers to support dynamic change of capture resolution
    if (!mBufferSlots.empty()) {
        ALOGE("buffers have been already registered");
        return ALREADY_EXISTS;
    }

    // grow mBufferSlots vector
    mBufferSlots.insertAt(0, numBuffers);

    for (int i = 0; i < numBuffers; ++i) {
        status_t err = registerBufferLocked(i, handles[i]);
        if (err)
            return err;
    }

    return NO_ERROR;
}

status_t DSSWBHal::queue(int wbHandle, int bufIndex) {
    AutoMutex lock(mLock);
    ALOGV("DSSWBHal::queue");

    if (wbHandle != mWBHandle)
        return PERMISSION_DENIED;

    if (bufIndex < 0 || bufIndex >= (int)mBufferSlots.size() || !mBufferSlots[bufIndex].handle)
        return BAD_INDEX;

    if (mBufferSlots[bufIndex].state == BufferSlot::QUEUED)
        return ALREADY_EXISTS;

    if (mBufferSlots[bufIndex].state == BufferSlot::WRITEBACK)
        return INVALID_OPERATION;

    mQueueList.push_back(bufIndex);
    mBufferSlots.editItemAt(bufIndex).state = BufferSlot::QUEUED;

    ALOGV("WBHal::queue index %d numqueued %d", bufIndex, mQueueList.size());
    return NO_ERROR;
}

status_t DSSWBHal::dequeue(int wbHandle, int *bufIndex) {
    ALOGV("DSSWBHal::dequeue");
    {
        AutoMutex lock(mLock);
        if (wbHandle != mWBHandle)
            return PERMISSION_DENIED;

        while ((!mQueueList.empty() || !mWritebackList.empty()) && mDequeueList.empty()) {
            ALOGV("no buffers to dequeue numqueued %d", mQueueList.size());
            // wait for the queue to get one more buffer
            mDequeueCondition.wait(mLock);
        }

        if (mDequeueList.empty()) {
            return INVALID_OPERATION;
        }

        List<int>::iterator it;
        it = mDequeueList.begin();
        *bufIndex = *it;

        mDequeueList.erase(it);
        mBufferSlots.editItemAt(*bufIndex).state = BufferSlot::DEQUEUED;
    }

    int err = NO_ERROR;
    if (mBufferSlots[*bufIndex].syncId != 0) {
        err = ioctl(mDssCompFd, DSSCIOC_WB_DONE, &mBufferSlots[*bufIndex].syncId);
        if (err)
            ALOGW("Timed out waiting for WB operation to complete (%d)", err);
    }

    ALOGV("WBHal::dequeue index %d status %d", *bufIndex, BufferSlot::DEQUEUED);

    return err;
}

status_t DSSWBHal::cancelBuffer(int wbHandle, int *bufIndex) {
    AutoMutex lock(mLock);
    ALOGV("DSSWBHal::cancelBuffer");
    if (wbHandle != mWBHandle)
        return PERMISSION_DENIED;

    if (mQueueList.empty()) {
        ALOGV("no buffers to cancel %d", mQueueList.size());
        // wait for the queue to get one more buffer
        return INVALID_OPERATION;
    }

    List<int>::iterator it;
    it = mQueueList.begin();
    *bufIndex = *it;

    mQueueList.erase(it);
    mBufferSlots.editItemAt(*bufIndex).state = BufferSlot::DEQUEUED;
    ALOGV("WBHal::cancelBuffer index %d status %d", *bufIndex, BufferSlot::DEQUEUED);
    mDequeueCondition.signal();
    return NO_ERROR;
}

status_t DSSWBHal::setConfig(int wbHandle, const wb_capture_config_t &config) {
    // A limitation of decoupling config from buffer is that a config
    // is loosely associated with buffer and not tied too hard.
    ALOGV("DSSWBHal::setConfig");
    AutoMutex lock(mLock);
    if (wbHandle != mWBHandle)
        return PERMISSION_DENIED;

    // TODO: need to check for capabilities before accepting the config
    mConfig = config;

    ALOGV("Config transform %d", mConfig.transform);

    return NO_ERROR;
}

status_t DSSWBHal::getConfig(int wbHandle, wb_capture_config_t *config) {
    ALOGV("DSSWBHal::getConfig");
    AutoMutex lock(mLock);
    if (wbHandle != mWBHandle)
        return PERMISSION_DENIED;

    getConfigLocked(config);

    return NO_ERROR;
}

void DSSWBHal::getConfigLocked(wb_capture_config_t *config) {
    *config = mConfig;
}

status_t DSSWBHal::registerBufferLocked(int bufIndex, buffer_handle_t handle) {
    if (handle == NULL) {
        ALOGE("invalid buffer handle");
        return BAD_VALUE;
    }

    if ((size_t)bufIndex >= mBufferSlots.size()) {
        // grow mBufferSlots vector
        mBufferSlots.insertAt(mBufferSlots.size(), bufIndex - mBufferSlots.size() + 1);
    }

    // allow buffer registration only once per WB session
    // TODO: allow multiple registrations of buffers to support dynamic change of capture resolution
    if (mBufferSlots[bufIndex].state != BufferSlot::FREE) {
        ALOGE("buffer slot % is already used", bufIndex);
        return ALREADY_EXISTS;
    }

    status_t err = mGrallocModule->registerBuffer(mGrallocModule, handle);
    if (err) {
        ALOGE("unable to register handle with SF allocator");
        return err;
    }

    BufferSlot &slot = mBufferSlots.editItemAt(bufIndex);
    slot.handle = (native_handle_t *)handle;
    slot.state = BufferSlot::DEQUEUED;

    ALOGV("registered handle %p", slot.handle);

    return NO_ERROR;
}

int wb_open() {
    return DSSWBHal::instantiate();
}

int wb_capture_layer(hwc_layer_1_t *wb_layer) {
    wb_capture_config config;
    buffer_handle_t handle = gDSSWBHal->processQueue();

    // check if we have anything to capture
    if (handle == NULL)
        return 0;

    gDSSWBHal->getConfig(&config);

    //format the capture frame info as a layer
    wb_layer->handle = handle;

    wb_layer->transform = config.transform;
    wb_layer->displayFrame = config.captureFrame;
    wb_layer->sourceCrop = config.sourceCrop;

    // constant settings for WB layer, may use/change these later
    wb_layer->blending = HWC_BLENDING_NONE;
    wb_layer->compositionType = HWC_OVERLAY;
    wb_layer->hints = 0;
    wb_layer->flags = 0;

    return 1;
}

void wb_capture_started(buffer_handle_t handle, int sync_id) {
    gDSSWBHal->captureStarted(handle, sync_id);
}

int wb_capture_pending() {
    return gDSSWBHal->capturePending();
}

};
