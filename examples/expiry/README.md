# Time Expiry Example

Beta/trial builds that automatically expire after N days.

## How It Works

### Compile-Time Timestamp
When you build the library:
```bash
# Build on August 17, 2026 at 10:00 AM
ndk-build  # Captures timestamp: 1723892400 (Unix seconds)
```

The pass injects:
- `@__build_timestamp` global constant = 1723892400
- `@__expiry_minutes` global constant = 10080 (7 days × 1440 min/day)
- `__expiry_guard()` function (heavily obfuscated + VM-protected)
- Global constructor that calls `__expiry_guard()` before `JNI_OnLoad`

### Runtime Check
When someone loads the library:
```java
System.loadLibrary("beta-lib");  // <-- Expiry check happens HERE
```

1. Global constructor runs (priority 101)
2. `__expiry_guard()` executes:
   - Calls `clock_gettime()` via AArch64 SVC syscall #113
   - Loads `@__build_timestamp` (compile time)
   - Computes deadline: `build_time + (7 × 86400) seconds`
   - Compares current time vs deadline
3. **If expired:** Print "Expired\n" (if `-expiry-print=1`) + `abort()`
4. **If valid:** Constructor returns, library loads normally

### Time Source
Uses **raw syscall** (not libc):
```asm
mov x8, #113        ; clock_gettime syscall
mov x0, #0          ; CLOCK_REALTIME
mov x1, sp          ; struct timespec* on stack
svc #0              ; Direct kernel call
```

Why? Syscalls cannot be hooked by Frida/Xposed (no PLT/GOT entry).

## Build Examples

### 7-Day Beta (Silent)
```bash
# Android.mk
LOCAL_CFLAGS += -mllvm -expiry 7 -mllvm -expiry-print=0
```
**Result:** Expires 7 days after build, crashes silently

### 30-Day Trial (With Message)
```bash
# CMakeLists.txt
target_compile_options(beta-lib PRIVATE
    -mllvm -expiry 30
    -mllvm -expiry-print=1  # Show "Expired" message
)
```
**Result:** Expires after 30 days, prints message before crash

### 1-Day Debug Build
```bash
ndk-build LOCAL_CFLAGS="-mllvm -expiry 1"
```
**Result:** Expires tomorrow at the same time

### 90-Day Release
```bash
# For production trials
-mllvm -expiry 90 -mllvm -expiry-print=0
```

## Testing Expiry

### Method 1: Wait (Not Practical)
```bash
# Build, wait 7 days, test
ndk-build
# ... wait 7 days ...
adb push libs/arm64-v8a/libbeta-lib.so /data/local/tmp/
adb shell "LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/test"
# Output: Aborted (or "Expired" if print=1)
```

### Method 2: Mock Build Time (Advanced)
You can't easily mock this without modifying the pass, because the timestamp is baked in at compile time. Options:

**A. Rebuild with short expiry:**
```bash
# Expire in 1 minute for testing
ndk-build LOCAL_CFLAGS="-mllvm -expiry 0.0007"  # ~1 minute
# Build completes at 10:00:00
# Test at 10:01:00 - should be expired
```

**B. Modify system time on device (requires root):**
```bash
# Build with -expiry 7
ndk-build
adb root
adb shell "date 082517002026"  # Set to Aug 25 (8 days later)
adb push libs/arm64-v8a/libbeta-lib.so /data/local/tmp/
adb shell "LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/test"
# Should crash (expired)
adb shell "date 081717002026"  # Restore to Aug 17
```

**C. Verify in IDA/Ghidra (before expiry):**
```bash
llvm-objdump -d libbeta-lib.so | grep -A 100 "__expiry_guard"
# You'll see:
# - Obfuscated control flow (no clear if/else)
# - SVC #0 syscall (clock_gettime)
# - Magic constants (build timestamp XOR-encrypted)
# - VM bytecode dispatch (if VMP was applied)
```

## Expected Behavior

### Before Expiry (Day 0-6)
```java
System.loadLibrary("beta-lib");  // Loads successfully
String info = getBuildInfo();     // Returns "Beta Build - Time-Limited"
boolean active = isActive();      // Returns true
```
**Logcat:**
```
I/ExpiryDemo: Library loaded successfully - not expired
```

### After Expiry (Day 7+)
```java
System.loadLibrary("beta-lib");  // Crashes here (JNI_OnLoad never called)
// App never reaches next line
```

**Logcat (if -expiry-print=1):**
```
Expired
A/libc: Fatal signal 6 (SIGABRT)
```

**Logcat (if -expiry-print=0 - silent):**
```
A/libc: Fatal signal 6 (SIGABRT)
```

## Security Features

### Syscall-Based Time Check
- Cannot hook `time()` / `gettimeofday()` via Frida
- No PLT/GOT entry to redirect
- Requires kernel-level hook (needs root + custom kernel)

### Obfuscation
The expiry check function is automatically protected:
```cpp
// Pass injects this annotation:
__attribute__((annotate("fla bcf2 sub mba constenc vmp")))
void __expiry_guard() { ... }
```

**Result:**
- No clear if/else branches
- Build timestamp encrypted (not visible in `strings`)
- VM-protected (bytecode interpreter)
- Comparison logic obfuscated (MBA transformations)

### Early Check
Runs in global constructor (priority 101):
- Executes before `JNI_OnLoad`
- Executes before any user code
- Cannot be bypassed by patching Java layer

### Randomization
Each build uses different:
- Register assignments (x0-x7 shuffled)
- SVC immediate values
- Comparison order (deadline < current vs current > deadline)
- XOR keys for timestamp encryption

## Common Scenarios

### Beta Testing
```bash
# 14-day beta builds
-mllvm -expiry 14 -mllvm -expiry-print=1

# Testers see clear message when expired
# Reminder to download latest build
```

### Trial Licenses
```bash
# 30-day trial
-mllvm -expiry 30 -mllvm -expiry-print=0

# Silent expiry = better UX (show dialog in Java layer instead)
try {
    System.loadLibrary("beta-lib");
} catch (UnsatisfiedLinkError e) {
    // Show "Trial expired" dialog
}
```

### Internal Dogfooding
```bash
# 90-day internal builds
-mllvm -expiry 90

# Prevents old internal builds from lingering
# Forces developers to use latest version
```

## Flag Reference

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `-expiry` | int | 0 (disabled) | Days until expiry (1-365+) |
| `-expiry-print` | bool | 1 (enabled) | Print "Expired\n" to stderr |

### Examples
```bash
-mllvm -expiry 0               # Disabled (no check injected)
-mllvm -expiry 1               # Expires tomorrow
-mllvm -expiry 7               # Expires in 1 week
-mllvm -expiry 30              # Expires in 1 month
-mllvm -expiry 365             # Expires in 1 year

# Control message
-mllvm -expiry 7 -mllvm -expiry-print=1   # Show "Expired"
-mllvm -expiry 7 -mllvm -expiry-print=0   # Silent abort
```

## Limitations

### Cannot Change After Build
Expiry is **hardcoded at compile time**:
- Cannot extend trial remotely
- Cannot disable check via config file
- Requires rebuild to change expiry period

### System Time Dependency
Uses system clock (CLOCK_REALTIME):
- User can change device date (requires root on locked devices)
- Not secure against determined attackers
- Best for honest users / beta testers

### Not Cryptographic
The time check is **obfuscated**, not **cryptographically secure**:
- Can be patched by skilled reverse engineers
- Syscall can be hooked (requires root + kernel module)
- Build timestamp can be found (with effort)

## Best Practices

✅ **Do:**
- Use for beta/trial builds
- Combine with server-side validation
- Set expiry-print=0 for release trials
- Use short expiry (7-30 days) to prevent stale builds
- Rebuild monthly for internal dogfood builds

❌ **Don't:**
- Rely on this alone for license enforcement
- Use extreme expiry (365+ days - defeats purpose)
- Forget to rebuild expired internal tools
- Expect 100% tamper-proof protection

## Troubleshooting

### Library Crashes Immediately
**Symptom:** `System.loadLibrary()` crashes, no logcat message
**Cause:** Build expired (and `-expiry-print=0`)
**Fix:** Rebuild the library

### "Expired" Message But Should Be Valid
**Symptom:** Library crashes with "Expired" within expiry window
**Possible causes:**
1. Device clock set to future date
2. Build timezone vs device timezone mismatch
3. Build timestamp captured incorrectly

**Debug:**
```bash
# Check device time
adb shell "date"

# Check build artifact timestamp
ls -l libs/arm64-v8a/libbeta-lib.so

# If mismatch, rebuild or fix device clock
```

### Expiry Check Not Working
**Symptom:** Library loads fine after expiry period
**Possible causes:**
1. `-expiry 0` (disabled)
2. Flag not passed to compiler
3. CMake cache issue

**Debug:**
```bash
# Verify flag in build log
grep "expiry" build.log

# Check if __expiry_guard exists
llvm-nm libbeta-lib.so | grep expiry

# Force clean rebuild
ndk-build clean && ndk-build -B
```

## Performance Impact

| Component | Overhead |
|-----------|----------|
| Global constructor | +0.5ms at library load |
| Syscall (clock_gettime) | ~50μs |
| Obfuscation overhead | Negligible (runs once) |
| VM protection | +2-5ms if VMP applied |

**Total:** <5ms added to library load time (one-time cost).
