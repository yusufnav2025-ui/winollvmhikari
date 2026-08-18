# Basic Obfuscation Example

Simple Android JNI library with basic obfuscation flags.

## Features
- Control flow flattening (`-fla`)
- Bogus control flow (`-bcf`)
- Instruction substitution (`-sub`)

## Build with Android.mk

```bash
cd examples/basic
ndk-build
```

Output: `libs/arm64-v8a/libnative-lib.so`

## Build with CMake

```bash
cd examples/basic
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
         -DANDROID_ABI=arm64-v8a \
         -DANDROID_PLATFORM=android-21
make
```

## Expected Behavior
- All functions obfuscated with basic passes
- ~10-15% performance overhead
- Build time: 1.2x baseline

## Verify Obfuscation

```bash
# Disassemble the library
llvm-objdump -d libnative-lib.so | grep -A 20 "stringFromJNI"

# You should see:
# - Switch tables (from -fla)
# - Extra branches (from -bcf)
# - Complex arithmetic (from -sub)
```
