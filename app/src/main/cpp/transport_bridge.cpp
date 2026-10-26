#include <jni.h>
#include "native_transport.h"
#include "video_payload.h"

#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace camdroid::android {

    namespace utils {

        static std::mutex registry_mutex;
        static std::unordered_map<jlong, std::shared_ptr<native_transport>> transports;
        static jlong next_handle = 1;

        static std::shared_ptr<native_transport> lookup(jlong handle) {

            std::lock_guard lock(registry_mutex);
            auto entry = transports.find(handle);

            if (entry == transports.end())
                throw std::runtime_error("Native connection is closed or invalid.");

            return entry->second;

        }

        static void report(JNIEnv* env, const char* type, const char* message) {

            if (env->ExceptionCheck())
                return;

            auto exception = env->FindClass(type);

            if (exception != nullptr)
                env->ThrowNew(exception, message);

        }

        template <typename Result, typename Function> static Result guard(JNIEnv* env, Result fallback, Function function) {

            try {

                return function();

            } catch (const std::invalid_argument& error) {

                report(env, "java/lang/IllegalArgumentException", error.what());

            } catch (const std::exception& error) {

                report(env, "java/lang/IllegalStateException", error.what());

            } catch (...) {

                report(env, "java/lang/IllegalStateException", "Unexpected native transport failure.");

            }

            return fallback;

        }

        static std::vector<std::uint8_t> copy_array(JNIEnv* env, jbyteArray data) {

            if (data == nullptr)
                throw std::invalid_argument("Missing byte array.");

            const jsize size = env->GetArrayLength(data);

            if (size > 80 * 1024 * 1024)
                throw std::length_error("Android packet limit exceeded.");

            std::vector<std::uint8_t> result(static_cast<std::size_t>(size));

            if (size > 0)
                env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(result.data()));

            return result;

        }

    };

};

extern "C" JNIEXPORT jlong JNICALL Java_csral_camdroid_TransportBridge_create(JNIEnv* env, jobject) {

    return camdroid::android::utils::guard<jlong>(env, 0, [] {
        auto transport = std::make_shared<camdroid::android::native_transport>();
        std::lock_guard lock(camdroid::android::utils::registry_mutex);

        if (camdroid::android::utils::next_handle == std::numeric_limits<jlong>::max())
            throw std::runtime_error("Native handle space exhausted.");

        const jlong handle = camdroid::android::utils::next_handle++;
        camdroid::android::utils::transports.emplace(handle, std::move(transport));

        return handle;
    });

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_TransportBridge_close(JNIEnv* env, jobject, jlong handle) {

    camdroid::android::utils::guard<bool>(env, false, [&] {

        std::shared_ptr<camdroid::android::native_transport> transport;

        {

            std::lock_guard lock(camdroid::android::utils::registry_mutex);
            auto entry = camdroid::android::utils::transports.find(handle);

            if (entry == camdroid::android::utils::transports.end())
                return true;

            transport = std::move(entry->second);
            camdroid::android::utils::transports.erase(entry);

        }

        transport->close();

        return true;

    });

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_TransportBridge_interrupt(JNIEnv* env, jobject, jlong handle) {

    camdroid::android::utils::guard<bool>(env, false, [&] {
        camdroid::android::utils::lookup(handle)->interrupt();

        return true;
    });

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_TransportBridge_connect(
    JNIEnv* env,
    jobject,
    jlong handle,
    jstring host,

    jint port,
    jboolean optimized,
    jlong network
) {

    camdroid::android::utils::guard<bool>(env, false, [&] {

        auto transport = camdroid::android::utils::lookup(handle);

        if (host == nullptr)
            throw std::invalid_argument("Missing desktop address.");

        const char* chars = env->GetStringUTFChars(host, nullptr);

        if (chars == nullptr)
            return false;

        std::string address;

        try {

            address = chars;

        } catch (...) {

            env->ReleaseStringUTFChars(host, chars);
            throw;

        }

        env->ReleaseStringUTFChars(host, chars);
        transport->connect(address, port, optimized == JNI_TRUE, static_cast<std::uint64_t>(network));

        return true;

    });

}

extern "C" JNIEXPORT jboolean JNICALL Java_csral_camdroid_TransportBridge_isConnected(JNIEnv* env, jobject, jlong handle) {

    return camdroid::android::utils::guard<jboolean>(env, JNI_FALSE, [&] {
        return static_cast<jboolean>(camdroid::android::utils::lookup(handle)->is_connected());
    });

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_TransportBridge_videoConfig(
    JNIEnv* env,
    jobject,
    jlong handle,
    jbyteArray csd0,

    jbyteArray csd1,
    jint rotation,
    jboolean mirrored,
    jint fps,
    jint bitrate,

    jboolean user_configured
) {

    camdroid::android::utils::guard<bool>(env, false, [&] {

        auto transport = camdroid::android::utils::lookup(handle);
        camdroid::protocol::video_config config;

        if (rotation < 0 || rotation > 270 || rotation % 90 != 0)
            throw std::invalid_argument("Invalid video rotation.");

        if (fps < 1 || fps > 255 || bitrate < 1)
            throw std::invalid_argument("Invalid video settings.");

        config.settings = {static_cast<std::uint16_t>(fps), static_cast<std::uint32_t>(bitrate), user_configured == JNI_TRUE};
        config.rotation = static_cast<std::uint16_t>(rotation);
        config.mirrored = mirrored == JNI_TRUE;
        config.csd0 = camdroid::android::utils::copy_array(env, csd0);

        if (env->ExceptionCheck())
            return false;

        config.csd1 = camdroid::android::utils::copy_array(env, csd1);

        if (env->ExceptionCheck())
            return false;

        transport->video_config(camdroid::protocol::video_payload::make_config(config));

        return true;

    });

}

extern "C" JNIEXPORT jboolean JNICALL Java_csral_camdroid_TransportBridge_videoFragment(
    JNIEnv* env,
    jobject,
    jlong handle,
    jobject buffer,

    jint offset,
    jint size,
    jlong pts,
    jboolean keyframe,
    jboolean partial
) {

    return camdroid::android::utils::guard<jboolean>(env, JNI_FALSE, [&] {
        auto transport = camdroid::android::utils::lookup(handle);

        if (buffer == nullptr || offset < 0 || size < 0 || pts < 0)
            throw std::invalid_argument("Invalid video buffer, range or timestamp.");

        const jlong capacity = env->GetDirectBufferCapacity(buffer);

        if (capacity < 0 || offset > capacity || size > capacity - offset)
            throw std::invalid_argument("Video buffer must be direct and its range valid.");

        auto* data = static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(buffer));

        if (data == nullptr && size != 0)
            throw std::invalid_argument("Direct video buffer is unavailable.");

        // Copy before returning: MediaCodec may recycle this memory immediately afterward.
        auto view = size == 0 ? std::span<const std::uint8_t>{}
                              : std::span<const std::uint8_t>{data + offset, static_cast<std::size_t>(size)};

        return static_cast<jboolean>(
            transport->video_fragment(view, static_cast<std::uint64_t>(pts), keyframe == JNI_TRUE, partial == JNI_TRUE)
        );
    });

}

extern "C" JNIEXPORT void JNICALL
Java_csral_camdroid_TransportBridge_videoRotation(JNIEnv* env, jobject, jlong handle, jint rotation) {

    camdroid::android::utils::guard<bool>(env, false, [&] {
        camdroid::android::utils::lookup(handle)->video_rotation(rotation);
        return true;
    });

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_TransportBridge_videoSettings(
    JNIEnv* env,
    jobject,
    jlong handle,
    jint fps,

    jint bitrate,
    jboolean user_configured
) {

    camdroid::android::utils::guard<bool>(env, false, [&] {
        camdroid::android::utils::lookup(handle)->video_settings(fps, bitrate, user_configured == JNI_TRUE);

        return true;
    });

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_TransportBridge_videoEos(JNIEnv* env, jobject, jlong handle) {

    camdroid::android::utils::guard<bool>(env, false, [&] {
        camdroid::android::utils::lookup(handle)->video_eos();

        return true;
    });

}

extern "C" JNIEXPORT jboolean JNICALL
Java_csral_camdroid_TransportBridge_needsVideoKeyframe(JNIEnv* env, jobject, jlong handle) {

    return camdroid::android::utils::guard<jboolean>(env, JNI_FALSE, [&] {
        return static_cast<jboolean>(camdroid::android::utils::lookup(handle)->needs_video_keyframe());
    });

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_TransportBridge_sendPacket(
    JNIEnv* env,
    jobject,
    jlong handle,
    jint type,

    jint flags,
    jint payload_flags,
    jbyteArray payload
) {

    camdroid::android::utils::guard<bool>(env, false, [&] {

        auto transport = camdroid::android::utils::lookup(handle);

        if (type < 0 || type > 4 || flags < 0 || flags > 63 || payload_flags < 0 || payload_flags > 15)
            throw std::invalid_argument("Invalid packet type or flags.");

        camdroid::protocol::packet pkt;
        pkt.type = static_cast<camdroid::protocol::PacketType>(type);
        pkt.flags = static_cast<std::uint8_t>(flags);
        pkt.payload_flags = static_cast<std::uint8_t>(payload_flags);
        pkt.payload = camdroid::android::utils::copy_array(env, payload);

        if (env->ExceptionCheck())
            return false;

        transport->send_packet(std::move(pkt));

        return true;

    });

}

extern "C" JNIEXPORT jobject JNICALL Java_csral_camdroid_TransportBridge_waitPacket(JNIEnv* env, jobject, jlong handle) {

    return camdroid::android::utils::guard<jobject>(env, nullptr, [&]() -> jobject {
        auto transport = camdroid::android::utils::lookup(handle);
        auto pkt = transport->wait_packet(500);

        if (!pkt)
            return nullptr;

        jbyteArray payload = env->NewByteArray(static_cast<jsize>(pkt->payload.size()));

        if (payload == nullptr)
            return nullptr;

        if (!pkt->payload.empty())
            env->SetByteArrayRegion(
                payload,
                0,
                static_cast<jsize>(pkt->payload.size()),

                reinterpret_cast<const jbyte*>(pkt->payload.data())
            );

        if (env->ExceptionCheck())
            return nullptr;

        auto cls = env->FindClass("csral/camdroid/IncomingPacket");

        if (cls == nullptr)
            return nullptr;

        auto constructor = env->GetMethodID(cls, "<init>", "(IIIJ[B)V");

        if (constructor == nullptr)
            return nullptr;

        return env->NewObject(
            cls,
            constructor,
            static_cast<jint>(pkt->type),
            static_cast<jint>(pkt->flags),

            static_cast<jint>(pkt->payload_flags),
            static_cast<jlong>(pkt->timestamp_ms),
            payload
        );
    });

}
