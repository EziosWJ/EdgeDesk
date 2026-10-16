#include "app/config.hpp"

#include <limits>

namespace edgedesk {

std::string help_text() {
    return "EdgeDesk Phase 1\n"
           "Usage: edgedesk [--host 127.0.0.1] [--port 8080] [--web-root PATH]\n"
           "Note: Phase 1 has NO login/authentication; keep --host on loopback.\n";
}

Config parse_args(int argc, const char* const argv[]) {
    Config config;
#ifdef EDGEDESK_DISK_DEFAULT_DIR
    config.web_root = EDGEDESK_DISK_DEFAULT_DIR;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            throw std::invalid_argument(help_text());
        }
        if (arg != "--host" && arg != "--port" && arg != "--web-root") {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
        if (++i >= argc) {
            throw std::invalid_argument("Missing value for " + arg);
        }
        const std::string value = argv[i];
        if (value.empty()) {
            throw std::invalid_argument("Empty value for " + arg);
        }
        if (arg == "--host") {
            // Phase 1: prevent accidental public exposure before authentication is designed.
            if (value != "127.0.0.1" && value != "localhost" && value != "::1") {
                throw std::invalid_argument("Phase 1 only permits loopback --host (127.0.0.1 / ::1)");
            }
            config.host = value == "localhost" ? "127.0.0.1" : value;
        } else if (arg == "--web-root") {
            config.web_root = value;
        } else {
            if (value.find_first_not_of("0123456789") != std::string::npos) {
                throw std::invalid_argument("Port must be a decimal integer");
            }
            try {
                const auto port = std::stoul(value);
                if (port == 0 || port > std::numeric_limits<std::uint16_t>::max()) {
                    throw std::invalid_argument("Port must be between 1 and 65535");
                }
                config.port = static_cast<std::uint16_t>(port);
            } catch (const std::out_of_range&) {
                throw std::invalid_argument("Port out of range");
            }
        }
    }
    return config;
}

} // namespace edgedesk
