# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)

###
### GENERIC AUDIO HAL
###

include $(CLEAR_VARS)

LOCAL_MODULE := audio.primary.$(TARGET_BOOTLOADER_BOARD_NAME)
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

LOCAL_SRC_FILES := \
	audio_hw.c

ifneq ($(BOARD_AUDIO_HW_CONFIG_DIR),)
LOCAL_C_INCLUDES += $(BOARD_AUDIO_HW_CONFIG_DIR)
else
LOCAL_C_INCLUDES += $(LOCAL_PATH)/config
endif

LOCAL_C_INCLUDES += \
	external/tinyalsa/include \
	system/media/audio_route/include \
	$(call include-path-for, audio-utils) \
	$(call include-path-for, audio-effects)

LOCAL_SHARED_LIBRARIES := liblog libcutils libtinyalsa libaudioutils libaudio-resampler libaudioroute
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

###
### OMAP HDMI AUDIO HAL
###

include $(CLEAR_VARS)

LOCAL_MODULE := audio.hdmi.$(TARGET_BOOTLOADER_BOARD_NAME)

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SRC_FILES := hdmi_audio_hw.c \
	hdmi_audio_utils.c

LOCAL_C_INCLUDES += \
	external/tinyalsa/include \
	system/media/audio_utils/include \
	system/media/audio_effects/include \
	frameworks/native/include/media/openmax \
        $(DOMX_PATH)/omx_core/inc

LOCAL_SHARED_LIBRARIES := liblog libcutils libtinyalsa libaudioutils libdl
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

