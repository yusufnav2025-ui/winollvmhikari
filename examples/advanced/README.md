# Advanced Obfuscation Example

Demonstrates all available obfuscation passes with per-function control.

## Overview

This example shows **6 protection levels** from basic to maximum:

| Level | Techniques | Overhead | Use Case |
|-------|------------|----------|----------|
| 1 | `fla` or `bcf` | 5-10% | Non-critical functions |
| 2 | `fla bcf sub mba` | 15-25% | Moderate protection |
| 3 | `fla bcf2 sub split constenc` | 30-50% | Sensitive operations |
| 4 | `vmp` (VM protection) | 50-100% | License validation |
| 5 | `icall ibr igv` | 10-20% | Hide call graph |
| 6 | `fco funcwrap mergefunc` | 20-40% | Function-level hiding |

## Per-Function Annotations

Enable with `-mllvm -fncmd`, then annotate functions:

```cpp
__attribute__((annotate("fla bcf sub")))
int myFunction(int x) {
    return x * 2;
}
```

### Available Keywords

**Control Flow:**
- `fla` — Control flow flattening (switch dispatcher)
- `bcf` — Bogus control flow (fake branches)
- `bcf2` — Enhanced bogus control flow v2

**Arithmetic:**
- `sub` — Instruction substitution (replace add/xor/etc)
- `sub_loop=N` — Apply substitution N times (1-5)
- `mba` — Mixed Boolean Arithmetic (bit manipulation)

**Block-Level:**
- `split` — Split basic blocks
- `split_num=N` — Number of splits (1-10)

**Data:**
- `constenc` — Constant encryption (XOR literals)

**Advanced:**
- `vmp` — Virtualization (bytecode + interpreter)
- `antidbg` — Anti-debugging checks
- `antihook` — Anti-hooking detection

**Call Graph:**
- `icall` — Indirect calls
- `ibr` — Indirect branches
- `igv` — Indirect global variables
- `fco` — Function call obfuscation (dlopen/dlsym)
- `funcwrap` — Function wrapper
- `mergefunc` — Merge similar functions

## Level 1: Basic (5-10% overhead)

### Single Pass
```cpp
__attribute__((annotate("fla")))
int add(int a, int b) {
    return a + b;
}
```

**Result:**
- Linear code → switch-case dispatcher
- All paths lead to same result
- Confuses decompilers (IDA shows messy CFG)

### Bogus Control Flow
```cpp
__attribute__((annotate("bcf")))
int multiply(int a, int b) {
    return a * b;
}
```

**Result:**
- Extra conditional branches
- Dead code blocks (never executed)
- CFG looks complex but behaves normally

## Level 2: Medium (15-25% overhead)

### Balanced Protection
```cpp
__attribute__((annotate("fla bcf sub mba")))
int hash(const char* str) {
    int h = 5381;
    while (*str) {
        h = ((h << 5) + h) + (*str++);
    }
    return h;
}
```

**Result:**
- Flattened control flow
- Bogus branches
- Arithmetic operations replaced (+ becomes xor/add combo)
- Bit manipulation (shifts/rotates)

**Disassembly:**
```asm
; Before (clear):
add w8, w9, w8, lsl #5

; After (obfuscated):
eor w10, w9, w8
and w11, w9, w8
add w8, w10, w11, lsl #1
lsl w8, w8, #4
```

## Level 3: Heavy (30-50% overhead)

### Enhanced Protection
```cpp
__attribute__((annotate("fla bcf2 sub split constenc")))
__attribute__((annotate("sub_loop=3 split_num=5")))
int checksum(const unsigned char* data, int len) {
    unsigned int crc = 0xFFFFFFFF;
    const unsigned int poly = 0xEDB88320;  // Encrypted!
    
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? poly : 0);
        }
    }
    return ~crc;
}
```

**Result:**
- Constants encrypted: `0xEDB88320` not visible in strings
- Basic blocks split into 5 pieces
- Substitution applied 3 times (layered obfuscation)
- Enhanced BCF v2 (more realistic fake branches)

**Binary analysis:**
```bash
strings libadvanced-lib.so | grep "EDB88320"
# (nothing found - constant is encrypted)

llvm-objdump -d libadvanced-lib.so | grep -A 50 "checksum"
# Shows 5 separate blocks instead of 1 loop
```

## Level 4: Maximum (50-100% overhead)

### VM Protection
```cpp
__attribute__((annotate("fla bcf2 sub split mba constenc vmp")))
__attribute__((annotate("sub_loop=3 split_num=5")))
bool validateLicense(const char* key) {
    const unsigned int magic1 = 0xDEADBEEF;
    const unsigned int magic2 = 0xCAFEBABE;
    
    unsigned int hash = magic2;
    while (*key) {
        hash = ((hash << 5) + hash) ^ (*key++);
        hash += magic1;
    }
    
    return (hash ^ magic1) == 0x12345678;
}
```

**Result:**
- **Entire function converted to bytecode**
- Custom VM interpreter embedded
- Original assembly removed completely
- Extreme anti-disassembly protection

**Disassembly shows:**
```asm
; No function prologue/epilogue
; Just VM dispatcher:
vm_dispatch:
    ldr x8, [x19, #8]    ; Fetch bytecode
    ldrb w9, [x8]        ; Decode opcode
    adr x10, vm_handlers
    ldr x10, [x10, x9, lsl #3]
    br x10               ; Jump to handler
```

### VM + Anti-Debugging
```cpp
__attribute__((annotate("vmp antidbg antihook")))
void criticalOperation() {
    // Blocks debuggers THEN runs in VM
}
```

**Execution flow:**
1. Anti-debug check (ptrace syscall)
2. Anti-hook check (prologue validation)
3. If clean → execute VM bytecode
4. If tampered → abort()

## Level 5: Call Obfuscation (10-20% overhead)

### Indirect Calls
```cpp
__attribute__((annotate("icall")))
int callFunction(int (*fn)(int, int), int a, int b) {
    return fn(a, b);  // Call target hidden
}
```

**Before:**
```asm
blr x8  ; Direct call
```

**After:**
```asm
adr x9, __icall_table
ldr x10, [x9, x8, lsl #3]
blr x10  ; Indirect via table
```

### Indirect Branches
```cpp
__attribute__((annotate("ibr")))
int switchCase(int choice) {
    switch (choice) {
        case 0: return 100;
        case 1: return 200;
        case 2: return 300;
        default: return 0;
    }
}
```

**Before:**
```asm
tbz w0, #1, .L2
b .L3
```

**After:**
```asm
adr x8, __ibr_table
ldr x9, [x8, w0, uxtw #3]
br x9  ; Computed goto
```

### Indirect Globals
```cpp
__attribute__((annotate("igv")))
int globalVar = 42;

int getGlobal() {
    return globalVar;  // Access obfuscated
}
```

**Before:**
```asm
adrp x8, globalVar
ldr w0, [x8, :lo12:globalVar]
```

**After:**
```asm
adr x8, __igv_table
ldr x9, [x8, #offset]
ldr w0, [x9]  ; Double indirection
```

## Level 6: Function-Level (20-40% overhead)

### Function Call Obfuscation
```cpp
__attribute__((annotate("fco")))
extern int external_function(int x);

int callExternal(int value) {
    return external_function(value);
}
```

**Result:**
- `external_function` called via dlopen/dlsym
- No direct PLT/GOT entry
- Call target resolved at runtime

**Generated code:**
```cpp
void* handle = dlopen("libc.so", RTLD_LAZY);
int (*fn)(int) = (int(*)(int))dlsym(handle, "external_function");
return fn(value);
```

### Function Wrapper
```cpp
__attribute__((annotate("funcwrap")))
int wrapped(int a, int b) {
    return a * b + 100;
}
```

**Result:**
- Wrapper function created
- Original function hidden
- Call graph obfuscated

**Before:**
```
caller() → wrapped()
```

**After:**
```
caller() → __wrapper_stub() → wrapped_impl()
```

### Function Merging
```cpp
__attribute__((annotate("mergefunc")))
int merge1(int x) { return x * 2; }

__attribute__((annotate("mergefunc")))
int merge2(int x) { return x * 3; }
```

**Result:**
- Similar functions merged into one
- Dispatch via parameter
- Reduces function count (harder to analyze)

**After:**
```cpp
int merged(int x, int selector) {
    switch (selector) {
        case 0: return x * 2;
        case 1: return x * 3;
    }
}
```

## Build

### Android.mk
```bash
cd examples/advanced
ndk-build
```

### CMake
```bash
cd examples/advanced
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
         -DANDROID_ABI=arm64-v8a \
         -DANDROID_PLATFORM=android-21
make
```

**Build time:** 2-3x baseline (VMP functions are slow to compile)

## Usage in Java

```java
public class AdvancedLib {
    static {
        System.loadLibrary("advanced-lib");
    }

    // Level 1: Basic obfuscation
    public native int testLevel1(int a, int b);

    // Level 2: Medium obfuscation
    public native int testLevel2(String str);

    // Level 3: Heavy obfuscation
    public native int testLevel3(byte[] data);

    // Level 4: VM protection
    public native boolean testLevel4(String key);
    public native void testLevel4Critical();

    // Level 5: Call obfuscation
    public native int testLevel5(int choice);
}

// Example usage
AdvancedLib lib = new AdvancedLib();

// Basic functions - fast
int sum = lib.testLevel1(10, 20);

// Medium protection - moderate overhead
int hash = lib.testLevel2("hello");

// Heavy protection - slower
byte[] data = {1, 2, 3, 4};
int crc = lib.testLevel3(data);

// VM protection - slowest but most secure
boolean valid = lib.testLevel4("license-key");

// Call obfuscation
int result = lib.testLevel5(1);
```

## Performance Comparison

Benchmark on Snapdragon 888 (AArch64):

| Function | Obfuscation | Native Time | Obfuscated Time | Overhead |
|----------|-------------|-------------|-----------------|----------|
| `level1_add` | `fla` | 5ns | 5.5ns | +10% |
| `level1_multiply` | `bcf` | 4ns | 4.4ns | +10% |
| `level2_hash` | `fla bcf sub mba` | 120ns | 150ns | +25% |
| `level3_checksum` | Heavy | 2.5μs | 4.2μs | +68% |
| `level4_validateLicense` | `vmp` | 800ns | 1.6μs | +100% |
| `level5_switch` | `ibr` | 8ns | 10ns | +25% |

**Takeaway:** Use VMP sparingly (1-2 critical functions only).

## Verification

### Check VM Protection
```bash
llvm-objdump -d libadvanced-lib.so | grep -A 100 "validateLicense"

# You should see:
# - No standard function prologue (stp x29, x30, [sp, #-16]!)
# - VM dispatcher loop
# - Bytecode fetch/decode pattern
# - Handler table
```

### Check Constant Encryption
```bash
strings libadvanced-lib.so | grep -E "DEADBEEF|CAFEBABE"
# (should return nothing)

# Verify constants are XOR-encrypted
hexdump -C libadvanced-lib.so | grep -C 5 "$(printf '%08x' $((0xDEADBEEF ^ 0x12345678)))"
```

### Check Call Obfuscation
```bash
llvm-nm libadvanced-lib.so | grep external_function
# (should NOT appear in symbol table)

llvm-objdump -d libadvanced-lib.so | grep -A 20 "callExternal"
# Should see dlopen/dlsym calls instead of direct branch
```

## Recommended Combinations

### License Validation
```cpp
__attribute__((annotate("vmp antidbg antihook constenc")))
bool checkLicense(const char* key);
```
**Why:** VM protects logic, anti-debug blocks tools, constenc hides keys

### Crypto Operations
```cpp
__attribute__((annotate("fla bcf2 sub mba constenc")))
void encrypt(uint8_t* data, size_t len, const uint8_t* key);
```
**Why:** Heavy obfuscation without VMP overhead

### API Key Storage
```cpp
__attribute__((annotate("constenc igv")))
const char API_KEY[] = "secret";
```
**Why:** Key encrypted, access obfuscated

### Anti-Cheat
```cpp
__attribute__((annotate("antidbg antihook")))
void detectCheat();
```
**Why:** Blocks debuggers/hooks early

### Hot Loop (Performance-Critical)
```cpp
__attribute__((annotate("fla")))  // Light obfuscation only
void renderFrame();
```
**Why:** Minimal overhead for frequently called code

## Tips

1. **Start light, add gradually:** Begin with `fla bcf`, add more if needed
2. **Profile first:** Measure overhead before deploying heavy protection
3. **VM is expensive:** Only use for 1-2 critical functions
4. **Combine strategically:** `vmp + antidbg + antihook` for max protection
5. **Test on device:** Emulator performance != real device
6. **Watch build time:** VMP can add 10+ minutes to clean builds

## Common Pitfalls

❌ **Don't:**
- Apply VMP to hot loops (100x overhead)
- Use `sub_loop=5` everywhere (diminishing returns)
- Obfuscate printf/logging (breaks at runtime)
- Mix incompatible passes (some conflict)

✅ **Do:**
- Use annotations selectively
- Test each level incrementally
- Keep VMP for 1-2 functions
- Profile before/after
- Document annotation choices
