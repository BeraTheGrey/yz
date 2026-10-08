// JNI entry points for io.github.berathegrey.yzdac.usb.NativeBridge.
#include <jni.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../engine/dac_device.h"
#include "../engine/json.h"

using engine::DacDevice;
using engine::ReattachMode;

namespace {

DacDevice* dev(jlong h) { return reinterpret_cast<DacDevice*>(h); }

jstring str(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

jstring error(JNIEnv* env, const char* message) {
    engine::Json j;
    j.beginObject().field("ok", false).field("error", message).endObject();
    return str(env, j.str());
}

ReattachMode mode(jint m) { return m == 1 ? ReattachMode::UsbReset : ReattachMode::AttachDriver; }

std::string lastOpenError;
std::mutex openErrorMutex;

}  // namespace

#define JNI_FN(name) Java_io_github_berathegrey_yzdac_usb_NativeBridge_##name

extern "C" {

JNIEXPORT jlong JNICALL JNI_FN(nativeOpen)(JNIEnv* env, jclass, jint fd, jbyteArray raw) {
    if (!raw) return 0;
    const jsize len = env->GetArrayLength(raw);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    env->GetByteArrayRegion(raw, 0, len, reinterpret_cast<jbyte*>(buf.data()));
    std::string err;
    auto d = DacDevice::open(fd, buf.data(), buf.size(), err);
    std::lock_guard<std::mutex> lock(openErrorMutex);
    lastOpenError = err;
    return reinterpret_cast<jlong>(d.release());
}

JNIEXPORT jstring JNICALL JNI_FN(nativeLastOpenError)(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(openErrorMutex);
    return str(env, lastOpenError);
}

JNIEXPORT void JNICALL JNI_FN(nativeClose)(JNIEnv*, jclass, jlong h) { delete dev(h); }

JNIEXPORT jstring JNICALL JNI_FN(nativeDescribe)(JNIEnv* env, jclass, jlong h) {
    if (!h) return error(env, "cihaz açık değil");
    return str(env, dev(h)->describeJson());
}

JNIEXPORT jstring JNICALL JNI_FN(nativeReadVolume)(JNIEnv* env, jclass, jlong h, jint reattach) {
    if (!h) return error(env, "cihaz açık değil");
    return str(env, dev(h)->volumeJson(mode(reattach)));
}

JNIEXPORT jstring JNICALL JNI_FN(nativeApplyVolume)(JNIEnv* env, jclass, jlong h, jboolean toMax, jint target,
                                                    jboolean allowBeyondMax, jint reattach) {
    if (!h) return error(env, "cihaz açık değil");
    const int clamped = target < -32768 ? -32768 : (target > 32767 ? 32767 : target);
    return str(env, dev(h)->applyVolumeJson(toMax, static_cast<int16_t>(clamped), allowBeyondMax, mode(reattach)));
}

JNIEXPORT jstring JNICALL JNI_FN(nativeProbeAboveMax)(JNIEnv* env, jclass, jlong h, jint limitDb,
                                                      jboolean keepHighest, jint reattach) {
    if (!h) return error(env, "cihaz açık değil");
    return str(env, dev(h)->probeAboveMaxJson(limitDb, keepHighest, mode(reattach)));
}

JNIEXPORT jstring JNICALL JNI_FN(nativeStartStream)(JNIEnv* env, jclass, jlong h, jint rate, jint channels,
                                                    jint bits, jint forcedRate) {
    if (!h) return error(env, "cihaz açık değil");
    if (rate <= 0 || channels <= 0) return error(env, "geçersiz kaynak formatı");
    return str(env, dev(h)->startStream(static_cast<uint32_t>(rate), channels, bits,
                                        forcedRate > 0 ? static_cast<uint32_t>(forcedRate) : 0));
}

JNIEXPORT jint JNICALL JNI_FN(nativeWrite)(JNIEnv* env, jclass, jlong h, jobject buffer, jint offset, jint size,
                                           jint encoding, jint channels, jint rate) {
    if (!h || !buffer || size <= 0 || offset < 0) return -1;
    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
    const jlong cap = env->GetDirectBufferCapacity(buffer);
    if (!base || cap < static_cast<jlong>(offset) + size) return -1;
    return dev(h)->write(base + offset, static_cast<size_t>(size), static_cast<dsp::InputEncoding>(encoding), channels,
                         static_cast<uint32_t>(rate));
}

JNIEXPORT void JNICALL JNI_FN(nativeFlush)(JNIEnv*, jclass, jlong h) {
    if (h) dev(h)->flush();
}

JNIEXPORT void JNICALL JNI_FN(nativeStopStream)(JNIEnv*, jclass, jlong h) {
    if (h) dev(h)->stopStream();
}

JNIEXPORT jstring JNICALL JNI_FN(nativeStats)(JNIEnv* env, jclass, jlong h) {
    if (!h) return error(env, "cihaz açık değil");
    return str(env, dev(h)->statsJson());
}

JNIEXPORT void JNICALL JNI_FN(nativeSetDigitalGain)(JNIEnv*, jclass, jlong h, jfloat gain, jboolean softClip) {
    if (h) dev(h)->setDigitalGain(gain, softClip);
}

}  // extern "C"
