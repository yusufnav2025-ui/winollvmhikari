cmake_minimum_required(VERSION 3.18)
project(AdvancedObfuscation)

# Create shared library
add_library(advanced-lib SHARED advanced.cpp)

# Enable per-function annotation control
target_compile_options(advanced-lib PRIVATE -mllvm -fncmd)

# Global baseline obfuscation (applied to all non-annotated functions)
target_compile_options(advanced-lib PRIVATE
    -mllvm -fla          # Control flow flattening
    -mllvm -bcf          # Bogus control flow
    -mllvm -sub          # Instruction substitution
)

# Enable all advanced passes (controlled per-function via annotations)
target_compile_options(advanced-lib PRIVATE
    -mllvm -bcf2         # Enhanced bogus control flow
    -mllvm -split        # Basic block splitting
    -mllvm -mba          # Mixed Boolean Arithmetic
    -mllvm -constenc     # Constant encryption
    -mllvm -vmp          # Virtualization
    -mllvm -antidbg      # Anti-debugging
    -mllvm -antihook     # Anti-hooking
    -mllvm -icall        # Indirect calls
    -mllvm -ibr          # Indirect branches
    -mllvm -igv          # Indirect global variables
    -mllvm -fco          # Function call obfuscation
    -mllvm -funcwrap     # Function wrapper
    -mllvm -mergefunc    # Merge functions
)

# Link libraries
target_link_libraries(advanced-lib log dl)

# Build notes:
# - Per-function annotations give fine-grained control
# - Global flags enable passes, annotations apply them
# - VMP functions take 5-10x longer to compile
# - Total build time: 2-3x baseline for this example
