#include <jni.h>
#include <android/log.h>

#define TAG "ExpiryDemo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// This library will auto-expire based on compile-time flag
// Example: -mllvm -expiry 7 = expires 7 days after build

// Simple function - will be obfuscated globally
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_beta_MainActivity_getBuildInfo(JNIEnv* env, jobject) {
    // Note: The time expiry check happens in a global constructor
    // BEFORE this function is ever called. If expired, app crashes
    // at library load time.

    LOGI("Library loaded successfully - not expired");
    return env->NewStringUTF("Beta Build - Time-Limited");
}

// Check if we can execute (this will only run if not expired)
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_beta_MainActivity_isActive(JNIEnv*, jobject) {
    // If we reach here, library is still valid
    return true;
}

// Anti-debugging wrapper for sensitive operations
__attribute__((annotate("antidbg")))
extern "C" JNIEXPORT void JNICALL
Java_com_example_beta_MainActivity_performSensitiveOperation(JNIEnv*, jobject) {
    // Blocks debuggers, then checks expiry
    LOGI("Performing sensitive operation");
}

// Get build timestamp (for debugging only - remove in production)
extern "C" JNIEXPORT jlong JNICALL
Java_com_example_beta_MainActivity_getBuildTimestamp(JNIEnv*, jobject) {
    // This would show the compile-time timestamp
    // In real builds, this should be removed or obfuscated
    LOGI("Note: Build timestamp is embedded, not exposed via API");
    return 0; // Placeholder - actual timestamp is in __build_timestamp global
}
