#ifndef CAMDROID_VIRTUAL_CAMERA_H
#define CAMDROID_VIRTUAL_CAMERA_H
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <optional>
#include "video_control.h"

#include "video_payload.h"

struct AVFrame;

namespace camdroid {

    class virtual_camera {

        private:
        struct implementation;
        std::unique_ptr<implementation> state;

        public:
        virtual_camera();
        ~virtual_camera();
        virtual_camera(const virtual_camera&) = delete;
        virtual_camera& operator=(const virtual_camera&) = delete;

        void enable(bool enabled);
        void install(bool remove = false);
        bool active() const;
        bool pending() const;
        std::string error() const;

        std::string notice() const;
        void clear();
        void allow_app_outputs(bool allow);
        bool app_outputs_allowed() const;
        void set_settings(protocol::video_settings settings);

        std::optional<protocol::video_format> take_request();
        void publish_rgba(const std::uint8_t* data, int width, int height, int stride);
        void publish_frame(const AVFrame& frame, int rotation = -1, bool mirrored = false);
        static int install_source(bool remove);

    };

};

#endif