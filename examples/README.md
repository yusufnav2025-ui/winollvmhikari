# Examples

Practical obfuscation examples for Android JNI libraries.

## Quick Start

Each example is a complete, buildable Android JNI library demonstrating different obfuscation techniques.

### Build All Examples

```bash
cd examples
for dir in basic crypto expiry advanced; do
    cd $dir
    ndk-build
    cd ..
done
```

Or with CMake:

```bash
for dir in basic crypto expiry advanced; do
    cd $dir
    mkdir -p build && cd build
    cmake .. -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
             -DANDROID_ABI=arm64-v8a \
             -DANDROID_PLATFORM=android-21
    make
    cd ../..
done
```

## Examples Overview

| Example | Focus | Complexity | Build Time | Runtime Overhead |
|---------|-------|------------|------------|------------------|
| [basic](basic/) | Simple obfuscation | Low | 1.2x | 10-15% |
| [crypto](crypto/) | License + encryption | Medium | 3-5x | 25-100% |
| [expiry](expiry/) | Time-limited builds | Low | 1.1x | <1% |
| [advanced](advanced/) | All techniques | High | 2-3x | 5-100% |

## Example Details

### [Basic](basic/) - Getting Started
**What it shows:**
- Control flow flattening (`-fla`)
- Bogus control flow (`-bcf`)
- Instruction substitution (`-sub`)

**Best for:**
- First-time users
- Understanding pass effects
- Baseline protection

**Build time:** ~5 minutes on typical CI  
**Use case:** General-purpose apps

---

### [Crypto](crypto/) - Sensitive Operations
**What it shows:**
- Per-function annotations
- VM protection (`vmp`)
- Constant encryption (`constenc`)
- Anti-debugging (`antidbg`)
- Anti-hooking (`antihook`)

**Best for:**
- License validation
- API key protection
- Encryption/decryption
- DRM systems

**Build time:** ~15 minutes (due to VMP)  
**Use case:** Apps with license checks or crypto

---

### [Expiry](expiry/) - Time-Limited Builds
**What it shows:**
- Time-based expiry (`-expiry N`)
- Compile-time timestamp injection
- Syscall-based time checks
- Beta/trial builds

**Best for:**
- Beta testing
- Trial licenses
- Internal dogfood builds
- Time-limited demos

**Build time:** ~4 minutes  
**Use case:** Beta apps, trials, internal tools

---

### [Advanced](advanced/) - Complete Showcase
**What it shows:**
- All 6 protection levels
- Per-function fine-tuning
- Performance comparisons
- Recommended combinations

**Best for:**
- Learning all techniques
- Comparing approaches
- Reference implementation
- Advanced users

**Build time:** ~10 minutes  
**Use case:** Security-critical apps

## Usage Patterns

### Pattern 1: Basic Protection (All Apps)
```bash
# Global flags only
-mllvm -fla -mllvm -bcf -mllvm -sub

# Overhead: 10-15%
# Build time: 1.2x
```

**Apply to:** All production apps as baseline protection

---

### Pattern 2: Sensitive Functions (Crypto/License)
```cpp
// Annotate critical functions
__attribute__((annotate("fla bcf2 sub mba constenc")))
bool validateLicense(const char* key);

__attribute__((annotate("vmp antidbg")))
void checkIntegrity();
```

```bash
# Enable passes + per-function control
-mllvm -fncmd -mllvm -vmp -mllvm -antidbg -mllvm -constenc
```

**Apply to:** License validators, crypto operations, anti-cheat

---

### Pattern 3: Time-Limited (Beta/Trial)
```bash
# 14-day beta build
-mllvm -expiry 14 -mllvm -expiry-print=1

# 90-day trial (silent)
-mllvm -expiry 90 -mllvm -expiry-print=0
```

**Apply to:** Beta releases, trial licenses, internal tools

---

### Pattern 4: Maximum Protection (High-Security)
```cpp
__attribute__((annotate("vmp antidbg antihook constenc fla bcf2 sub mba")))
void criticalOperation();
```

```bash
# All passes enabled
-mllvm -fncmd -mllvm -vmp -mllvm -antidbg -mllvm -antihook \
-mllvm -constenc -mllvm -fla -mllvm -bcf2 -mllvm -sub -mllvm -mba
```

**Apply to:** Banking apps, payment processors, DRM systems

---

## Performance Comparison

Measured on Snapdragon 888 (AArch64), median of 1000 runs:

| Obfuscation Level | Example Function | Native | Obfuscated | Overhead |
|-------------------|------------------|---------|------------|----------|
| **None** | `add(a, b)` | 4ns | 4ns | 0% |
| **Light** (`fla`) | `add_obf(a, b)` | 4ns | 4.5ns | +12% |
| **Medium** (`fla bcf sub`) | `hash(str)` | 120ns | 150ns | +25% |
| **Heavy** (`bcf2 mba constenc`) | `encrypt(data)` | 2.5μs | 3.8μs | +52% |
| **Maximum** (`vmp`) | `validate(key)` | 800ns | 1.6μs | +100% |

**Key insights:**
- Light obfuscation: negligible overhead (<15%)
- Heavy obfuscation: acceptable for non-hot code (50-70%)
- VMP: only for 1-2 critical functions (100%+ overhead)

---

## Build Time Comparison

Clean build on GitHub Actions (windows-latest, 16GB RAM):

| Example | Baseline | With Obfuscation | Multiplier |
|---------|----------|------------------|------------|
| basic | 4min | 5min | 1.25x |
| crypto (no VMP) | 4min | 8min | 2x |
| crypto (with VMP) | 4min | 18min | 4.5x |
| expiry | 4min | 4.5min | 1.12x |
| advanced | 4min | 10min | 2.5x |

**VMP impact:** Each VM-protected function adds 3-5 minutes to build time.

---

## Security vs Performance Trade-offs

### Scenario 1: Mobile Game
**Requirements:** High FPS, anti-cheat
**Recommendation:**
- Light obfuscation globally (`-fla -bcf`)
- Anti-debug for cheat detection (`annotate("antidbg")`)
- **Avoid:** VMP on rendering/physics loops

**Result:** <15% overhead, playable FPS

---

### Scenario 2: Banking App
**Requirements:** Maximum security, license checks
**Recommendation:**
- Heavy obfuscation globally (`-fla -bcf2 -sub -mba`)
- VMP for transaction validation (`annotate("vmp antidbg antihook")`)
- Constant encryption for API keys (`-constenc`)

**Result:** 50-100% overhead on critical paths, acceptable for infrequent operations

---

### Scenario 3: SaaS App
**Requirements:** Trial license, moderate protection
**Recommendation:**
- Medium obfuscation (`-fla -bcf -sub`)
- Time expiry for trials (`-expiry 30`)
- Anti-debug for license checks (`annotate("antidbg")`)

**Result:** 20-30% overhead, 30-day auto-expiry

---

### Scenario 4: Open-Source Tool
**Requirements:** Minimal overhead, anti-tampering
**Recommendation:**
- Light obfuscation (`-fla`)
- No VMP (compile time too high)
- Optional: `-antidbg` for release builds only

**Result:** <10% overhead, short build times

---

## Recommendations by App Type

### General-Purpose Apps
- **Use:** [basic](basic/) example
- **Flags:** `-fla -bcf -sub`
- **Why:** Balanced protection, low overhead

### Security-Critical Apps
- **Use:** [crypto](crypto/) + [advanced](advanced/)
- **Flags:** Full suite with VMP for 1-2 functions
- **Why:** Maximum protection where it matters

### Beta/Trial Apps
- **Use:** [expiry](expiry/)
- **Flags:** `-expiry N -expiry-print=0/1`
- **Why:** Auto-expire old builds

### Learning/Experimentation
- **Use:** [advanced](advanced/)
- **Flags:** All passes with per-function control
- **Why:** See all techniques in action

---

## Common Questions

### Q: Which example should I start with?
**A:** Start with [basic](basic/). It shows the core passes with minimal complexity.

### Q: My build is too slow with VMP. What do I do?
**A:** Only annotate 1-2 critical functions with `vmp`. See [crypto](crypto/) for selective usage.

### Q: Can I combine flags from different examples?
**A:** Yes! Mix and match. See [advanced](advanced/) for all combinations.

### Q: How do I verify obfuscation worked?
**A:** Each example's README has a "Verification" section with disassembly commands.

### Q: What's the smallest obfuscation I can use?
**A:** Just `-mllvm -fla` adds ~10% overhead. See [basic](basic/).

### Q: How do I obfuscate only one function?
**A:** Use annotations. See [crypto](crypto/) and [advanced](advanced/).

---

## Next Steps

1. **Try basic example:**
   ```bash
   cd examples/basic
   ndk-build
   llvm-objdump -d libs/arm64-v8a/libnative-lib.so
   ```

2. **Read the READMEs:**
   - [basic/README.md](basic/README.md) - Start here
   - [crypto/README.md](crypto/README.md) - Advanced techniques
   - [expiry/README.md](expiry/README.md) - Time-limited builds
   - [advanced/README.md](advanced/README.md) - Complete reference

3. **Experiment with flags:**
   - Modify `Android.mk` or `CMakeLists.txt`
   - Rebuild and compare binary size/performance
   - Disassemble to verify obfuscation

4. **Profile on device:**
   ```bash
   adb push libs/arm64-v8a/*.so /data/local/tmp/
   adb shell "time /data/local/tmp/test"
   ```

5. **Read the main documentation:**
   - [../../README.md](../../README.md) - Complete flag reference
   - [../../docs/PASSES.md](../../docs/PASSES.md) - Pass details

---

## Support

- **Issues:** [GitHub Issues](https://github.com/DreamSoule/ollvm17/issues)
- **Discussions:** [GitHub Discussions](https://github.com/DreamSoule/ollvm17/discussions)
- **Upstream:** [Hikari LLVM](https://github.com/HikariObfuscator/Hikari)

---

## Credits

Examples based on:
- DreamSoule/ollvm17 (LLVM 17 port)
- Hikari Obfuscator (original passes)
- Polaris additions (LinearMBA, AliasAccess, JunkCode, BCF2, MergeFunction, TimeExpiry)
