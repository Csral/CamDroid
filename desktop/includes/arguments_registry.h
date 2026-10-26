#pragma once
#ifndef CAMDROID_ARGUMENTS_REGISTRY_H
#define CAMDROID_ARGUMENTS_REGISTRY_H

#include <iostream>
#include <string>
#include <vector>

namespace camdroid {

    struct argument_registry_entry {

        std::string name;
        std::vector<std::string> aliases;
        std::string description;

    };

    typedef struct argument_registry_entry argument_registry_t;
    typedef std::vector<argument_registry_t> registry_t;

    namespace arguments_registry {

        inline const registry_t general = {
            {"--webcam", {}, "Enable the Windows 11 virtual webcam with selectable output formats."},
            {"--install-webcam",
             {},
             "Development helper: install webcam support as administrator. The MSI handles normal installation."},

            {"--uninstall-webcam", {}, "Remove installed webcam support; requires administrator access."},
            {"--headless", {}, "Run the receiver in CLI mode without opening a window."},
            {"--help", {"-h"}, "Display this help message without starting the server."},
            {"--port PORT", {"-p PORT"}, "Listening TCP port, 4097-65535. Default: 6969. Also accepts --port=PORT."},
            {"--preview", {}, "Display decoded video in a standalone Qt6 preview window."},

            {"--gui", {}, "Open receiver controls, video preview, status and connection events."}
        };

        inline void print_registry(const registry_t& registry, std::ostream& output = std::cout) {

            if (registry.empty()) {

                output << "    (none)\n";

                return;

            }

            for (const auto& entry : registry) {

                output << "    " << entry.name;

                if (!entry.aliases.empty()) {

                    output << " (";

                    for (std::size_t i = 0; i < entry.aliases.size(); i++) {

                        output << entry.aliases[i];

                        if (i + 1 < entry.aliases.size())
                            output << ", ";

                    }

                    output << ")";

                }

                output << '\n' << "        " << entry.description << "\n\n";

            }

            return;

        }

    };

};

#endif