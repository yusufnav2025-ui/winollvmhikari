cmake_minimum_required(VERSION 3.18)
project(ExpiryDemo)

# Create shared library
add_library(beta-lib SHARED beta.cpp)

# Time expiry configuration
# This library will auto-expire 7 days after compilation
target_compile_options(beta-lib PRIVATE
    -mllvm -expiry 7           # Expire after 7 days
    -mllvm -expiry-print=0     # Silent crash (no message)
)

# Basic obfuscation
target_compile_options(beta-lib PRIVATE
    -mllvm -fla                # Control flow flattening
    -mllvm -bcf                # Bogus control flow
    -mllvm -sub                # Instruction substitution
    -mllvm -antidbg            # Anti-debugging
)

# Link Android log library
target_link_libraries(beta-lib log)

# Build notes:
# - Compile time is captured at IR generation (when you run cmake/make)
# - Binary expires exactly 7*24*60 minutes after that timestamp
# - Time check uses syscall (clock_gettime), not libc time()
# - Expiry check runs in global constructor (before main/JNI_OnLoad)
