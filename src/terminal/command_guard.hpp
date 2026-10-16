#pragma once

#include <optional>
#include <string>
#include <string_view>

// CommandGuard: heuristic protection against obviously destructive commands
// (ADR-0005). It is a UX guard, NOT a security boundary: Bash runs with the
// service user's real permissions.

namespace edgedesk {

struct GuardMatch {
    std::string id;          // stable pattern identifier
    std::string description;  // user-facing (Chinese) explanation
};

class CommandGuard {
public:
    // Inspects a single shell line (without the trailing newline).
    static std::optional<GuardMatch> inspect(std::string_view line);

    // True for control characters that reset line tracking (arrows, Ctrl+A...).
    static bool breaks_line_tracking(unsigned char byte);
};

} // namespace edgedesk
