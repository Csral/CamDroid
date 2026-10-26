#pragma once
#ifndef CAMDROID_ARGS_H
#define CAMDROID_ARGS_H

#include <cstdint>
#include <stdexcept>
#include <string>

namespace camdroid {

    struct app_config {

        std::uint16_t port = 6969;
        bool preview = false;
        bool gui = false;
        bool headless = false;
        bool webcam = false;

        bool install_webcam = false;
        bool uninstall_webcam = false;
        bool help = false;

    };

    namespace valid_arguments {

        namespace exceptions {

            class missing_arguments : public std::runtime_error {

                public:
                explicit missing_arguments(const std::string& msg)
                    : std::runtime_error("Missing required argument: " + msg) {};

            };

            class app_invalid_argument : public std::invalid_argument {

                public:
                explicit app_invalid_argument(const std::string& msg)
                    : std::invalid_argument("Invalid argument passed: " + msg) {};

            };

        };

        inline void validate_arguments(const app_config& conf) {

            if (conf.install_webcam && conf.uninstall_webcam)
                throw exceptions::app_invalid_argument("Choose installation or removal of webcam support.");

            if (conf.gui && conf.preview)
                throw exceptions::app_invalid_argument("Choose --gui or --preview, not both.");

            if (conf.headless && (conf.gui || conf.preview))
                throw exceptions::app_invalid_argument("--headless cannot be combined with a window mode.");

            // Matches the current tcp_socket::bind() policy.
            if (conf.port < 4097)
                throw exceptions::app_invalid_argument("Port must be between 4097 and 65535.");

            return;

        }

    };

};

#endif