LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := native-lib
LOCAL_SRC_FILES := native.cpp

# Basic obfuscation flags
# -fla: Control flow flattening
# -bcf: Bogus control flow
# -sub: Instruction substitution
LOCAL_CFLAGS += -mllvm -fla -mllvm -bcf -mllvm -sub

# Add logging support
LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
