#pragma once

#include "core/fd.hpp"

#include <sys/stat.h>

#include <cstddef>
#include <string>
#include <string_view>

// Small, race-resistant persistence helpers for security state.
//
// Files are created relative to an already-open data-directory descriptor so
// callers never re-resolve a path after validation. Writes use a temporary
// file in the same directory, fsync, renameat and a final directory fsync.

namespace edgedesk {

// Creates `path` with mode 0700 when missing and verifies it is a directory
// owned by the current effective user. Existing permissions are tightened to
// 0700. Returns a directory descriptor on success.
UniqueFd open_private_dir(const std::string& path, std::string& error);

// Reads a regular file relative to `dir_fd`. `missing` is true for ENOENT.
bool read_file_at(int dir_fd, const std::string& name, std::size_t max_bytes, std::string& out,
                  bool& missing, std::string& error);

// Atomically replaces `name` under `dir_fd` with `data`. `mode` is applied with
// fchmod before the rename. Rejects names containing '/' or NUL.
bool atomic_write_file_at(int dir_fd, const std::string& name, std::string_view data, mode_t mode,
                          std::string& error);

// Removes `name` under `dir_fd`; a missing file is not an error.
bool remove_file_at(int dir_fd, const std::string& name, std::string& error);

} // namespace edgedesk
