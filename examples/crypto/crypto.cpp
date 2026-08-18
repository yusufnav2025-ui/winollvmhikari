#include <jni.h>
#include <string>
#include <cstdint>
#include <android/log.h>

#define TAG "CryptoLib"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// License validation with maximum obfuscation
// This function is VM-protected + heavily obfuscated
__attribute__((annotate("fla bcf2 sub vmp mba constenc")))
bool validateLicense(const std::string& key) {
    // Magic constants will be encrypted at compile time (-constenc)
    const uint32_t magic1 = 0xDEADBEEF;
    const uint32_t magic2 = 0x13371337;
    const uint32_t expected = 0x12345678;

    uint32_t hash = magic2;
    for (char c : key) {
        hash = ((hash << 5) + hash) ^ c;
        hash += magic1;
    }

    return (hash ^ magic1) == expected;
}

// XOR encryption with math obfuscation
__attribute__((annotate("fla bcf sub mba constenc")))
void xorEncrypt(uint8_t* data, size_t len, const uint8_t* key, size_t keyLen) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= key[i % keyLen];
        data[i] = (data[i] << 1) | (data[i] >> 7); // Rotate left
    }
}

// Decrypt with lighter obfuscation (performance-critical)
__attribute__((annotate("fla bcf")))
void xorDecrypt(uint8_t* data, size_t len, const uint8_t* key, size_t keyLen) {
    for (size_t i = 0; i < len; i++) {
        data[i] = (data[i] >> 1) | (data[i] << 7); // Rotate right
        data[i] ^= key[i % keyLen];
    }
}

// Anti-debugging + anti-hooking wrapper
__attribute__((annotate("antidbg antihook")))
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_crypto_CryptoLib_checkLicense(JNIEnv* env, jobject, jstring key) {
    // Blocks Frida/IDA before validation
    const char* keyStr = env->GetStringUTFChars(key, nullptr);
    bool valid = validateLicense(keyStr);
    env->ReleaseStringUTFChars(key, keyStr);

    if (valid) {
        LOGI("License valid");
    } else {
        LOGI("License invalid");
    }

    return valid;
}

// Encrypt data (exposed to Java)
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_example_crypto_CryptoLib_encrypt(JNIEnv* env, jobject,
                                           jbyteArray data, jbyteArray key) {
    jsize dataLen = env->GetArrayLength(data);
    jsize keyLen = env->GetArrayLength(key);

    jbyte* dataBytes = env->GetByteArrayElements(data, nullptr);
    jbyte* keyBytes = env->GetByteArrayElements(key, nullptr);

    // Encrypt in-place
    xorEncrypt(reinterpret_cast<uint8_t*>(dataBytes), dataLen,
               reinterpret_cast<uint8_t*>(keyBytes), keyLen);

    // Create result
    jbyteArray result = env->NewByteArray(dataLen);
    env->SetByteArrayRegion(result, 0, dataLen, dataBytes);

    env->ReleaseByteArrayElements(data, dataBytes, 0);
    env->ReleaseByteArrayElements(key, keyBytes, 0);

    return result;
}

// Decrypt data (exposed to Java)
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_example_crypto_CryptoLib_decrypt(JNIEnv* env, jobject,
                                           jbyteArray data, jbyteArray key) {
    jsize dataLen = env->GetArrayLength(data);
    jsize keyLen = env->GetArrayLength(key);

    jbyte* dataBytes = env->GetByteArrayElements(data, nullptr);
    jbyte* keyBytes = env->GetByteArrayElements(key, nullptr);

    // Decrypt in-place
    xorDecrypt(reinterpret_cast<uint8_t*>(dataBytes), dataLen,
               reinterpret_cast<uint8_t*>(keyBytes), keyLen);

    // Create result
    jbyteArray result = env->NewByteArray(dataLen);
    env->SetByteArrayRegion(result, 0, dataLen, dataBytes);

    env->ReleaseByteArrayElements(data, dataBytes, 0);
    env->ReleaseByteArrayElements(key, keyBytes, 0);

    return result;
}

// Simple hash function with medium obfuscation
__attribute__((annotate("fla bcf sub mba")))
extern "C" JNIEXPORT jint JNICALL
Java_com_example_crypto_CryptoLib_hash(JNIEnv* env, jobject, jstring input) {
    const char* str = env->GetStringUTFChars(input, nullptr);

    uint32_t hash = 5381;
    while (*str) {
        hash = ((hash << 5) + hash) ^ (*str++);
    }

    env->ReleaseStringUTFChars(input, str);
    return static_cast<jint>(hash);
}
