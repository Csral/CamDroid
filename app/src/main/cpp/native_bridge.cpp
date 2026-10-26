#include <jni.h>
#include "protocol.h"
#include "session.h"
#include <cstdint>
#include <exception>

#include <memory>
#include <limits>
#include <stdexcept>
#include "packet_builder.h"
#include "video_payload.h"

#include <chrono>
#include <utility>
#include <mutex>
#include <unordered_map>

// Compatibility/learning API. Production streaming uses transport_bridge.cpp
// and sends video directly from native storage, without returning frame arrays.

namespace utils {

    // Compatibility API used by the learning examples. The production transport
    // has its own registry; neither API exposes pointers as handles anymore.
    struct session_entry {

        camdroid::protocol::session connection;
        std::mutex mutex;

    };

    struct session_access {

        std::shared_ptr<session_entry> owner;
        std::unique_lock<std::mutex> lock;

        explicit session_access(std::shared_ptr<session_entry> value) : owner(std::move(value)), lock(owner->mutex) {}

    };

    static std::mutex session_registry_mutex;
    static std::unordered_map<jlong, std::shared_ptr<session_entry>> sessions;
    static jlong next_session_handle = -1;

    static session_access lookup_session(jlong handle) {

        std::shared_ptr<session_entry> entry;

        {

            std::lock_guard lock(session_registry_mutex);
            auto found = sessions.find(handle);

            if (found == sessions.end())
                throw std::runtime_error("Native session is closed or invalid.");

            entry = found->second;

        }

        return session_access(std::move(entry));

    }

    static jbyteArray serialize_video(JNIEnv* env, camdroid::protocol::session& connection, camdroid::protocol::packet pkt) {

        if (connection.get_state() != camdroid::protocol::SessionState::Established)
            throw std::runtime_error("Video requires an established session.");

        pkt.set_flag(camdroid::protocol::ProtocolFlag::NACK);
        pkt.set_flag(camdroid::protocol::ProtocolFlag::OPTS, connection.is_optimized());

        pkt.timestamp_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
        );

        auto bytes = connection.send(pkt);

        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max()))
            throw std::length_error("Video packet exceeds Java array size.");

        auto size = static_cast<jsize>(bytes.size());
        jbyteArray result = env->NewByteArray(size);

        if (result == nullptr)
            return nullptr;

        env->SetByteArrayRegion(result, 0, size, reinterpret_cast<const jbyte*>(bytes.data()));

        if (env->ExceptionCheck())
            return nullptr;

        return result;

    }

}

extern "C" JNIEXPORT jint JNICALL Java_csral_camdroid_NativeBridge_protocolVersion(JNIEnv*, jobject) {
    return static_cast<jint>(camdroid::protocol::version);
}

extern "C" JNIEXPORT jlong JNICALL Java_csral_camdroid_NativeBridge_createSession(JNIEnv* env, jobject) {

    try {

        auto entry = std::make_shared<utils::session_entry>();
        entry->connection.start();
        std::lock_guard lock(utils::session_registry_mutex);

        if (utils::next_session_handle == std::numeric_limits<jlong>::min())
            throw std::runtime_error("Native session handle space exhausted.");

        const jlong handle = utils::next_session_handle--;
        utils::sessions.emplace(handle, std::move(entry));

        return handle;

    } catch (const std::exception& e) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr) {

            env->ThrowNew(exception, e.what());

        }

        return 0;

    }

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_NativeBridge_closeSession(JNIEnv* env, jobject, jlong handle) {

    try {

        std::shared_ptr<utils::session_entry> entry;

        {

            std::lock_guard lock(utils::session_registry_mutex);
            auto found = utils::sessions.find(handle);

            if (found == utils::sessions.end())
                return;

            entry = std::move(found->second);
            utils::sessions.erase(found);

        }

        std::lock_guard lock(entry->mutex);
        entry->connection.close();

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

    }

}

extern "C" JNIEXPORT void JNICALL
Java_csral_camdroid_NativeBridge_feedSession(JNIEnv* env, jobject, jlong handle, jbyteArray data, jint size) {

    if (handle == 0 || data == nullptr || size < 0 || size > env->GetArrayLength(data)) {

        auto exception = env->FindClass("java/lang/IllegalArgumentException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Invalid session handle or input buffer.");

        return;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));

        if (size > 0) {

            env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(bytes.data()));

            if (env->ExceptionCheck())
                return;

        }

        connection->feed(bytes.data(), bytes.size());

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

    }

}

extern "C" JNIEXPORT jboolean JNICALL Java_csral_camdroid_NativeBridge_processNextPacket(JNIEnv* env, jobject, jlong handle) {

    if (handle == 0) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Native session is closed.");

        return JNI_FALSE;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        auto pkt = connection->next_packet();

        return pkt.has_value() ? JNI_TRUE : JNI_FALSE;

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return JNI_FALSE;

    }

}

extern "C" JNIEXPORT jobject JNICALL Java_csral_camdroid_NativeBridge_nextResponse(JNIEnv* env, jobject, jlong handle) {

    if (handle == 0) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Native session is closed.");

        return nullptr;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        auto response = connection->next_response();

        if (!response.has_value())
            return nullptr;

        const std::vector<std::uint8_t> terminal_payload{
            static_cast<std::uint8_t>(camdroid::protocol::CommunicationOpcode::Handshake),
            0x00,
            0xAE,
            0xEF,

            0x00,
            0x11,
            0xFF
        };

        bool terminal_fin =
            connection->is_terminal_fin_pending() && response->type == camdroid::protocol::PacketType::Communication &&
            response->has_flag(camdroid::protocol::ProtocolFlag::ACK) &&
            response->has_flag(camdroid::protocol::ProtocolFlag::FIN) && response->payload == terminal_payload;

        auto bytes = connection->send(*response);

        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max()))
            throw std::length_error("Native response exceeds Java array size.");

        auto size = static_cast<jsize>(bytes.size());
        jbyteArray data = env->NewByteArray(size);

        if (data == nullptr)
            return nullptr;

        if (size > 0) {

            env->SetByteArrayRegion(data, 0, size, reinterpret_cast<const jbyte*>(bytes.data()));

            if (env->ExceptionCheck())
                return nullptr;

        }

        auto response_class = env->FindClass("csral/camdroid/NativeResponse");

        if (response_class == nullptr)
            return nullptr;

        auto constructor = env->GetMethodID(response_class, "<init>", "([BZ)V");

        if (constructor == nullptr)
            return nullptr;

        return env->NewObject(response_class, constructor, data, terminal_fin ? JNI_TRUE : JNI_FALSE);

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return nullptr;

    }

}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_csral_camdroid_NativeBridge_initialHandshake(JNIEnv* env, jobject, jlong handle, jboolean optimized) {

    if (handle == 0) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Native session is closed.");

        return nullptr;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        if (connection->get_state() != camdroid::protocol::SessionState::Handshaking)
            throw std::runtime_error("Session is not handshaking.");

        std::vector<std::uint8_t> payload{
            static_cast<std::uint8_t>(camdroid::protocol::CommunicationOpcode::Handshake),
            camdroid::protocol::min_supported_version,
            camdroid::protocol::max_supported_version,
            static_cast<std::uint8_t>(optimized == JNI_TRUE ? 0x80 : 0x00)
        };

        camdroid::protocol::packet_builder builder;

        auto request = builder.type(camdroid::protocol::PacketType::Communication)
                           .flag(camdroid::protocol::ProtocolFlag::QRY)
                           .payload(payload)
                           .build();

        auto bytes = connection->send(request);
        auto size = static_cast<jsize>(bytes.size());

        jbyteArray result = env->NewByteArray(size);

        if (result == nullptr)
            return nullptr;

        env->SetByteArrayRegion(result, 0, size, reinterpret_cast<const jbyte*>(bytes.data()));

        if (env->ExceptionCheck())
            return nullptr;

        return result;

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return nullptr;

    }

}

extern "C" JNIEXPORT jboolean JNICALL
Java_csral_camdroid_NativeBridge_isTerminalFinPending(JNIEnv* env, jobject, jlong handle) {

    if (handle == 0) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Native session is closed.");

        return JNI_FALSE;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        return connection->is_terminal_fin_pending() ? JNI_TRUE : JNI_FALSE;

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return JNI_FALSE;

    }

}

extern "C" JNIEXPORT void JNICALL Java_csral_camdroid_NativeBridge_terminalFinSent(JNIEnv* env, jobject, jlong handle) {

    if (handle == 0) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Native session is closed.");

        return;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        connection->on_terminal_fin_sent();

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

    }

}

extern "C" JNIEXPORT jboolean JNICALL Java_csral_camdroid_NativeBridge_isEstablished(JNIEnv* env, jobject, jlong handle) {

    if (handle == 0) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Native session is closed.");

        return JNI_FALSE;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        return connection->get_state() == camdroid::protocol::SessionState::Established ? JNI_TRUE : JNI_FALSE;

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return JNI_FALSE;

    }

}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_csral_camdroid_NativeBridge_videoConfig(JNIEnv* env, jobject, jlong handle, jbyteArray csd0, jbyteArray csd1) {

    if (handle == 0 || csd0 == nullptr || csd1 == nullptr) {

        auto exception = env->FindClass("java/lang/IllegalArgumentException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Invalid session or configuration buffers.");

        return nullptr;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        if (connection->get_state() != camdroid::protocol::SessionState::Established)
            throw std::runtime_error("Video requires an established session.");

        camdroid::protocol::video_config config;
        config.codec = camdroid::protocol::VideoCodec::H264;

        jsize size0 = env->GetArrayLength(csd0);
        jsize size1 = env->GetArrayLength(csd1);

        config.csd0.resize(static_cast<std::size_t>(size0));
        config.csd1.resize(static_cast<std::size_t>(size1));

        if (size0 > 0) {

            env->GetByteArrayRegion(csd0, 0, size0, reinterpret_cast<jbyte*>(config.csd0.data()));

            if (env->ExceptionCheck())
                return nullptr;

        }

        if (size1 > 0) {

            env->GetByteArrayRegion(csd1, 0, size1, reinterpret_cast<jbyte*>(config.csd1.data()));

            if (env->ExceptionCheck())
                return nullptr;

        }

        auto pkt = camdroid::protocol::video_payload::make_config(config);

        pkt.set_flag(camdroid::protocol::ProtocolFlag::NACK);
        pkt.set_flag(camdroid::protocol::ProtocolFlag::OPTS, connection->is_optimized());

        pkt.timestamp_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
        );

        auto bytes = connection->send(pkt);

        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max()))
            throw std::length_error("Video configuration exceeds Java array size.");

        auto size = static_cast<jsize>(bytes.size());
        jbyteArray result = env->NewByteArray(size);

        if (result == nullptr)
            return nullptr;

        env->SetByteArrayRegion(result, 0, size, reinterpret_cast<const jbyte*>(bytes.data()));

        if (env->ExceptionCheck())
            return nullptr;

        return result;

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return nullptr;

    }

}

extern "C" JNIEXPORT jbyteArray JNICALL Java_csral_camdroid_NativeBridge_videoFrame(
    JNIEnv* env,
    jobject,
    jlong handle,
    jbyteArray data,

    jlong pts_us,
    jboolean keyframe
) {

    if (handle == 0 || data == nullptr || pts_us < 0) {

        auto exception = env->FindClass("java/lang/IllegalArgumentException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Invalid session, frame or timestamp.");

        return nullptr;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        jsize size = env->GetArrayLength(data);

        if (size == 0)
            throw std::invalid_argument("Encoded frame must not be empty.");

        camdroid::protocol::video_frame frame;
        frame.pts_us = static_cast<std::uint64_t>(pts_us);
        frame.data.resize(static_cast<std::size_t>(size));

        env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(frame.data.data()));

        if (env->ExceptionCheck())
            return nullptr;

        auto pkt = camdroid::protocol::video_payload::make_frame(frame, keyframe == JNI_TRUE);

        return utils::serialize_video(env, *connection, std::move(pkt));

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return nullptr;

    }

}

extern "C" JNIEXPORT jbyteArray JNICALL Java_csral_camdroid_NativeBridge_videoEos(JNIEnv* env, jobject, jlong handle) {

    if (handle == 0) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, "Native session is closed.");

        return nullptr;

    }

    try {

        auto access = utils::lookup_session(handle);
        auto* connection = &access.owner->connection;

        auto pkt = camdroid::protocol::video_payload::make_eos();

        return utils::serialize_video(env, *connection, std::move(pkt));

    } catch (const std::exception& error) {

        auto exception = env->FindClass("java/lang/IllegalStateException");

        if (exception != nullptr)
            env->ThrowNew(exception, error.what());

        return nullptr;

    }

}
