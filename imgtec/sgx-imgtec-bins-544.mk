# Graphics/IMGTEC
SGX_LOCAL_PATH := $(COMMON_FOLDER)/imgtec

SGX_MODULES:
	make clean -C $(SGX_LOCAL_PATH)/eurasia_km/eurasiacon/build/linux2/omap4430_android
	cp $(TARGET_KERNEL_SOURCE)/drivers/video/omap2/omapfb/omapfb.h $(KERNEL_OUT)/drivers/video/omap2/omapfb/omapfb.h
	make -j8 -C $(SGX_LOCAL_PATH)/eurasia_km/eurasiacon/build/linux2/omap4430_android ARCH=arm KERNEL_CROSS_COMPILE=arm-eabi- CROSS_COMPILE=arm-eabi- KERNELDIR=$(KERNEL_OUT) TARGET_PRODUCT="blaze_tablet" BUILD=release TARGET_SGX=544sc PLATFORM_VERSION=4.0
	mv $(KERNEL_OUT)/../../target/kbuild/pvrsrvkm_sgx544_112.ko $(KERNEL_MODULES_OUT)

TARGET_KERNEL_MODULES += SGX_MODULES

# Graphics
PRODUCT_COPY_FILES += \
    $(SGX_LOCAL_PATH)/lib/hw/gralloc.omap4470.so:/system/vendor/lib/hw/gralloc.omap4.so \
    $(SGX_LOCAL_PATH)/lib/egl/libEGL_POWERVR_SGX544_112.so:/system/vendor/lib/egl/libEGL_POWERVR_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/egl/libGLESv1_CM_POWERVR_SGX544_112.so:/system/vendor/lib/egl/libGLESv1_CM_POWERVR_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/egl/libGLESv2_POWERVR_SGX544_112.so:/system/vendor/lib/egl/libGLESv2_POWERVR_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libglslcompiler_SGX544_112.so:/system/vendor/lib/libglslcompiler_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libIMGegl_SGX544_112.so:/system/vendor/lib/libIMGegl_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libpvr2d_SGX544_112.so:/system/vendor/lib/libpvr2d_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libpvrANDROID_WSEGL_SGX544_112.so:/system/vendor/lib/libpvrANDROID_WSEGL_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libPVRScopeServices_SGX544_112.so:/system/vendor/lib/libPVRScopeServices_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libsrv_init_SGX544_112.so:/system/vendor/lib/libsrv_init_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libsrv_um_SGX544_112.so:/system/vendor/lib/libsrv_um_SGX544_112.so \
    $(SGX_LOCAL_PATH)/lib/libusc_SGX544_112.so:/system/vendor/lib/libusc_SGX544_112.so \
    $(SGX_LOCAL_PATH)/bin/pvrsrvinit:/system/vendor/bin/pvrsrvinit \
    $(SGX_LOCAL_PATH)/bin/pvrsrvctl_SGX544_112:/system/vendor/bin/pvrsrvctl_SGX544_112 \
    $(SGX_LOCAL_PATH)/etc/powervr.ini:/system/etc/powervr.ini \

