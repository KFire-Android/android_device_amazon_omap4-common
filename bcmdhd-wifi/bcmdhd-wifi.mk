# Setup file for BCMDHD used in Amazon Kindle Fire HD models

# Connectivity - Wi-Fi
BOARD_WPA_SUPPLICANT_DRIVER      := NL80211
WPA_SUPPLICANT_VERSION           := VER_0_8_X
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_bcmdhd
#BOARD_HOSTAPD_DRIVER             := NL80211
#BOARD_HOSTAPD_PRIVATE_LIB        := lib_driver_cmd_bcmdhd
BOARD_WLAN_DEVICE                := bcmdhd-amazon
BOARD_WLAN_DEVICE_REV            := bcm43239_a0
WIFI_DRIVER_FW_PATH_PARAM        := "/sys/module/bcmdhd/parameters/firmware_path"
#WIFI_DRIVER_MODULE_PATH          := "/system/lib/modules/bcmdhd.ko"
WIFI_DRIVER_FW_PATH_STA          := "/vendor/firmware/fw_bcmdhd.bin"
#WIFI_DRIVER_FW_PATH_P2P          := "/vendor/firmware/fw_bcmdhd_p2p.bin"
#WIFI_DRIVER_FW_PATH_AP           := "/vendor/firmware/fw_bcmdhd_apsta.bin"
PRODUCT_WIRELESS_TOOLS           := true


# Wifi
PRODUCT_PACKAGES += \
    lib_driver_cmd_bcmdhd \
    libnetcmdiface

PRODUCT_COPY_FILES += \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/firmware-$(TARGET_BOOTLOADER_BOARD_SUBTYPE).bin:system/vendor/firmware/fw_bcmdhd.bin \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/firmware-$(TARGET_BOOTLOADER_BOARD_SUBTYPE).bin:system/vendor/firmware/fw_bcmdhd_apsta.bin \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/firmware-$(TARGET_BOOTLOADER_BOARD_SUBTYPE).bin:system/vendor/firmware/fw_bcmdhd_p2p.bin \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/nvram_tate_usi.txt:system/etc/wifi/nvram_tate_usi.txt \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/nvram_tate_semco.txt:system/etc/wifi/nvram_tate_semco.txt \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/nvram_jem_semco.txt:system/etc/wifi/nvram_jem_semco.txt \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/nvram_jem_usi.txt:system/etc/wifi/nvram_jem_usi.txt \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/nvram_jem-wan_semco.txt:system/etc/wifi/nvram_jem-wan_semco.txt \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/nvram_jem-wan_usi.txt:system/etc/wifi/nvram_jem-wan_usi.txt \
    $(COMMON_FOLDER)/bcmdhd-wifi/firmware/nvram.txt:system/etc/wifi/bcmdhd.cal \
    $(COMMON_FOLDER)/bcmdhd-wifi/wpa_supplicant.conf:system/etc/wifi/wpa_supplicant.conf \

