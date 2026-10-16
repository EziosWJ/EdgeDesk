#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace edgedesk {

struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string web_root;
};

// Throws std::invalid_argument for invalid flags; run --help to view usage.
Config parse_args(int argc, const char* const argv[]);
std::string help_text();

} // namespace edgedesk
