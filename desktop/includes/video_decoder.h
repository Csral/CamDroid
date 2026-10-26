#ifndef CAMDROID_VIDEO_DECODER_H
#define CAMDROID_VIDEO_DECODER_H
#pragma once

#include "video_payload.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVFrame;

namespace camdroid {

    struct decoder_option {

        std::string id;
        std::string name;
        bool hardware = false;
        bool specialized = false;
        double decode_ms = 0;
        int buffered_frames = 0;

    };

    struct decoder_selection {

        std::vector<decoder_option> options;
        std::string requested = "auto";
        std::string active;
        std::string message;
        std::vector<std::string> recommended;

        bool identifying = false;
        bool automatic = false;

    };

    class video_decoder {

        public:
        using frame_handler = std::function<void(const AVFrame&)>;
        using selection_handler = std::function<void(const decoder_selection&)>;
        using cancellation_handler = std::function<bool()>;

        private:
        struct state;
        std::unique_ptr<state> engine;

        public:
        explicit video_decoder(frame_handler handler, selection_handler selection = {}, cancellation_handler cancelled = {});
        ~video_decoder();

        video_decoder(const video_decoder&) = delete;
        video_decoder& operator=(const video_decoder&) = delete;

        void configure(const protocol::video_config& config);
        void set_preference(const std::string& id);
        void identify_best(bool select_best = true);
        void set_automatic(bool enabled);
        void update_settings(protocol::video_settings settings);

        // Returns false when footage is intentionally ignored:
        // before CONFIG, before the first keyframe, or after EOS.
        bool decode(const protocol::video_frame& footage, bool keyframe);

        // Delivers any delayed decoded frames. Safe to call repeatedly.
        void drain();
        void discontinuity();

    };

};

#endif