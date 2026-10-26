#include "tcp_socket.h"
#include "cli.h"
#include "application.h"
#include "virtual_camera.h"

#include <chrono>
#include <iostream>
#include <utility>
#include <atomic>
#include <csignal>

#include <thread>
#include <condition_variable>
#include <mutex>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace camdroid {

    namespace utils {

        class webcam_output {

            private:
            virtual_camera& camera;
            std::mutex mutex;
            std::condition_variable changed;
            std::shared_ptr<AVFrame> pending;

            std::chrono::steady_clock::time_point queued;
            std::jthread worker;

            public:
            explicit webcam_output(virtual_camera& webcam)
                : camera(webcam), worker([this](std::stop_token stop) {

                      while (!stop.stop_requested()) {

                          std::unique_lock lock(mutex);

                          changed.wait(lock, [&] {
                              return stop.stop_requested() || pending;
                          });

                          if (stop.stop_requested())
                              return;

                          auto frame = std::move(pending);

                          if (std::chrono::steady_clock::now() - queued <= std::chrono::milliseconds(75))
                              camera.publish_frame(*frame);

                      }

                  }) {}

            ~webcam_output() {
                worker.request_stop();
                changed.notify_all();
                worker.join();
            }

            void submit(const AVFrame& frame) {

                std::unique_lock lock(mutex, std::try_to_lock);

                if (!lock.owns_lock())
                    return;

                auto* copy = av_frame_clone(&frame);

                if (!copy)
                    throw std::bad_alloc();

                pending = std::shared_ptr<AVFrame>(copy, [](AVFrame* value) {
                    av_frame_free(&value);
                });

                queued = std::chrono::steady_clock::now();
                changed.notify_all();

            }

            void clear() {
                std::lock_guard lock(mutex);
                pending.reset();
                camera.clear();
            }

        };

        static volatile std::sig_atomic_t interrupted = 0;
        #ifdef _WIN32
        static std::atomic<bool> console_interrupted = false;

        static BOOL WINAPI console_stop(DWORD event) {

            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
                return FALSE;

            console_interrupted = true;

            return TRUE;

        }

        #else
        static void console_stop(int) {
            interrupted = 1;
        }

        #endif

    };

    int run_cli(const app_config& configuration) {

        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;
        auto last_report = std::chrono::steady_clock::now();
        bool report_next_frame = true;
        virtual_camera webcam;
        utils::webcam_output output(webcam);

        if (configuration.webcam)
            webcam.enable(true);

        application_callbacks callbacks;

        callbacks.on_decoder_selection = [](const decoder_selection& selection) {
            if (!selection.message.empty())
                std::cout << "Decoder: " << selection.message << '\n';
        };

        callbacks.on_negotiation = [](const std::string& message) {
            std::cout << message << '\n';
        };

        callbacks.on_frame = [&](const AVFrame& frame, std::uint64_t decoded_frames) {

            if (configuration.webcam)
                output.submit(frame);

            const auto now = std::chrono::steady_clock::now();

            if (!report_next_frame && now - last_report < std::chrono::seconds(1))
                return;

            report_next_frame = false;
            last_report = now;

            const char* format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format));

            auto pts = frame.pts;

            if (pts == AV_NOPTS_VALUE)
                pts = frame.best_effort_timestamp;

            std::cout << "Decoded frame #" << decoded_frames << " | " << frame.width << "x" << frame.height
                      << " | format=" << (format ? format : "unknown") << " | PTS=";

            if (pts == AV_NOPTS_VALUE)
                std::cout << "unknown";
            else
                std::cout << pts << " us";

            std::cout << " | keyframe=" << ((frame.flags & AV_FRAME_FLAG_KEY) != 0) << '\n';

            // Actual decoded pixels are available here:
            //
            // frame.data[plane]
            // frame.linesize[plane]
            //
            // Don't assume tightly packed rows.
            // Don't retain this frame reference after the callback.

        };

        callbacks.on_listening = [](std::uint16_t port) {

            std::cout << "Device: " << utils::local_device_name() << '\n';

            for (const auto& ip : utils::local_ipv4_addresses())
                std::cout << "Listening: " << ip << ':' << port << '\n';
            std::cout << "Waiting on port " << port << "...\n";

        };

        callbacks.on_error = [](const std::string& error) {
            std::cerr << "Connection error: " << error << '\n';
        };

        callbacks.on_connected = [] {
            std::cout << "Connected.\n";
        };

        callbacks.on_video_config = [&](const protocol::video_config& config) {

            output.clear();
            report_next_frame = true;
            std::cout << "H.264 decoder configured"
                      << " | csd0=" << config.csd0.size() << " bytes"
                      << " | csd1=" << config.csd1.size() << " bytes\n";

        };

        callbacks.on_video_settings = [&](protocol::video_settings settings) {
            webcam.set_settings(settings);

            if (settings.fps)
                std::cout << "Configured: " << settings.fps << " FPS | " << settings.bitrate / 1000000.0 << " Mbps\n";
        };

        callbacks.on_video_ended = [&](std::uint64_t count) {
            output.clear();
            std::cout << "Video stream ended. Total decoded frames: " << count << '\n';
        };

        callbacks.on_disconnected = [&](std::uint64_t count) {
            output.clear();
            std::cout << "Disconnected. Total decoded frames: " << count << '\n';
        };

        application app(application_config{configuration.port}, std::move(callbacks));
        utils::interrupted = 0;
        #ifdef _WIN32
        utils::console_interrupted = false;
        SetConsoleCtrlHandler(utils::console_stop, TRUE);

        #else
        const auto previous_handler = std::signal(SIGINT, utils::console_stop);
        #endif
        std::jthread cancellation([&](std::stop_token token) {

            std::string camera_error;

            while (!token.stop_requested()) {

                const auto error = webcam.error();

                if (!error.empty() && camera_error != error) {

                    std::cerr << "Webcam: " << error << '\n';
                    camera_error = error;

                }

                #ifdef _WIN32
                const bool stop = utils::console_interrupted;
                #else
                const bool stop = utils::interrupted != 0;
                #endif
                if (stop) {

                    app.request_stop();

                    return;

                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));

            }

        });

        try {

            app.run();

        } catch (...) {

            cancellation.request_stop();
            cancellation.join();
            #ifdef _WIN32
            SetConsoleCtrlHandler(utils::console_stop, FALSE);

            #else
            std::signal(SIGINT, previous_handler);
            #endif
            throw;

        }

        cancellation.request_stop();
        cancellation.join();
        #ifdef _WIN32
        SetConsoleCtrlHandler(utils::console_stop, FALSE);

        #else
        std::signal(SIGINT, previous_handler);
        #endif

        return 0;

    }

}