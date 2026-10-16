#include "core/error.hpp"

namespace edgedesk {

const char* error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::ok: return "ok";
        case ErrorCode::invalid_argument: return "invalid_argument";
        case ErrorCode::not_found: return "not_found";
        case ErrorCode::permission_denied: return "permission_denied";
        case ErrorCode::already_exists: return "already_exists";
        case ErrorCode::not_a_directory: return "not_a_directory";
        case ErrorCode::is_a_directory: return "is_a_directory";
        case ErrorCode::too_large: return "too_large";
        case ErrorCode::io_error: return "io_error";
        case ErrorCode::unsupported: return "unsupported";
        case ErrorCode::conflict: return "conflict";
        case ErrorCode::busy: return "busy";
        case ErrorCode::limit_exceeded: return "limit_exceeded";
        case ErrorCode::unavailable: return "unavailable";
    }
    return "unknown";
}

const char* error_code_slug(ErrorCode code) {
    switch (code) {
        case ErrorCode::ok: return "ok";
        case ErrorCode::invalid_argument: return "invalid_argument";
        case ErrorCode::not_found: return "not_found";
        case ErrorCode::permission_denied: return "permission_denied";
        case ErrorCode::already_exists: return "already_exists";
        case ErrorCode::not_a_directory: return "not_a_directory";
        case ErrorCode::is_a_directory: return "is_a_directory";
        case ErrorCode::too_large: return "too_large";
        case ErrorCode::io_error: return "io_error";
        case ErrorCode::unsupported: return "unsupported";
        case ErrorCode::conflict: return "conflict";
        case ErrorCode::busy: return "busy";
        case ErrorCode::limit_exceeded: return "limit_exceeded";
        case ErrorCode::unavailable: return "unavailable";
    }
    return "unknown";
}

int http_status_for(ErrorCode code) {
    switch (code) {
        case ErrorCode::ok: return 200;
        case ErrorCode::invalid_argument: return 400;
        case ErrorCode::not_found: return 404;
        case ErrorCode::permission_denied: return 403;
        case ErrorCode::already_exists: return 409;
        case ErrorCode::not_a_directory: return 400;
        case ErrorCode::is_a_directory: return 409;
        case ErrorCode::too_large: return 413;
        case ErrorCode::io_error: return 500;
        case ErrorCode::unsupported: return 501;
        case ErrorCode::conflict: return 409;
        case ErrorCode::busy: return 423;
        case ErrorCode::limit_exceeded: return 429;
        case ErrorCode::unavailable: return 503;
    }
    return 500;
}

} // namespace edgedesk
