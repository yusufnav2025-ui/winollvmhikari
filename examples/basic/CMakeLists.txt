cmake_minimum_required(VERSION 3.18)
project(BasicObfuscation)

# Create shared library
add_library(native-lib SHARED native.cpp)

# Basic obfuscation flags
# -fla: Control flow flattening - converts if/else to switch-case
# -bcf: Bogus control flow - adds fake branches
# -sub: Instruction substitution - replaces operations with equivalents
target_compile_options(native-lib PRIVATE
    -mllvm -fla
    -mllvm -bcf
    -mllvm -sub
)

# Link Android log library
target_link_libraries(native-lib log)
