# OLLVM17 - Windows Build for Android NDK r26

Complete LLVM 17.0.6 toolchain with obfuscation passes for Android development.

## Quick Start

```bash
# Clone the repository
git clone https://github.com/yusufnav2025-ui/winollvmhikari.git
cd winollvmhikari

# The toolchain is built via GitHub Actions
# Download the latest release artifact from Actions tab
```

## Complete Flag Reference

### Control Flow Obfuscation

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-fla` | Control flow flattening (switch dispatcher) | 10-15% | General-purpose obfuscation |
| `-bcf` | Bogus control flow (fake branches) | 10-15% | Add fake paths to confuse decompilers |
| `-bcf_prob=N` | BCF probability (0-100, default 30) | Variable | Control how many fake branches to add |
| `-bcf_loop=N` | BCF application rounds (default 1) | Variable | Apply BCF multiple times (1-3) |
| `-bcf2` | Enhanced bogus control flow v2 | 20-30% | More realistic fake branches |
| `-split` | Split basic blocks | 15-25% | Break code into smaller pieces |
| `-split_num=N` | Number of splits per block (default 2) | Variable | Control block fragmentation (1-10) |

### Arithmetic Obfuscation

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-sub` | Instruction substitution (replace add/xor/etc) | 10-20% | Replace simple operations with complex ones |
| `-sub_loop=N` | Substitution rounds (default 1) | Variable | Apply substitution multiple times (1-5) |
| `-mba` | Mixed Boolean Arithmetic (bit manipulation) | 15-25% | Turn math into bit operations (rbit/eor/orr) |
| `-sobf` | String obfuscation (legacy, less secure) | 5-10% | Basic string hiding (use `-strenc` instead) |
| `-strenc` | String encryption (AES) | 10-15% | Encrypt string literals at compile time |

### Data Obfuscation

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-constenc` | Constant encryption (XOR literals) | 5-10% | Hide magic numbers/API keys in binary |
| `-igv` | Indirect global variable access | 10-15% | Hide global variable references |

### Call Graph Obfuscation

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-icall` | Indirect calls (via function table) | 10-15% | Hide function call targets |
| `-ibr` | Indirect branches (computed goto) | 10-15% | Hide branch targets (switch statements) |
| `-fco` | Function call obfuscation (dlopen/dlsym) | 20-30% | Wrap external calls with dynamic loading |
| `-funcwrap` | Function wrapper (hide call graph) | 15-25% | Create wrapper functions to hide callers |
| `-mergefunc` | Merge similar functions | 10-20% | Combine similar functions into one |

### Maximum Protection

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-vmp` | Virtualization (bytecode + interpreter) | 50-200% | Convert function to VM bytecode (strongest) |
| `-antidbg` | Anti-debugging (ptrace detection) | <1% | Block Frida/IDA/GDB attachment |
| `-antihook` | Anti-hooking (inline hook detection) | <1% | Detect function prologue tampering |

### Time-Based Protection

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-expiry=N` | Time expiry in days (0=disabled) | <1% | Auto-expire beta/trial builds after N days |
| `-expiry-print=0/1` | Show expiry message (default 1) | 0% | Print "Expired" before abort (1=yes, 0=silent) |

### Per-Function Control

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-fncmd` | Enable function-name annotations | 0% | Use `__attribute__((annotate("fla bcf")))` |

### Backend Obfuscation (AArch64 Only)

| Flag | What It Does | Overhead | Use Case |
|------|--------------|----------|----------|
| `-aarch64-obfu` | AArch64 assembly obfuscation | 5-15% | Requires `asm("backend-obfu")` marker |

---

## Total Flag Count: **27 flags**

### Categories:
- **Control Flow:** 7 flags (fla, bcf, bcf_prob, bcf_loop, bcf2, split, split_num)
- **Arithmetic:** 5 flags (sub, sub_loop, mba, sobf, strenc)
- **Data:** 2 flags (constenc, igv)
- **Call Graph:** 5 flags (icall, ibr, fco, funcwrap, mergefunc)
- **Maximum Protection:** 3 flags (vmp, antidbg, antihook)
- **Time-Based:** 2 flags (expiry, expiry-print)
- **Control:** 1 flag (fncmd)
- **Backend:** 1 flag (aarch64-obfu)
- **Legacy:** 1 flag (sobf - deprecated)

---

## Flag Combinations by Use Case

### 1. Basic Protection (All Apps)
```bash
-mllvm -fla -mllvm -bcf -mllvm -sub
```
**Result:** 10-15% overhead, good baseline

### 2. Moderate Protection (Most Apps)
```bash
-mllvm -fla -mllvm -bcf2 -mllvm -sub -mllvm -split -mllvm -mba
```
**Result:** 25-40% overhead, strong obfuscation

### 3. License Validation (Critical Functions)
```cpp
__attribute__((annotate("vmp antidbg antihook constenc")))
bool checkLicense(const char* key);
```
```bash
-mllvm -fncmd -mllvm -vmp -mllvm -antidbg -mllvm -antihook -mllvm -constenc
```
**Result:** 100%+ overhead on this function only

### 4. API Key Protection
```cpp
__attribute__((annotate("constenc igv")))
const char API_KEY[] = "secret-key-here";
```
```bash
-mllvm -fncmd -mllvm -constenc -mllvm -igv
```
**Result:** Key not visible in `strings` output

### 5. Beta/Trial Builds
```bash
-mllvm -expiry 14 -mllvm -expiry-print=1
```
**Result:** Auto-expires 14 days after build

### 6. Maximum Security (Banking/DRM)
```bash
-mllvm -fla -mllvm -bcf2 -mllvm -sub -mllvm -split -mllvm -mba \
-mllvm -constenc -mllvm -icall -mllvm -ibr -mllvm -antidbg -mllvm -antihook
```
**Result:** 50-70% overhead globally

### 7. VM-Protected Critical Path
```cpp
__attribute__((annotate("vmp antidbg antihook")))
void criticalOperation() {
    // Entire function converted to bytecode
}
```
```bash
-mllvm -fncmd -mllvm -vmp -mllvm -antidbg -mllvm -antihook
```
**Result:** 200%+ overhead, maximum protection

---

## Performance Impact Summary

| Obfuscation Level | Flags | Overhead | Build Time |
|-------------------|-------|----------|------------|
| **None** | (baseline) | 0% | 1x |
| **Light** | `-fla` | 10% | 1.1x |
| **Basic** | `-fla -bcf -sub` | 15% | 1.2x |
| **Medium** | `-fla -bcf2 -sub -mba` | 30% | 1.5x |
| **Heavy** | Medium + `-split -constenc -icall` | 50% | 2x |
| **Maximum** | Heavy + `-vmp` (1-2 functions) | 100%+ | 4-5x |

---

## Usage Examples

See the [examples/](examples/) folder for complete, buildable Android JNI projects:

- **[examples/basic](examples/basic/)** - Getting started with core flags
- **[examples/crypto](examples/crypto/)** - License validation + VM protection
- **[examples/expiry](examples/expiry/)** - Time-limited beta/trial builds
- **[examples/advanced](examples/advanced/)** - All 6 protection levels

---

## Build System Integration

### Android.mk
```makefile
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := mylib
LOCAL_SRC_FILES := mylib.cpp

# Basic obfuscation
LOCAL_CFLAGS += -mllvm -fla -mllvm -bcf -mllvm -sub

# Enable per-function control
LOCAL_CFLAGS += -mllvm -fncmd

# Advanced passes (controlled via annotations)
LOCAL_CFLAGS += -mllvm -vmp -mllvm -antidbg -mllvm -constenc

# Time expiry (14-day beta)
LOCAL_CFLAGS += -mllvm -expiry 14

include $(BUILD_SHARED_LIBRARY)
```

### CMakeLists.txt
```cmake
add_library(mylib SHARED mylib.cpp)

# Basic obfuscation
target_compile_options(mylib PRIVATE
    -mllvm -fla
    -mllvm -bcf
    -mllvm -sub
)

# Enable per-function control
target_compile_options(mylib PRIVATE -mllvm -fncmd)

# Advanced passes
target_compile_options(mylib PRIVATE
    -mllvm -vmp
    -mllvm -antidbg
    -mllvm -constenc
)

# Time expiry (30-day trial)
target_compile_options(mylib PRIVATE
    -mllvm -expiry 30
    -mllvm -expiry-print=0  # Silent expiry
)
```

### Gradle (via externalNativeBuild)
```gradle
android {
    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags "-mllvm", "-fla", "-mllvm", "-bcf", "-mllvm", "-sub"
                // Add more flags as needed
            }
        }
    }
}
```

---

## Per-Function Annotations

Enable with `-mllvm -fncmd`, then annotate functions:

```cpp
// Light obfuscation
__attribute__((annotate("fla bcf")))
int simpleFunction(int x) {
    return x * 2;
}

// Heavy obfuscation
__attribute__((annotate("fla bcf2 sub mba constenc")))
__attribute__((annotate("sub_loop=3 split_num=5")))
int sensitiveFunction(int x) {
    const int secret = 0x12345678;  // Encrypted!
    return x ^ secret;
}

// VM protection
__attribute__((annotate("vmp antidbg antihook")))
bool validateLicense(const char* key) {
    // Entire function in VM bytecode
    return checkKey(key);
}

// No obfuscation (even with global flags)
__attribute__((annotate("noobf")))
void performanceHotPath() {
    // Skips all obfuscation passes
}
```

**Available annotation keywords:**
```
fla bcf bcf2 sub split mba constenc strenc
icall ibr igv fco funcwrap mergefunc
vmp antidbg antihook
noobf
```

**Parameters:**
```
bcf_prob=N bcf_loop=N sub_loop=N split_num=N
```

---

## Verification

### Check Obfuscation Applied
```bash
# Flatten check (should see switch-case dispatcher)
llvm-objdump -d libmylib.so | grep -A 30 "myFunction"

# Constant encryption check (literals not visible)
strings libmylib.so | grep "12345678"  # Should return nothing

# VM protection check (no standard prologue)
llvm-objdump -d libmylib.so | grep -A 50 "validateLicense"
# Look for bytecode fetch/decode pattern instead of normal asm

# Anti-debug check (syscall #117 = ptrace)
llvm-objdump -d libmylib.so | grep -B 5 -A 5 "mov.*#117"

# Expiry check (global constructor exists)
llvm-nm libmylib.so | grep "__expiry_guard"
```

### Measure Overhead
```bash
# Baseline build
ndk-build APP_CFLAGS=""
adb shell "time /data/local/tmp/test_baseline"

# Obfuscated build
ndk-build APP_CFLAGS="-mllvm -fla -mllvm -bcf -mllvm -sub"
adb shell "time /data/local/tmp/test_obfuscated"

# Compare execution time
```

---

## Troubleshooting

### Build Errors

**Error:** `unknown argument: '-fla'`
- **Cause:** Using stock clang instead of OLLVM clang
- **Fix:** Set `NDK_TOOLCHAIN_VERSION` or use full path to OLLVM toolchain

**Error:** `Build takes forever (>30 min)`
- **Cause:** VMP applied to too many functions
- **Fix:** Only annotate 1-2 critical functions with `vmp`

**Error:** `Binary size increased 10x`
- **Cause:** VMP creates large bytecode arrays + interpreter
- **Fix:** Reduce VMP usage or use `-mllvm -strip` to remove debug info

### Runtime Errors

**Error:** App crashes on startup (no logcat)
- **Cause:** Time expiry triggered (silent mode)
- **Fix:** Rebuild or check `llvm-nm` for `__expiry_guard`

**Error:** `SIGILL` (illegal instruction)
- **Cause:** Backend obfuscation on non-AArch64 target
- **Fix:** Only use `-aarch64-obfu` for arm64-v8a builds

**Error:** Performance degraded >100%
- **Cause:** VMP or heavy obfuscation in hot loop
- **Fix:** Profile with `simpleperf`, annotate hot functions with `noobf`

---

## FAQ

**Q: Which flags should I start with?**  
**A:** Start with `-fla -bcf -sub` (basic protection, 15% overhead).

**Q: Can I use all flags at once?**  
**A:** Yes, but build time will be 3-5x longer and runtime overhead 50-100%.

**Q: When should I use VMP?**  
**A:** Only for 1-2 critical functions (license checks, crypto). VMP overhead is 100-200%.

**Q: How do I obfuscate only one function?**  
**A:** Use `-mllvm -fncmd` + `__attribute__((annotate("fla bcf")))`.

**Q: Does this work on x86/x86_64?**  
**A:** Yes, but `-antidbg`/`-antihook`/`-aarch64-obfu` only work on AArch64.

**Q: Can I remove obfuscation for debug builds?**  
**A:** Yes, add flags only in `release` build type.

**Q: How do I test time expiry?**  
**A:** Build with `-expiry 0.0007` (~1 minute), wait, then test.

---

## Credits

- **DreamSoule/ollvm17** - LLVM 17 port
- **Hikari Obfuscator** - Original passes
- **Polaris Additions** - LinearMBA, AliasAccess, JunkCode, BCF2, MergeFunction, AntiDebugging, AntiHooking, FunctionWrapper, ConstantEncryption, TimeExpiry

---

## License

See upstream projects:
- LLVM Project - Apache 2.0 with LLVM Exceptions
- Hikari - BSD-style license
