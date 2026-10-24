#include "test_util.hpp"

#include "core/util.hpp"
#include "supervisor/process_control.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace edgedesk;
using namespace edgedesk::supervisor;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kAnchorStackSize = 64 * 1024;

struct Reaper {
    pid_t pid = -1;
    ~Reaper() {
        if (pid <= 1) return;
        (void)::kill(-pid, SIGKILL);
        (void)::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
};

bool wait_stat(pid_t pid, ProcStat& out, int timeout_ms = 2000) {
    for (int attempt = 0; attempt < timeout_ms; ++attempt) {
        if (read_proc_stat(pid, out)) return true;
        ::usleep(1000);
    }
    return false;
}

bool wait_group_absent(int pgid, int timeout_ms = 3000) {
    for (int attempt = 0; attempt < timeout_ms; ++attempt) {
        if (!process_group_has_any_members(pgid)) return true;
        ::usleep(1000);
    }
    return false;
}

std::string current_user() {
    struct passwd* entry = ::getpwuid(::geteuid());
    if (entry != nullptr && entry->pw_name != nullptr) return entry->pw_name;
    return std::to_string(::geteuid());
}

std::string current_group() {
    struct group* entry = ::getgrgid(::getegid());
    if (entry != nullptr && entry->gr_name != nullptr) return entry->gr_name;
    return std::to_string(::getegid());
}

std::string drain_pipe(int fd, int timeout_ms = 3000) {
    std::string out;
    const auto deadline = ::time(nullptr) + timeout_ms / 1000 + 1;
    for (;;) {
        struct pollfd wait {};
        wait.fd = fd;
        wait.events = POLLIN;
        const int ready = ::poll(&wait, 1, 50);
        if (ready > 0) {
            char buffer[4096];
            const ssize_t count = ::read(fd, buffer, sizeof(buffer));
            if (count > 0) {
                out.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) return out;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return out;
        }
        if (ready == 0 && ::time(nullptr) >= deadline) return out;
    }
}

std::uint64_t open_until_absent(int& fd) {
    std::uint64_t sum = 0;
    if (fd >= 0) {
        sum += 1;
        ::close(fd);
        fd = -1;
    }
    return sum;
}

// PSH-01 (legacy boundary): a live setsid() leader with the recorded start
// time is verified; the same numeric group with a different start time, boot ID
// or an unreadable identity is never signalled.
void test_legacy_leader_identity() {
    const std::string boot = read_boot_id();
    CHECK(!boot.empty());
    if (boot.empty()) return;

    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        if (::setsid() < 0) ::_exit(2);
        for (;;) ::pause();
    }
    Reaper reaper;
    reaper.pid = pid;

    ProcStat stat;
    CHECK(wait_stat(pid, stat));
    if (!stat.valid) return;

    GroupProbe probe = probe_service_group(pid, pid, stat.start_ticks, boot, boot);
    CHECK(probe.state == GroupState::verified);
    CHECK_EQ(probe.live_members, 1);

    // Reused leader identity: the PID is alive but its kernel start time is
    // different from the recorded one.
    probe = probe_service_group(pid, pid, stat.start_ticks + 1, boot, boot);
    CHECK(probe.state == GroupState::uncertain);
    CHECK(!probe.detail.empty());
    std::string error;
    CHECK(!signal_service_group(probe, SIGTERM, error));
    CHECK(!error.empty());
    // The refusal must not have terminated the process.
    CHECK(::kill(pid, 0) == 0);

    // Different boot cycle: persisted identity is stale.
    probe = probe_service_group(pid, pid, stat.start_ticks, boot, "another-boot");
    CHECK(probe.state == GroupState::uncertain);

    // Missing recorded identity must fail closed instead of falling back to a
    // numeric-PGID-only check.
    probe = probe_service_group(pid, pid, 0, boot, boot);
    CHECK(probe.state == GroupState::uncertain);

    probe = probe_service_group(pid, pid, stat.start_ticks, boot, boot);
    CHECK(probe.state == GroupState::verified);
    CHECK(signal_service_group(probe, SIGTERM, error));
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    reaper.pid = -1;

    probe = probe_service_group(pid, pid, stat.start_ticks, boot, boot);
    CHECK(probe.state == GroupState::absent);
    CHECK(!signal_service_group(probe, SIGTERM, error));
}

// PSH-01/PSH-02: with a live identity anchor, descendants left behind by the
// leader are verified. Once only the anchor is alive the probe reports
// anchor_only; SIGTERM is refused and SIGKILL reaps it.
void test_anchor_descendant_identity() {
    const std::string boot = read_boot_id();
    CHECK(!boot.empty());
    if (boot.empty()) return;

    int fds[2];
    CHECK(::pipe(fds) == 0);
    const pid_t leader = ::fork();
    CHECK(leader >= 0);
    if (leader < 0) return;
    if (leader == 0) {
        static unsigned char stack[kAnchorStackSize];
        struct Child {
            static int run(void* argument) {
                (void)argument;
                sigset_t blocked;
                ::sigfillset(&blocked);
                ::sigprocmask(SIG_SETMASK, &blocked, nullptr);
                for (;;) ::pause();
                return 0;
            }
        };
        ::close(fds[0]);
        if (::setsid() < 0) ::_exit(2);
        const pid_t anchor = ::clone(Child::run, stack + sizeof(stack),
                                     CLONE_PARENT | SIGCHLD, nullptr);
        const pid_t descendant = ::fork();
        if (descendant == 0) {
            for (;;) ::pause();
        }
        ProcStat self;
        ProcStat anchor_stat;
        ProcStat descendant_stat;
        if (anchor < 0 || descendant < 0 || !read_proc_stat(::getpid(), self) ||
            !read_proc_stat(anchor, anchor_stat) ||
            !read_proc_stat(descendant, descendant_stat)) {
            ::_exit(3);
        }
        const std::string line =
            std::to_string(static_cast<long>(anchor)) + " " +
            std::to_string(anchor_stat.start_ticks) + " " +
            std::to_string(static_cast<long>(descendant)) + " " +
            std::to_string(descendant_stat.start_ticks) + " " +
            std::to_string(self.start_ticks);
        (void)::write(fds[1], line.data(), line.size());
        ::close(fds[1]);
        ::_exit(0);
    }
    Reaper reaper;
    reaper.pid = leader;
    ::close(fds[1]);

    char buffer[128] = {0};
    const ssize_t count = ::read(fds[0], buffer, sizeof(buffer) - 1);
    ::close(fds[0]);
    CHECK(count > 0);
    if (count <= 0) return;

    char* cursor = buffer;
    const pid_t anchor = static_cast<pid_t>(std::strtol(cursor, &cursor, 10));
    const std::uint64_t anchor_start = std::strtoull(cursor, &cursor, 10);
    const pid_t descendant = static_cast<pid_t>(std::strtol(cursor, &cursor, 10));
    const std::uint64_t descendant_start = std::strtoull(cursor, &cursor, 10);
    const std::uint64_t leader_start = std::strtoull(cursor, &cursor, 10);
    (void)descendant_start;
    CHECK(anchor > 1);
    CHECK(descendant > 1);
    CHECK(anchor_start > 0);
    CHECK(leader_start > 0);

    int status = 0;
    while (::waitpid(leader, &status, 0) < 0 && errno == EINTR) {
    }
    reaper.pid = -1;

    GroupProbe probe = probe_service_group(leader, leader, leader_start, boot, boot,
                                           anchor, anchor_start);
    CHECK(probe.state == GroupState::verified);
    CHECK_EQ(probe.live_members, 1);
    CHECK(probe.anchor_alive);

    // Wrong anchor start time proves the anchor PID was reused.
    probe = probe_service_group(leader, leader, leader_start, boot, boot, anchor,
                                anchor_start + 1);
    CHECK(probe.state == GroupState::uncertain);

    probe = probe_service_group(leader, leader, leader_start, boot, boot, anchor,
                                anchor_start);
    std::string error;
    CHECK(signal_service_group(probe, SIGTERM, error));
    for (int attempt = 0; attempt < 2000; ++attempt) {
        if (::kill(descendant, 0) != 0) break;
        ::usleep(1000);
    }
    CHECK_EQ(::kill(descendant, 0), -1);

    probe = probe_service_group(leader, leader, leader_start, boot, boot, anchor,
                                anchor_start);
    CHECK(probe.state == GroupState::anchor_only);
    CHECK(probe.anchor_alive);
    CHECK(!signal_service_group(probe, SIGTERM, error));
    CHECK(signal_service_group(probe, SIGKILL, error));
    CHECK(wait_group_absent(leader));
    probe = probe_service_group(leader, leader, leader_start, boot, boot, anchor,
                                anchor_start);
    CHECK(probe.state == GroupState::absent);
}

// Spawns through the real path and verifies that the sentinel is created in the
// service session, is excluded from service membership, and can only be reaped
// with SIGKILL.
void test_spawn_anchor_lifecycle() {
    ResolvedIdentity identity;
    const Error resolved = resolve_identity(current_user(), current_group(), identity);
    CHECK(resolved.ok());
    if (!resolved.ok()) return;

    ServiceConfig config;
    config.name = "anchor-test";
    config.executable = "/bin/sleep";
    config.args = {"30"};
    config.cwd = "/";
    config.run_user = current_user();
    config.run_group = current_group();

    const fs::path staging_root = fs::temp_directory_path() /
                                  ("edgedesk-staging-" + std::to_string(::getpid()));
    std::error_code ignored;
    fs::remove_all(staging_root, ignored);
    fs::create_directories(staging_root);
    CHECK(::chmod(staging_root.c_str(), 0700) == 0);

    SpawnOutcome outcome = spawn_service(config, identity, staging_root.string());
    CHECK(outcome.error.ok());
    if (!outcome.error.ok()) return;
    CHECK(outcome.pid > 1);
    CHECK(outcome.anchor_pid > 1);
    CHECK(outcome.anchor_start_ticks > 0);
    CHECK(!outcome.exec_staging_link.empty());
    CHECK(fs::is_symlink(outcome.exec_staging_link));
    CHECK(fs::path(outcome.exec_staging_link).filename() == "sleep");
    CHECK(fs::read_symlink(outcome.exec_staging_link).string().rfind("/proc/self/fd/", 0) == 0);

    const std::string boot = read_boot_id();
    GroupProbe probe = probe_service_group(outcome.pgid, outcome.pid, outcome.start_ticks,
                                           boot, boot, outcome.anchor_pid,
                                           outcome.anchor_start_ticks);
    CHECK(probe.state == GroupState::verified);
    CHECK_EQ(probe.live_members, 1);
    CHECK(probe.anchor_alive);

    // The sentinel must not retain the Supervisor's sockets, database or log
    // descriptors after it is released.
    {
        const std::string fd_dir = "/proc/" + std::to_string(outcome.anchor_pid) + "/fd";
        int fds = 0;
        if (DIR* dir = ::opendir(fd_dir.c_str())) {
            while (dirent* entry = ::readdir(dir)) {
                if (entry->d_name[0] != '.') ++fds;
            }
            ::closedir(dir);
        }
        // Before the exec commit the sentinel keeps exactly the commit pipe.
        CHECK_EQ(fds, 1);
        std::ifstream comm("/proc/" + std::to_string(outcome.anchor_pid) + "/comm");
        std::string name;
        std::getline(comm, name);
        CHECK_EQ(name, std::string("edgedesk-anchor"));
    }

    // Anchor identity mismatch must fail closed.
    GroupProbe bad = probe_service_group(outcome.pgid, outcome.pid, outcome.start_ticks,
                                         boot, boot, outcome.anchor_pid,
                                         outcome.anchor_start_ticks + 1);
    CHECK(bad.state == GroupState::uncertain);

    std::string release_error;
    CHECK(release_service_child(outcome.handshake_fd, release_error));
    CHECK(outcome.handshake_fd == -1);

    // Give the exec a moment, then terminate the workload. The anchor blocks
    // SIGTERM, so only the service process exits.
    ProcStat stat;
    CHECK(wait_stat(outcome.pid, stat));
    {
        // After the commit the sentinel closes the pipe and keeps no FD at all.
        const std::string fd_dir = "/proc/" + std::to_string(outcome.anchor_pid) + "/fd";
        int fds = 1;
        for (int attempt = 0; attempt < 500 && fds != 0; ++attempt) {
            fds = 0;
            if (DIR* dir = ::opendir(fd_dir.c_str())) {
                while (dirent* entry = ::readdir(dir)) {
                    if (entry->d_name[0] != '.') ++fds;
                }
                ::closedir(dir);
            }
            if (fds != 0) ::usleep(1000);
        }
        CHECK_EQ(fds, 0);
    }
    CHECK(signal_service_group(probe, SIGTERM, release_error));
    int status = 0;
    while (::waitpid(outcome.pid, &status, 0) < 0 && errno == EINTR) {
    }

    for (int attempt = 0; attempt < 2000; ++attempt) {
        probe = probe_service_group(outcome.pgid, 0, outcome.start_ticks, boot, boot,
                                    outcome.anchor_pid, outcome.anchor_start_ticks);
        if (probe.state == GroupState::anchor_only) break;
        ::usleep(1000);
    }
    CHECK(probe.state == GroupState::anchor_only);
    CHECK(!signal_service_group(probe, SIGTERM, release_error));
    CHECK(signal_service_group(probe, SIGKILL, release_error));
    while (::waitpid(outcome.anchor_pid, &status, 0) < 0 && errno == EINTR) {
    }
    CHECK(wait_group_absent(outcome.pgid));
    probe = probe_service_group(outcome.pgid, 0, outcome.start_ticks, boot, boot,
                                outcome.anchor_pid, outcome.anchor_start_ticks);
    CHECK(probe.state == GroupState::absent);
    open_until_absent(outcome.log_read_fd);
    open_until_absent(outcome.exec_read_fd);
    fs::remove_all(staging_root, ignored);
}
// PSH-07: the descriptor that is inspected is the descriptor that is executed,
// argv stays an array, and malformed entry points fail with the real cause.
void test_script_inode_and_argv() {
    const fs::path root = fs::temp_directory_path() /
                          ("edgedesk-process-script-" + std::to_string(::getpid()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);

    ResolvedIdentity identity;
    CHECK(resolve_identity(current_user(), current_group(), identity).ok());

    const fs::path script = root / "argv.sh";
    {
        std::ofstream out(script);
        out << "#!/bin/sh\nprintf '<%s>\\n' \"$@\"\n";
    }
    CHECK(::chmod(script.c_str(), 0755) == 0);

    // A symlink resolves once to the same inode; arguments are passed literally
    // and never interpreted by a shell.
    const fs::path link = root / "argv-link.sh";
    fs::create_symlink(script, link, ignored);
    CHECK(fs::is_symlink(link));

    ServiceConfig config;
    config.name = "script-test";
    config.executable = link.string();
    config.args = {"one two", "; echo hacked", "$HOME", "`id`"};
    config.cwd = root.string();
    config.run_user = current_user();
    config.run_group = current_group();

    SpawnOutcome outcome = spawn_service(config, identity);
    CHECK(outcome.error.ok());
    if (!outcome.error.ok()) {
        fs::remove_all(root, ignored);
        return;
    }
    CHECK(outcome.script_entry);
    std::string release_error;
    CHECK(release_service_child(outcome.handshake_fd, release_error));
    int status = 0;
    while (::waitpid(outcome.pid, &status, 0) < 0 && errno == EINTR) {
    }
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
    const std::string output = drain_pipe(outcome.log_read_fd);
    CHECK(output.find("<one two>") != std::string::npos);
    CHECK(output.find("<; echo hacked>") != std::string::npos);
    CHECK(output.find("<$HOME>") != std::string::npos);
    CHECK(output.find("<`id`>") != std::string::npos);
    open_until_absent(outcome.log_read_fd);
    open_until_absent(outcome.exec_read_fd);
    // The anchor only exits on SIGKILL (the exec commit succeeded).
    (void)::kill(outcome.anchor_pid, SIGKILL);
    int anchor_status = 0;
    while (::waitpid(outcome.anchor_pid, &anchor_status, 0) < 0 && errno == EINTR) {
    }
    CHECK(wait_group_absent(outcome.pgid));

    // A shebang with one interpreter argument is supported.
    const fs::path with_arg = root / "with-arg.sh";
    {
        std::ofstream out(with_arg);
        out << "#!/bin/sh -e\nexit 0\n";
    }
    CHECK(::chmod(with_arg.c_str(), 0755) == 0);
    ServiceConfig arg_config = config;
    arg_config.executable = with_arg.string();
    arg_config.args = {};
    SpawnOutcome arg_outcome = spawn_service(arg_config, identity);
    CHECK(arg_outcome.error.ok());
    if (arg_outcome.error.ok()) {
        CHECK(arg_outcome.script_entry);
        CHECK(release_service_child(arg_outcome.handshake_fd, release_error));
        int arg_status = 0;
        while (::waitpid(arg_outcome.pid, &arg_status, 0) < 0 && errno == EINTR) {
        }
        CHECK(WIFEXITED(arg_status));
        CHECK_EQ(WEXITSTATUS(arg_status), 0);
        open_until_absent(arg_outcome.log_read_fd);
        open_until_absent(arg_outcome.exec_read_fd);
        (void)::kill(arg_outcome.anchor_pid, SIGKILL);
        while (::waitpid(arg_outcome.anchor_pid, &arg_status, 0) < 0 && errno == EINTR) {
        }
    }

    // Non-regular entry points are rejected before fork.
    const fs::path fifo = root / "fifo";
    CHECK(::mkfifo(fifo.c_str(), 0755) == 0);
    ServiceConfig fifo_config = config;
    fifo_config.executable = fifo.string();
    SpawnOutcome fifo_outcome = spawn_service(fifo_config, identity);
    CHECK(!fifo_outcome.error.ok());
    if (!fifo_outcome.error.ok()) {
        CHECK(fifo_outcome.error.message.find("regular executable") != std::string::npos);
    }

    // A shebang longer than the kernel limit must be reported as such.
    const fs::path long_header = root / "long.sh";
    {
        std::ofstream out(long_header);
        out << "#!" << std::string(300, 'x') << "\n";
    }
    CHECK(::chmod(long_header.c_str(), 0755) == 0);
    ServiceConfig long_config = config;
    long_config.executable = long_header.string();
    SpawnOutcome long_outcome = spawn_service(long_config, identity);
    CHECK(!long_outcome.error.ok());
    if (!long_outcome.error.ok()) {
        CHECK(long_outcome.error.message.find("too long") != std::string::npos);
    }

    // A missing interpreter is reported instead of a misleading ENOENT.
    const fs::path broken = root / "broken.sh";
    {
        std::ofstream out(broken);
        out << "#!/definitely/not/installed\n";
    }
    CHECK(::chmod(broken.c_str(), 0755) == 0);
    ServiceConfig broken_config = config;
    broken_config.executable = broken.string();
    SpawnOutcome broken_outcome = spawn_service(broken_config, identity);
    CHECK(!broken_outcome.error.ok());
    if (!broken_outcome.error.ok()) {
        CHECK(broken_outcome.error.message.find("interpreter not found") !=
              std::string::npos);
    }

    fs::remove_all(root, ignored);
}

} // namespace

// SPEC-0011: application-managed services are identified by the host-owned
// socket environment variable. Their program tree must not be writable by the
// backend identity; this covers Supervisor autostart/recovery paths that do not
// pass through the extension lifecycle.
void test_native_app_program_ownership_gate() {
    struct passwd* entry = ::getpwuid(::geteuid());
    if (entry == nullptr) return;
    struct group* group_entry = ::getgrgid(::getegid());
    ResolvedIdentity identity;
    identity.uid = ::geteuid();
    identity.gid = ::getegid();
    identity.user_name = entry->pw_name != nullptr ? entry->pw_name : "";
    identity.group_name = group_entry != nullptr && group_entry->gr_name != nullptr
                              ? group_entry->gr_name
                              : "";
    identity.home = entry->pw_dir != nullptr ? entry->pw_dir : "/";
    identity.supplementary_groups = {::getegid()};
    identity.is_root = ::geteuid() == 0;

    ServiceConfig config;
    config.name = "app-program-gate";
    config.executable = "/bin/sleep";
    config.args = {"5"};
    config.cwd = "/bin"; // host-owned, not writable by the unprivileged identity
    config.run_user = identity.user_name;
    config.run_group = identity.group_name;
    config.env["EDGEDESK_APP_SOCKET"] = "/tmp/edgedesk-gate.sock";

    SpawnOutcome allowed = spawn_service(config, identity);
    CHECK(allowed.error.ok());
    if (allowed.error.ok()) {
        std::string release_error;
        CHECK(release_service_child(allowed.handshake_fd, release_error));
        (void)::kill(-allowed.pgid, SIGKILL);
        int status = 0;
        while (::waitpid(allowed.pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (allowed.log_read_fd >= 0) ::close(allowed.log_read_fd);
        if (allowed.exec_read_fd >= 0) ::close(allowed.exec_read_fd);
    }

    if (::geteuid() == 0) {
        // Root is a host-level identity explicitly accepted by allow_root; DAC
        // cannot confine it, so the ownership gate is intentionally skipped.
        return;
    }

    char directory[] = "/tmp/edgedesk-pc-gate-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    ServiceConfig denied = config;
    denied.cwd = directory; // owned by the backend identity
    SpawnOutcome rejected = spawn_service(denied, identity);
    CHECK(!rejected.error.ok());
    CHECK(rejected.error.code == ErrorCode::permission_denied);
    CHECK(rejected.pid <= 0);
    (void)::rmdir(directory);
}

int main() {
    test_legacy_leader_identity();
    test_anchor_descendant_identity();
    test_spawn_anchor_lifecycle();
    test_script_inode_and_argv();
    test_native_app_program_ownership_gate();
    return edgedesk::test::finish("process_control_test");
}
