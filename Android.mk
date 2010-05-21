LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

commands_recovery_local_path := $(LOCAL_PATH)

ifneq ($(TARGET_SIMULATOR),true)
ifeq ($(TARGET_ARCH),arm)

LOCAL_SRC_FILES := \
	recovery.c \
	commands.c \
	install.c \
	roots.c 

LOCAL_MODULE := install_zip

LOCAL_FORCE_STATIC_EXECUTABLE := true

RECOVERY_API_VERSION := 1.0
LOCAL_CFLAGS += -DRECOVERY_API_VERSION=$(RECOVERY_API_VERSION)

LOCAL_MODULE_TAGS := eng

LOCAL_STATIC_LIBRARIES := libminzip_two libunz libamend_two libmincrypt libmtdutils_two libcutils libstdc++ libc

include $(BUILD_EXECUTABLE)

include $(commands_recovery_local_path)/amend/Android.mk
include $(commands_recovery_local_path)/minzip/Android.mk
include $(commands_recovery_local_path)/mtdutils/Android.mk

endif   # TARGET_ARCH == arm
endif	# !TARGET_SIMULATOR
