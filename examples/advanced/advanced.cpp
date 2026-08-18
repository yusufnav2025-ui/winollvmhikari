#include <jni.h>
#include <string>

// Full obfuscation suite example
// This demonstrates MAXIMUM protection available in the toolchain

// ============================================================================
// LEVEL 1: BASIC OBFUSCATION (Fast, Light)
// ============================================================================

// Control flow flattening only
__attribute__((annotate("fla")))
int level1_add(int a, int b) {
    return a + b;
}

// Bogus control flow only
__attribute__((annotate("bcf")))
int level1_multiply(int a, int b) {
    return a * b;
}

// ============================================================================
// LEVEL 2: MEDIUM OBFUSCATION (Balanced)
// ============================================================================

// Flattening + bogus control flow + substitution
__attribute__((annotate("fla bcf sub")))
int level2_hash(const char* str) {
    int hash = 5381;
    while (*str) {
        hash = ((hash << 5) + hash) + (*str++);
    }
    return hash;
}

// Add math obfuscation
__attribute__((annotate("fla bcf sub mba")))
bool level2_validate(int value) {
    const int magic = 0x13371337;
    return (value ^ magic) == 0x12345678;
}

// ============================================================================
// LEVEL 3: HEAVY OBFUSCATION (Strong Protection)
// ============================================================================

// Enhanced BCF + multiple substitution rounds
__attribute__((annotate("fla bcf2 sub split")))
__attribute__((annotate("split_num=5")))  // Split into 5 basic blocks
void level3_encrypt(char* data, int len, char key) {
    for (int i = 0; i < len; i++) {
        data[i] ^= key;
        data[i] = (data[i] << 3) | (data[i] >> 5);
    }
}

// Add constant encryption
__attribute__((annotate("fla bcf2 sub mba constenc")))
__attribute__((annotate("sub_loop=3")))  // Apply substitution 3 times
int level3_checksum(const unsigned char* data, int len) {
    unsigned int crc = 0xFFFFFFFF;
    const unsigned int poly = 0xEDB88320;  // Encrypted at compile time

    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? poly : 0);
        }
    }
    return ~crc;
}

// ============================================================================
// LEVEL 4: MAXIMUM OBFUSCATION (VM Protection)
// ============================================================================

// Full obfuscation suite + VM protection
__attribute__((annotate("fla bcf2 sub split mba constenc vmp")))
__attribute__((annotate("sub_loop=3 split_num=5")))
bool level4_validateLicense(const char* key) {
    // This entire function is converted to VM bytecode
    const unsigned int magic1 = 0xDEADBEEF;
    const unsigned int magic2 = 0xCAFEBABE;

    unsigned int hash = magic2;
    while (*key) {
        hash = ((hash << 5) + hash) ^ (*key++);
        hash += magic1;
    }

    return (hash ^ magic1) == 0x12345678;
}

// VM + anti-debugging + anti-hooking
__attribute__((annotate("vmp antidbg antihook")))
void level4_criticalOperation() {
    // Blocks debuggers, detects hooks, runs in VM
    // This is the highest protection level available
}

// ============================================================================
// LEVEL 5: CALL OBFUSCATION (Hide Call Targets)
// ============================================================================

// Indirect calls via function table
__attribute__((annotate("icall")))
int level5_callFunction(int (*fn)(int, int), int a, int b) {
    return fn(a, b);  // Call target hidden
}

// Indirect branches via computed goto
__attribute__((annotate("ibr")))
int level5_switch(int choice) {
    switch (choice) {
        case 0: return 100;
        case 1: return 200;
        case 2: return 300;
        default: return 0;
    }
}

// Indirect global variable access
__attribute__((annotate("igv")))
int globalVar = 42;

int level5_getGlobal() {
    return globalVar;  // Access obfuscated
}

// ============================================================================
// LEVEL 6: FUNCTION-LEVEL COMPOSITION
// ============================================================================

// Function call obfuscation (dlopen/dlsym wrapper)
__attribute__((annotate("fco")))
extern int external_function(int x);

int level6_callExternal(int value) {
    // external_function is called via dlopen/dlsym
    // Call target completely hidden from static analysis
    return external_function(value);
}

// Function wrapper (hide call graph)
__attribute__((annotate("funcwrap")))
int level6_wrapped(int a, int b) {
    return a * b + 100;
}

// Function merging (merge similar functions)
__attribute__((annotate("mergefunc")))
int level6_merge1(int x) { return x * 2; }

__attribute__((annotate("mergefunc")))
int level6_merge2(int x) { return x * 3; }

// ============================================================================
// JNI EXPORTS
// ============================================================================

extern "C" JNIEXPORT jint JNICALL
Java_com_example_advanced_AdvancedLib_testLevel1(JNIEnv*, jobject, jint a, jint b) {
    return level1_add(a, b);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_advanced_AdvancedLib_testLevel2(JNIEnv* env, jobject, jstring str) {
    const char* cstr = env->GetStringUTFChars(str, nullptr);
    int result = level2_hash(cstr);
    env->ReleaseStringUTFChars(str, cstr);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_advanced_AdvancedLib_testLevel3(JNIEnv* env, jobject, jbyteArray data) {
    jsize len = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    int result = level3_checksum(reinterpret_cast<unsigned char*>(bytes), len);
    env->ReleaseByteArrayElements(data, bytes, 0);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_advanced_AdvancedLib_testLevel4(JNIEnv* env, jobject, jstring key) {
    const char* ckey = env->GetStringUTFChars(key, nullptr);
    bool result = level4_validateLicense(ckey);
    env->ReleaseStringUTFChars(key, ckey);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_advanced_AdvancedLib_testLevel4Critical(JNIEnv*, jobject) {
    level4_criticalOperation();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_advanced_AdvancedLib_testLevel5(JNIEnv*, jobject, jint choice) {
    return level5_switch(choice);
}
