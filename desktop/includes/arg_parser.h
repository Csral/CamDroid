#pragma once
#ifndef CAMDROID_ARG_PARSER_H
#define CAMDROID_ARG_PARSER_H

#include "args.h"
#include "arguments_registry.h"

#include <iostream>
#include <string>
#include <vector>

namespace camdroid {

    inline void show_help(std::ostream& output = std::cout) {

        output << R"(
CamDroid Desktop
================
Receive and decode video from the CamDroid Android app.

USAGE
-----
    camdroid_desktop [OPTIONS]

GENERAL OPTIONS
---------------
)";

        arguments_registry::print_registry(arguments_registry::general, output);

        output << R"(EXAMPLES
--------
    camdroid_desktop
    camdroid_desktop --port 7000
    camdroid_desktop --port=7000
    camdroid_desktop --help

NOTES
-----
    With no options, open the desktop GUI and listen on port 6969.
    --headless runs the receiver in the console without a window.
    Repeating --port uses the last supplied valid value.
    --preview opens only video. --gui explicitly selects the default GUI.
    F11 toggles fullscreen. An occupied port tries up to five alternatives;
    use the reported listening port in Android. Ctrl+C stops CLI mode.

)";

        return;

    }

    namespace utils {

        inline std::string get_option_value(std::size_t ctr, const std::vector<std::string>& arguments) {
            if (++ctr >= arguments.size() || arguments[ctr].empty() || arguments[ctr][0] == '-')
                throw valid_arguments::exceptions::missing_arguments("--port expects a decimal port number.");

            return arguments[ctr];
        }

        inline std::uint16_t parse_port(const std::string& value) {

            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw valid_arguments::exceptions::app_invalid_argument("Port must contain decimal digits only: " + value);

            unsigned long long port;

            try {

                port = std::stoull(value);

            } catch (const std::out_of_range&) {

                throw valid_arguments::exceptions::app_invalid_argument("Port exceeds 65535: " + value);

            }

            // Check before narrowing, so large values cannot wrap into valid ports.
            if (port < 4097 || port > 65535)
                throw valid_arguments::exceptions::app_invalid_argument("Port must be between 4097 and 65535: " + value);

            return static_cast<std::uint16_t>(port);

        }

    };

    inline app_config argument_parser(const std::vector<std::string>& arguments) {

        app_config configuration;

        // Preserve the original parser's help precedence without terminating
        // inside a library function. The entry point decides how to exit.
        for (const auto& arg : arguments) {

            if (arg == "--help" || arg == "-h") {

                configuration.help = true;

                return configuration;

            }

        }

        for (std::size_t i = 0; i < arguments.size(); i++) {

            const std::string& arg = arguments[i];

            if (arg == "--port" || arg == "-p") {

                configuration.port = utils::parse_port(utils::get_option_value(i, arguments));
                i++;

            } else if (arg.starts_with("--port=")) {

                configuration.port = utils::parse_port(arg.substr(7));

            } else if (arg == "--preview") {

                configuration.preview = true;

            } else if (arg == "--gui") {

                configuration.gui = true;

            } else if (arg == "--webcam") {

                configuration.webcam = true;

            } else if (arg == "--install-webcam") {

                configuration.install_webcam = true;

            } else if (arg == "--uninstall-webcam") {

                configuration.uninstall_webcam = true;

            } else if (arg == "--headless") {

                configuration.headless = true;

            } else {

                throw valid_arguments::exceptions::app_invalid_argument("Unknown option or positional argument: " + arg);

            }

        }

        valid_arguments::validate_arguments(configuration);

        return configuration;

    }

    inline app_config argument_parser(int argc, char* argv[]) {
        if (argc < 1 || argv == nullptr)
            throw valid_arguments::exceptions::app_invalid_argument("Invalid argument array.");

        return argument_parser(std::vector<std::string>(argv + 1, argv + argc));
    }

};

#endif