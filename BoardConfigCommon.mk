# Copyright (C) 2013 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

COMMON_FOLDER := device/amazon/omap4-common

# set to allow building from omap4-common
BOARD_VENDOR := amazon

# This variable is set first, so it can be overridden
# by BoardConfigVendor.mk
BOARD_USES_GENERIC_AUDIO := false

# Bluetooth
BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := $(COMMON_FOLDER)/bluetooth

# TI Enhancement Settings (Part 1)
OMAP_ENHANCEMENT := true
#OMAP_ENHANCEMENT_BURST_CAPTURE := true
#OMAP_ENHANCEMENT_S3D := true
#OMAP_ENHANCEMENT_CPCAM := true
#OMAP_ENHANCEMENT_VTC := true
OMAP_ENHANCEMENT_MULTIGPU := true
ENHANCED_DOMX := true
BLTSVILLE_ENHANCEMENT :=true
#BOARD_USES_DVP := true
#BOARD_USES_ARX := true
#USE_ITTIAM_AAC := true
ANDROID_API_JB_OR_LATER := true

# Processor
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_VARIANT := cortex-a9
TARGET_CPU_ABI2 := armeabi
TARGET_CPU_SMP := true
TARGET_ARCH := arm
TARGET_BOARD_PLATFORM := omap4
TARGET_ARCH_VARIANT := armv7-a-neon

# Kernel
BOARD_KERNEL_BASE := 0x80000000
TARGET_NO_RADIOIMAGE := true
TARGET_NO_BOOTLOADER := true

# Filesystem
TARGET_USERIMAGES_USE_EXT4 := true
BOARD_VOLD_MAX_PARTITIONS := 32
BOARD_VOLD_EMMC_SHARES_DEV_MAJOR := true

# adb has root
ADDITIONAL_DEFAULT_PROPERTIES += ro.secure=0
ADDITIONAL_DEFAULT_PROPERTIES += ro.allow.mock.location=1

# Graphics
USE_OPENGL_RENDERER := true
# set if the target supports FBIO_WAITFORVSYNC
TARGET_HAS_WAITFORVSYNC := true

# Recovery
BOARD_HAS_NO_SELECT_BUTTON := true
BOARD_HAS_LARGE_FILESYSTEM := true
BOARD_UMS_LUNFILE := "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file"
TARGET_RECOVERY_PIXEL_FORMAT := "BGRA_8888"
TW_INCLUDE_FB2PNG := true

# Misc.
BOARD_NEEDS_CUTILS_LOG := true
BOARD_USES_SECURE_SERVICES := true

#Set 32 byte cache line to true
ARCH_ARM_HAVE_32_BYTE_CACHE_LINES := true

# Common device independent definitions
include device/ti/common-open/BoardConfig.mk
