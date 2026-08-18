# Crypto + License Validation Example

Heavy obfuscation for sensitive operations (license checks, encryption).

## Features

### Per-Function Obfuscation
- **validateLicense()**: VM-protected (`vmp`) + full obfuscation suite
- **xorEncrypt()**: Math obfuscation + constant encryption
- **xorDecrypt()**: Light obfuscation (performance-critical)
- **checkLicense()**: Anti-debugging + anti-hooking wrapper

### Global Flags
- Control flow flattening (`-fla`)
- Enhanced bogus control flow (`-bcf2`)
- Instruction substitution x3 (`-sub -sub_loop=3`)
- Mixed Boolean Arithmetic (`-mba`)
- Constant encryption (`-constenc`)
- Anti-debugging (`-antidbg`)
- Anti-hooking (`-antihook`)
- Virtualization (`-vmp`)

## Build

### Android.mk
```bash
cd examples/crypto
ndk-build
```

### CMake
```bash
cd examples/crypto
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
         -DANDROID_ABI=arm64-v8a \
         -DANDROID_PLATFORM=android-21
make
```

**Build time:** 3-5x baseline (due to VMP)

## Usage in Java

```java
public class CryptoLib {
    static {
        System.loadLibrary("crypto-lib");
    }

    // License validation (VM-protected)
    public native boolean checkLicense(String key);

    // Encryption (heavily obfuscated)
    public native byte[] encrypt(byte[] data, byte[] key);

    // Decryption (lighter obfuscation)
    public native byte[] decrypt(byte[] data, byte[] key);

    // Hash function
    public native int hash(String input);
}

// Example usage
CryptoLib crypto = new CryptoLib();

// Check license (blocks debuggers first)
if (!crypto.checkLicense("my-license-key")) {
    System.exit(1);
}

// Encrypt sensitive data
byte[] data = "secret".getBytes();
byte[] key = {(byte)0xAA, (byte)0xBB, (byte)0xCC};
byte[] encrypted = crypto.encrypt(data, key);
byte[] decrypted = crypto.decrypt(encrypted, key);
```

## Security Features

### Anti-Debugging
The `checkLicense()` function checks for debuggers before validation:
- Blocks Frida/IDA/gdb via ptrace syscalls
- Triggers before license logic runs
- Cannot be bypassed with simple hooks

### Anti-Hooking
Detects inline hooks (AArch64):
- Checks function prologues for modified instructions
- Validates GOT/PLT entries (anti-rebind/fishhook)
- Runtime check, not compile-time

### Constant Encryption
Magic numbers encrypted at compile time:
```cpp
const uint32_t magic = 0xDEADBEEF;  // Encrypted in binary
// Runtime: decrypted inline before use
```

### Virtualization
The `validateLicense()` function is translated to bytecode:
- Custom VM interpreter embedded in binary
- Original assembly completely removed
- Extreme anti-disassembly protection
- **Overhead:** 50-100% slower than native

## Performance Impact

| Function | Obfuscation Level | Runtime Overhead |
|----------|------------------|------------------|
| `validateLicense()` | Maximum (VMP) | 50-100% |
| `xorEncrypt()` | Heavy | 25-40% |
| `xorDecrypt()` | Light | 10-15% |
| `hash()` | Medium | 20-30% |

## Verify Obfuscation

### Check VM Protection
```bash
llvm-objdump -d libcrypto-lib.so | grep -A 50 "validateLicense"

# You should see:
# - No recognizable function prologue
# - VM dispatcher loop
# - Bytecode fetch/decode/execute pattern
```

### Check Constant Encryption
```bash
# Magic constants should NOT appear as literals
strings libcrypto-lib.so | grep -E "DEADBEEF|13371337"
# (should return nothing)
```

### Check Anti-Debug
```bash
# Run under debugger - should fail
adb shell gdb --args /data/local/tmp/test
# App exits immediately with PTRACE_TRACEME check

# Run normally - should work
adb shell /data/local/tmp/test
# License check succeeds
```

## Recommended Use Cases

✅ **Good for:**
- License validation
- DRM checks
- API key validation
- Critical crypto operations
- Anti-cheat systems

❌ **Avoid for:**
- Hot loops (performance-critical code)
- Large codebases (build time explosion)
- Frequently called functions

## Tips

1. **Use annotations selectively**: Only apply `vmp` to 1-2 critical functions
2. **Balance security vs speed**: Use lighter obfuscation for decrypt functions
3. **Test on device**: VMP overhead varies by CPU (50-100%)
4. **Combine with expiry**: Add `-mllvm -expiry 90` for beta builds
