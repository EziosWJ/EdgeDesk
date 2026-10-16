#pragma once

#include "core/error.hpp"
#include "core/fd.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

// forkpty wrapper. The child is always exec'd immediately (no async-unsafe
// calls between fork and exec): environment and arguments are prepared before
// the fork.

namespace edgedesk {

struct PtySpawnOptions {
    std::string shell = "/bin/bash";
    std::vector<std::string> arguments = {"-l"};
    std::string cwd;                        // empty = inherit service cwd
    std::string term = "xterm-256color";
    std::vector<std::pair<std::string, std::string>> extra_env;
    unsigned short cols = 80;
    unsigned short rows = 24;
};

class PtyProcess {
public:
    static std::unique_ptr<PtyProcess> spawn(const PtySpawnOptions& options, Error& error);

    PtyProcess(const PtyProcess&) = delete;
    PtyProcess& operator=(const PtyProcess&) = delete;
    ~PtyProcess();

    int master_fd() const { return master_.get(); }
    pid_t pid() const { return pid_; }

    Error resize(unsigned short cols, unsigned short rows);

    // Waits up to timeout_ms for readable output. Returns 1 readable, 0 timeout,
    // -1 error/hangup.
    int wait_readable(int timeout_ms) const;

    // Reads from the master; returns bytes read, 0 on EOF/hangup, -1 on error
    // (errno == EAGAIN when nothing is available right now).
    long read_some(char* buffer, std::size_t length);

    Error write_all(std::string_view data);

    // Non-blocking child status check. Returns true when the child has exited.
    bool child_exited(int& status);

    // SIGHUP with a short grace period, then SIGKILL; reaps the child.
    void terminate_and_reap();

    bool reaped() const { return reaped_; }

private:
    PtyProcess(UniqueFd master, pid_t pid) : master_(std::move(master)), pid_(pid) {}
    void hard_kill_and_reap();

    UniqueFd master_;
    pid_t pid_ = -1;
    bool reaped_ = false;
};

} // namespace edgedesk
