#pragma once

#include "core/error.hpp"
#include "core/fd.hpp"
#include "file/file_service.hpp"
#include "terminal/terminal_session.hpp"

// See http_util.hpp: libhv headers need the POSIX headers first.
#include <fcntl.h>
#include <sys/stat.h>

#include "HttpMessage.h"

#include <cstdint>
#include <memory>
#include <string>

// Per-request state for streaming handlers. Stored on HttpContext userdata;
// middleware.cpp owns its release path so a rejected request never leaves a
// temp file behind. This is the only writer of HttpContext::userdata.

namespace edgedesk {

enum class RequestStateKind { file_upload, extension_upload };

struct RequestState {
    explicit RequestState(RequestStateKind kind) : kind(kind) {}
    virtual ~RequestState() = default;
    RequestState(const RequestState&) = delete;
    RequestState& operator=(const RequestState&) = delete;
    const RequestStateKind kind;
};

struct UploadRequestState : RequestState {
    UploadRequestState() : RequestState(RequestStateKind::file_upload) {}
    FileService::Upload upload;
    std::string path;
    Error error;
    bool failed = false;
    std::uint64_t received = 0;
};

struct ExtensionUploadRequestState : RequestState {
    ExtensionUploadRequestState() : RequestState(RequestStateKind::extension_upload) {}
    UniqueFd fd;
    std::string temp_path;
    std::string app_id;   // upgrade target (empty for install)
    std::string filename;
    bool restore_data = false;
    bool overwrite_same_version = false;
    bool allow_downgrade = false;
    bool failed = false;
    Error error;
    std::uint64_t received = 0;
    std::int64_t started_ms = 0;
};

// Frees pending request state (if any) and clears the pointer. Safe for any
// RequestState kind.
void release_upload_state(const HttpContextPtr& context);

} // namespace edgedesk
