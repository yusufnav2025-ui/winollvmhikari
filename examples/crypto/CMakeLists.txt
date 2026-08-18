cmake_minimum_required(VERSION 3.18)
project(CryptoObfuscation)

# Create shared library
add_library(crypto-lib SHARED crypto.cpp)

# Heavy obfuscation for crypto functions
# This demonstrates maximum protection for sensitive code
target_compile_options(crypto-lib PRIVATE
    -mllvm -fla          # Control flow flattening
    -mllvm -bcf2         # Enhanced bogus control flow
    -mllvm -sub          # Instruction substitution
    -mllvm -sub_loop=3   # Apply substitution 3 times
    -mllvm -mba          # Mixed Boolean Arithmetic
    -mllvm -constenc     # Constant encryption (encrypts magic numbers)
    -mllvm -antidbg      # Anti-debugging (blocks Frida/IDA)
    -mllvm -antihook     # Anti-hooking (detects inline hooks)
    -mllvm -vmp          # Virtualization (VM-protect annotated functions)
    -mllvm -fncmd        # Enable per-function annotation control
)

# Link Android log library
target_link_libraries(crypto-lib log)

# Note: Build time will be 3-5x longer due to VMP
# Runtime overhead: 50-100% for VM-protected functions
# Security level: Maximum
