ifeq ($(BOARD_WLAN_DEVICE),bcmdhd-amazon)

LOCAL_PATH := $(call my-dir)

include $(call first-makefiles-under,$(LOCAL_PATH))

endif

