LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := crypto-lib
LOCAL_SRC_FILES := crypto.cpp

# Heavy obfuscation for crypto operations
# -fla: Control flow flattening
# -bcf2: Enhanced bogus control flow
# -sub: Instruction substitution
# -sub_loop=3: Apply substitution 3 times
# -mba: Mixed Boolean Arithmetic (bit manipulation)
# -constenc: Encrypt constants (magic numbers)
# -antidbg: Anti-debugging checks
# -antihook: Anti-hooking detection
# -vmp: Virtualization (VM-protect critical functions)
LOCAL_CFLAGS += -mllvm -fla \
                -mllvm -bcf2 \
                -mllvm -sub \
                -mllvm -sub_loop=3 \
                -mllvm -mba \
                -mllvm -constenc \
                -mllvm -antidbg \
                -mllvm -antihook \
                -mllvm -vmp

# Enable per-function control
LOCAL_CFLAGS += -mllvm -fncmd

# Add logging support
LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
