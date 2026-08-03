LOCAL_PATH := $(call my-dir)

# lxgr_dump CLI (standalone)
# Build:  $NDK/ndk-build   ->  libs/arm64-v8a/lxgr_dump
# Link lxgr_dump.c into your own project (as the overlay does) and ignore
# main.c.

include $(CLEAR_VARS)
LOCAL_MODULE    := lxgr_dump
LOCAL_CFLAGS    := -O3 -w -fvisibility=hidden -ffunction-sections -fdata-sections
LOCAL_LDFLAGS   += -Wl,--gc-sections,--strip-all
LOCAL_SRC_FILES := lxgr_dump.c lxgr_il2cpp.c main.c
# enable the embedded il2cpp name index when the generated header is present
ifneq ($(wildcard $(LOCAL_PATH)/il2cpp_index.h),)
LOCAL_CFLAGS += -DLXGR_IL2CPP_INDEX
endif
include $(BUILD_EXECUTABLE)
