#pragma once

#include "file/file_service.hpp"
#include "terminal/terminal_session.hpp"

// See http_util.hpp: libhv headers need the POSIX headers first.
#include <fcntl.h>
#include <sys/stat.h>

#include "HttpMessage.h"

#include <memory>
#include <string>

// Per-request state for the streaming upload route. Stored on HttpContext
// userdata; middleware.cpp owns its release path so a rejected request never
// leaves a temp file behind. This is the only writer of HttpContext::userdata.

namespace edgedesk {

struct UploadRequestState {
    FileService::Upload upload;
    std::string path;
    Error error;
    bool failed = false;
    std::uint64_t received = 0;
};

// Frees pending upload state (if any) and clears the pointer.
void release_upload_state(const HttpContextPtr& context);

} // namespace edgedesk
