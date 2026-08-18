#include <jni.h>
#include <string>
#include <android/log.h>

#define TAG "NativeLib"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// Basic obfuscated function - applies global flags from build system
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_basic_MainActivity_stringFromJNI(JNIEnv* env, jobject) {
    std::string hello = "Hello from OLLVM17!";
    return env->NewStringUTF(hello.c_str());
}

// Simple calculation - will be obfuscated by -fla -bcf -sub flags
extern "C" JNIEXPORT jint JNICALL
Java_com_example_basic_MainActivity_calculate(JNIEnv*, jobject, jint a, jint b) {
    int result = (a + b) * 2 - 10;
    return result;
}

// Version check - basic protection
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_basic_MainActivity_checkVersion(JNIEnv* env, jobject, jstring version) {
    const char* versionStr = env->GetStringUTFChars(version, nullptr);
    bool valid = (std::string(versionStr) == "1.0.0");
    env->ReleaseStringUTFChars(version, versionStr);
    return valid;
}
