LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := beta-lib
LOCAL_SRC_FILES := beta.cpp

# Time expiry: expires 7 days after build
# The library will automatically refuse to load after 7 days
LOCAL_CFLAGS += -mllvm -expiry 7

# Suppress expiry message (silent crash for release)
# Remove this flag to see "Expired" message on stderr
LOCAL_CFLAGS += -mllvm -expiry-print=0

# Basic obfuscation
LOCAL_CFLAGS += -mllvm -fla -mllvm -bcf -mllvm -sub

# Anti-debugging (optional)
LOCAL_CFLAGS += -mllvm -antidbg

# Add logging support
LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
