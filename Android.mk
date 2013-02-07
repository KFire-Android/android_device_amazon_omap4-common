ifeq ($(BOARD_VENDOR),amazon)
ifeq ($(TARGET_BOARD_PLATFORM),omap4)

LOCAL_PATH := $(call my-dir)

ifneq ($(TARGET_SIMULATOR),true)

ifeq ($(BOARD_CREATE_AMAZON_HDCP_KEYS_SYMLINK), true)

#Create HDCP symlink
HDCP_SYMLINK := $(TARGET_OUT_VENDOR)/firmware/hdcp.keys
$(HDCP_SYMLINK): HDCP_KEYS_FILE := /efs/hdcp/hdcp.kek.wrapped
$(HDCP_SYMLINK): $(LOCAL_INSTALLED_MODULE) $(LOCAL_PATH)/Android.mk
	@echo "Symlink: $@ -> $(HDCP_KEYS_FILE)"
	@mkdir -p $(TARGET_OUT_VENDOR)/firmware
	@rm -rf $@
	$(hide) ln -fs $(HDCP_KEYS_FILE) $@

ALL_DEFAULT_INSTALLED_MODULES += $(HDCP_SYMLINK)

# for mm/mmm
all_modules: $(HDCP_SYMLINK)

endif

include $(call first-makefiles-under,$(LOCAL_PATH))
endif

endif
endif

