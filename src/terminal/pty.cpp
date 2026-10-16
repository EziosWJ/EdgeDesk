#include "terminal/pty.hpp"

#include "core/util.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace edgedesk {
namespace {

constexpr int kTerminateGraceMs = 700;

std::vector<std::string> build_environment(const PtySpawnOptions& options) {
    std::vector<std::string> env;
    for (char** item = environ; item != nullptr && *item != nullptr; ++item) {
        env.emplace_back(*item);
    }
    auto has_key = [&env](const std::string& key) {
        const std::string prefix = key + "=";
        for (const auto& entry : env) {
            if (entry.rfind(prefix, 0) == 0) return true;
        }
        return false;
    };
    auto set_key = [&env, &has_key](const std::string& key, const std::string& value) {
        const std::string prefix = key + "=";
        if (has_key(key)) {
            for (auto& entry : env) {
                if (entry.rfind(prefix, 0) == 0) {
                    entry = prefix + value;
                    return;
                }
            }
        } else {
            env.push_back(prefix + value);
        }
    };
    set_key("TERM", options.term);
    if (!has_key("COLORTERM")) set_key("COLORTERM", "truecolor");
    if (!has_key("LANG")) set_key("LANG", "C.UTF-8");
    if (!has_key("HOME")) {
        // Fall back to the passwd database so login shells find their dotfiles.
        const auto* home = ::getenv("HOME");
        set_key("HOME", home != nullptr ? home : "/");
    }
    for (const auto& pair : options.extra_env) {
        set_key(pair.first, pair.second);
    }
    return env;
}

void reset_child_signals() {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGPIPE, SIGCHLD, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGTSTP, SIGTTIN, SIGTTOU}) {
        ::sigaction(signal, &action, nullptr);
    }
}

} // namespace

std::unique_ptr<PtyProcess> PtyProcess::spawn(const PtySpawnOptions& options, Error& error) {
    if (options.shell.empty()) {
        error = Error::make(ErrorCode::invalid_argument, "shell path must not be empty");
        return nullptr;
    }
    // Prepare everything that the child needs before forking: only
    // async-signal-safe calls are made between fork and exec.
    const auto env_strings = build_environment(options);
    std::vector<char*> env_ptrs;
    env_ptrs.reserve(env_strings.size() + 1);
    for (const auto& entry : env_strings) {
        env_ptrs.push_back(const_cast<char*>(entry.c_str()));
    }
    env_ptrs.push_back(nullptr);

    std::vector<std::string> argv_strings;
    argv_strings.push_back(options.shell);
    for (const auto& argument : options.arguments) argv_strings.push_back(argument);
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_strings.size() + 1);
    for (const auto& entry : argv_strings) {
        argv_ptrs.push_back(const_cast<char*>(entry.c_str()));
    }
    argv_ptrs.push_back(nullptr);

    struct winsize window {};
    window.ws_col = options.cols;
    window.ws_row = options.rows;
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &window);
    if (pid < 0) {
        error = Error::make(ErrorCode::io_error, std::string("forkpty failed: ") + std::strerror(errno), errno);
        return nullptr;
    }
    if (pid == 0) {
        // Child: async-signal-safe calls only.
        if (!options.cwd.empty()) {
            if (::chdir(options.cwd.c_str()) != 0) {
                // Fall through: HomeDirectory fallback would hide real errors, so
                // report via _exit and let the parent observe the exit status.
                ::_exit(126);
            }
        }
        reset_child_signals();
        ::execve(options.shell.c_str(), argv_ptrs.data(), env_ptrs.data());
        ::_exit(127);
    }

    UniqueFd master_fd(master);
    // Non-blocking master keeps read/write safe in combination with poll().
    const int flags = ::fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }
    error = Error::none();
    return std::unique_ptr<PtyProcess>(new PtyProcess(std::move(master_fd), pid));
}

PtyProcess::~PtyProcess() {
    if (!reaped_ && pid_ > 0) {
        hard_kill_and_reap();
    }
}

Error PtyProcess::resize(unsigned short cols, unsigned short rows) {
    if (!master_.valid()) {
        return Error::make(ErrorCode::conflict, "pty is closed");
    }
    if (cols == 0) cols = 80;
    if (rows == 0) rows = 24;
    struct winsize window {};
    window.ws_col = cols;
    window.ws_row = rows;
    if (::ioctl(master_.get(), TIOCSWINSZ, &window) != 0) {
        return Error::make(ErrorCode::io_error, std::string("TIOCSWINSZ failed: ") + std::strerror(errno), errno);
    }
    return Error::none();
}

int PtyProcess::wait_readable(int timeout_ms) const {
    if (!master_.valid()) return -1;
    struct pollfd descriptor {};
    descriptor.fd = master_.get();
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result < 0) return errno == EINTR ? 0 : -1;
    if (result == 0) return 0;
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (descriptor.revents & POLLIN) == 0) {
        return -1;
    }
    return 1;
}

long PtyProcess::read_some(char* buffer, std::size_t length) {
    if (!master_.valid()) return 0;
    const auto result = ::read(master_.get(), buffer, length);
    if (result >= 0) return static_cast<long>(result);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
    return 0; // EIO: the slave side is gone
}

Error PtyProcess::write_all(std::string_view data) {
    if (!master_.valid()) {
        return Error::make(ErrorCode::conflict, "pty is closed");
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(master_.get(), data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Master buffer is full: brief wait then retry (bounded by 2 s).
                struct pollfd descriptor {};
                descriptor.fd = master_.get();
                descriptor.events = POLLOUT;
                if (::poll(&descriptor, 1, 200) <= 0) {
                    return Error::make(ErrorCode::busy, "pty write buffer full");
                }
                continue;
            }
            return Error::make(ErrorCode::io_error, std::string("pty write failed: ") + std::strerror(errno), errno);
        }
        offset += static_cast<std::size_t>(written);
    }
    return Error::none();
}

bool PtyProcess::child_exited(int& status) {
    if (reaped_) return true;
    if (pid_ <= 0) return true;
    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
        reaped_ = true;
        return true;
    }
    if (result < 0 && errno == ECHILD) {
        reaped_ = true;
        return true;
    }
    return false;
}

void PtyProcess::hard_kill_and_reap() {
    if (pid_ > 0 && !reaped_) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        reaped_ = true;
    }
    master_.reset();
}

void PtyProcess::terminate_and_reap() {
    if (pid_ > 0 && !reaped_) {
        ::kill(pid_, SIGHUP);
        const auto deadline = steady_millis() + kTerminateGraceMs;
        int status = 0;
        for (;;) {
            const pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) {
                reaped_ = true;
                break;
            }
            if (steady_millis() >= deadline) {
                ::kill(pid_, SIGKILL);
                while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
                }
                reaped_ = true;
                break;
            }
            struct timespec pause {0, 20 * 1000 * 1000};
            ::nanosleep(&pause, nullptr);
        }
    }
    master_.reset();
}

} // namespace edgedesk
