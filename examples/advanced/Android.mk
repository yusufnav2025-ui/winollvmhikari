LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := advanced-lib
LOCAL_SRC_FILES := advanced.cpp

# Enable per-function annotation control
# This allows fine-grained obfuscation via __attribute__((annotate("...")))
LOCAL_CFLAGS += -mllvm -fncmd

# Global baseline obfuscation (applied to all non-annotated functions)
LOCAL_CFLAGS += -mllvm -fla \
                -mllvm -bcf \
                -mllvm -sub

# Enable all advanced passes (controlled per-function via annotations)
LOCAL_CFLAGS += -mllvm -bcf2 \
                -mllvm -split \
                -mllvm -mba \
                -mllvm -constenc \
                -mllvm -vmp \
                -mllvm -antidbg \
                -mllvm -antihook \
                -mllvm -icall \
                -mllvm -ibr \
                -mllvm -igv \
                -mllvm -fco \
                -mllvm -funcwrap \
                -mllvm -mergefunc

# Add logging support
LOCAL_LDLIBS := -llog -ldl

include $(BUILD_SHARED_LIBRARY)
