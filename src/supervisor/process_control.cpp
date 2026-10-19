#include "supervisor/process_control.hpp"

#include "core/types.hpp"
#include "core/util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <limits>
#include <map>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace edgedesk::supervisor {
namespace {

bool parse_u64_field(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        if (value > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(ch - '0')) / 10) {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    out = value;
    return true;
}

bool parse_i64_field(const std::string& text, std::int64_t& out) {
    if (text.empty()) return false;
    bool negative = false;
    std::size_t start = 0;
    if (text[0] == '-') {
        negative = true;
        start = 1;
    }
    std::uint64_t value = 0;
    if (!parse_u64_field(text.substr(start), value)) return false;
    out = negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    return true;
}

bool is_numeric_name(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

} // namespace

std::string read_boot_id() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    if (!input) return {};
    std::string value;
    std::getline(input, value);
    value = trim(value);
    return value;
}

bool read_proc_stat(int pid, ProcStat& out) {
    out = {};
    if (pid <= 0) return false;
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    if (!input) return false;
    std::string line;
    std::getline(input, line);
    // The comm field may contain spaces and parentheses; parse after the last ')'.
    const auto close = line.rfind(')');
    if (close == std::string::npos || close + 2 >= line.size()) return false;
    std::istringstream fields(line.substr(close + 2));
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token && tokens.size() < 22) tokens.push_back(token);
    if (tokens.size() < 22) return false;
    out.pid = pid;
    out.state = tokens[0].empty() ? '?' : tokens[0][0];
    std::int64_t field = 0;
    if (!parse_i64_field(tokens[2], field)) return false;
    out.pgrp = static_cast<int>(field);
    if (!parse_i64_field(tokens[3], field)) return false;
    out.session = static_cast<int>(field);
    if (!parse_u64_field(tokens[11], out.utime_ticks)) return false;
    if (!parse_u64_field(tokens[12], out.stime_ticks)) return false;
    if (!parse_u64_field(tokens[19], out.start_ticks)) return false;
    if (!parse_i64_field(tokens[21], out.rss_pages)) return false;
    if (parse_i64_field(tokens[1], field)) out.ppid = static_cast<int>(field);
    out.valid = true;
    return true;
}

bool process_identity_matches(int pid, std::string_view boot_id, std::uint64_t start_ticks) {
    if (boot_id.empty() || start_ticks == 0) return false;
    if (read_boot_id() != boot_id) return false;
    ProcStat stat;
    return read_proc_stat(pid, stat) && stat.start_ticks == start_ticks;
}

bool process_group_has_members(int pgid, std::uint64_t minimum_start_ticks) {
    if (pgid <= 1) return false;
    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) return false;
    bool found = false;
    while (dirent* entry = ::readdir(dir)) {
        if (!is_numeric_name(entry->d_name)) continue;
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) continue;
        ProcStat stat;
        if (!read_proc_stat(pid, stat)) continue;
        if (stat.pgrp == pgid && stat.start_ticks >= minimum_start_ticks) {
            found = true;
            break;
        }
    }
    ::closedir(dir);
    return found;
}

long clock_ticks_per_second() {
    static const long value = ::sysconf(_SC_CLK_TCK);
    return value > 0 ? value : 100;
}

std::int64_t page_size_bytes() {
    static const std::int64_t value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? value : 4096;
}

SpawnOutcome spawn_service(const ServiceConfig& config, const ResolvedIdentity& identity) {
    SpawnOutcome outcome;
    if (::geteuid() != 0) {
        if (identity.uid != ::geteuid() || identity.gid != ::getegid()) {
            outcome.error = Error::make(ErrorCode::permission_denied,
                                        "supervisor must run as root to switch run_user/run_group");
            return outcome;
        }
    }

    const int exec_fd = ::open(config.executable.c_str(), O_PATH | O_CLOEXEC);
    if (exec_fd < 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "cannot open executable " + config.executable + ": " +
                                        std::strerror(errno), errno);
        return outcome;
    }
    const int cwd_fd = ::open(config.cwd.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (cwd_fd < 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "cannot open working directory " + config.cwd + ": " +
                                        std::strerror(errno), errno);
        ::close(exec_fd);
        return outcome;
    }
    struct stat exec_stat {};
    if (::fstat(exec_fd, &exec_stat) != 0 || !S_ISREG(exec_stat.st_mode) ||
        (exec_stat.st_mode & 0111) == 0) {
        outcome.error = Error::make(ErrorCode::invalid_argument,
                                    "executable is not a regular executable file: " + config.executable);
        ::close(cwd_fd);
        ::close(exec_fd);
        return outcome;
    }

    int log_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe2(log_pipe, 0) != 0) {
        outcome.error = Error::make(ErrorCode::io_error, std::string("pipe2: ") + std::strerror(errno), errno);
        ::close(cwd_fd);
        ::close(exec_fd);
        return outcome;
    }
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
        outcome.error = Error::make(ErrorCode::io_error, std::string("pipe2: ") + std::strerror(errno), errno);
        ::close(log_pipe[0]);
        ::close(log_pipe[1]);
        ::close(cwd_fd);
        ::close(exec_fd);
        return outcome;
    }

    // Build argv/envp before fork; the child never allocates.
    std::vector<std::string> argv_storage;
    argv_storage.push_back(config.executable);
    for (const auto& arg : config.args) argv_storage.push_back(arg);
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& arg : argv_storage) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    std::map<std::string, std::string> environment;
    environment["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    environment["HOME"] = identity.home;
    const std::string user_name = identity.user_name.empty() ? config.run_user : identity.user_name;
    environment["USER"] = user_name;
    environment["LOGNAME"] = user_name;
    for (const auto& item : config.env) environment[item.first] = item.second;
    std::vector<std::string> env_storage;
    env_storage.reserve(environment.size());
    for (const auto& item : environment) env_storage.push_back(item.first + "=" + item.second);
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto& item : env_storage) envp.push_back(const_cast<char*>(item.c_str()));
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        outcome.error = Error::make(ErrorCode::io_error, std::string("fork: ") + std::strerror(errno), errno);
        ::close(log_pipe[0]);
        ::close(log_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        ::close(cwd_fd);
        ::close(exec_fd);
        return outcome;
    }

    if (pid == 0) {
        // Child: only syscalls, no allocation or C++ objects.
        ::close(log_pipe[0]);
        ::close(err_pipe[0]);
        if (::setsid() < 0) {
            const int value = errno;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }
        if (::dup2(log_pipe[1], STDOUT_FILENO) < 0 || ::dup2(log_pipe[1], STDERR_FILENO) < 0) {
            const int value = errno;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }
        if (log_pipe[1] > STDERR_FILENO) ::close(log_pipe[1]);
        if (::fchdir(cwd_fd) != 0) {
            const int value = errno;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }
        ::close(cwd_fd);
        ::umask(0022);
        sigset_t empty;
        ::sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig == SIGKILL || sig == SIGSTOP) continue;
            ::signal(sig, SIG_DFL);
        }
        if (::geteuid() == 0) {
            if (::setgroups(identity.supplementary_groups.size(), identity.supplementary_groups.data()) != 0 ||
                ::setgid(identity.gid) != 0 || ::setuid(identity.uid) != 0) {
                const int value = errno;
                (void)!::write(err_pipe[1], &value, sizeof(value));
                ::_exit(127);
            }
        } else if (::getuid() != identity.uid || ::getgid() != identity.gid ||
                   ::geteuid() != identity.uid || ::getegid() != identity.gid) {
            const int value = EPERM;
            (void)!::write(err_pipe[1], &value, sizeof(value));
            ::_exit(127);
        }
        ::fexecve(exec_fd, argv.data(), envp.data());
        const int value = errno;
        (void)!::write(err_pipe[1], &value, sizeof(value));
        ::_exit(127);
    }

    // Parent.
    ::close(log_pipe[1]);
    ::close(err_pipe[1]);
    ::close(cwd_fd);
    ::close(exec_fd);
    const int flags = ::fcntl(log_pipe[0], F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(log_pipe[0], F_SETFL, flags | O_NONBLOCK);
    (void)::fcntl(log_pipe[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);

    ProcStat stat;
    std::uint64_t start_ticks = 0;
    // The child may not have been scheduled yet; retry briefly. A missing stat
    // is recorded as zero, in which case recovery will treat identity as
    // unverifiable rather than signaling a reused PID.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (read_proc_stat(pid, stat)) {
            start_ticks = stat.start_ticks;
            break;
        }
        ::usleep(1000);
    }

    outcome.pid = pid;
    outcome.pgid = pid;
    outcome.start_ticks = start_ticks;
    outcome.log_read_fd = log_pipe[0];
    outcome.exec_read_fd = err_pipe[0];
    return outcome;
}

bool signal_service_group(int pgid, std::uint64_t minimum_start_ticks, int signal,
                          std::string& error) {
    if (pgid <= 1 || signal <= 0) {
        error = "invalid process group signal request";
        return false;
    }
    if (!process_group_has_members(pgid, minimum_start_ticks)) {
        error = "process group no longer exists";
        return false;
    }
    if (::kill(-pgid, signal) != 0) {
        error = std::string("kill(-") + std::to_string(pgid) + "): " + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace edgedesk::supervisor
