#pragma once

#include "terminal/terminal_session.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <cstdint>

// Per-WebSocket-connection state. Stored on the channel context pointer so no
// global registry is needed.

namespace edgedesk {

struct WsClientState {
    std::string path;
    std::string peer_ip;

    // terminal
    std::shared_ptr<TerminalSession> session;
    std::string session_id;
    std::uint64_t subscription = 0;

    // metrics
    std::uint64_t metrics_subscription = 0;
    std::uint64_t lagged_samples = 0;

    std::atomic<bool> closed{false};
};

} // namespace edgedesk
