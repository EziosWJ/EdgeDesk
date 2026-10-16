#pragma once

#include <string>

// Core error vocabulary shared by services and HTTP routes.
// Only this header maps business errors to HTTP status codes, so the same
// error keeps identical semantics across every endpoint.

namespace edgedesk {

enum class ErrorCode {
    ok = 0,
    invalid_argument,
    not_found,
    permission_denied,
    already_exists,
    not_a_directory,
    is_a_directory,
    too_large,
    io_error,
    unsupported,
    conflict,
    busy,
    limit_exceeded,
    unavailable,
};

struct Error {
    ErrorCode code = ErrorCode::ok;
    std::string message;
    int system_errno = 0;

    bool ok() const { return code == ErrorCode::ok; }
    static Error none() { return {}; }
    static Error make(ErrorCode code, std::string message, int system_errno = 0) {
        return Error{code, std::move(message), system_errno};
    }
};

const char* error_code_name(ErrorCode code);

// Stable machine-readable code used in JSON error bodies.
const char* error_code_slug(ErrorCode code);

int http_status_for(ErrorCode code);

} // namespace edgedesk
