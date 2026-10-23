#include "test_util.hpp"

#include "core/util.hpp"
#include "supervisor/process_control.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace edgedesk;
using namespace edgedesk::supervisor;

namespace {

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

// PSH-01: a live setsid() leader with the recorded start time is verified; the
// same numeric group with a different start time, boot ID or an unreadable
// identity is never signalled.
void test_verified_absent_and_uncertain() {
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

// PSH-02: after the leader exits, the session anchor still proves that a
// surviving descendant belongs to the original service.
void test_descendant_group_identity() {
    const std::string boot = read_boot_id();
    CHECK(!boot.empty());
    if (boot.empty()) return;

    int fds[2];
    CHECK(::pipe(fds) == 0);
    const pid_t leader = ::fork();
    CHECK(leader >= 0);
    if (leader < 0) return;
    if (leader == 0) {
        ::close(fds[0]);
        if (::setsid() < 0) ::_exit(2);
        ProcStat self;
        if (!read_proc_stat(::getpid(), self)) ::_exit(3);
        const pid_t grand = ::fork();
        if (grand == 0) {
            ::close(fds[1]);
            for (;;) ::pause();
        }
        if (grand < 0) ::_exit(4);
        const std::string line = std::to_string(static_cast<long>(grand)) + " " +
                                 std::to_string(self.start_ticks);
        (void)::write(fds[1], line.data(), line.size());
        ::close(fds[1]);
        ::_exit(0);
    }
    Reaper reaper;
    reaper.pid = leader;
    ::close(fds[1]);

    char buffer[64] = {0};
    const ssize_t count = ::read(fds[0], buffer, sizeof(buffer) - 1);
    ::close(fds[0]);
    CHECK(count > 0);
    if (count <= 0) return;

    const pid_t grand = static_cast<pid_t>(std::strtol(buffer, nullptr, 10));
    const char* space = std::strchr(buffer, ' ');
    CHECK(space != nullptr);
    const std::uint64_t start_ticks =
        space != nullptr ? std::strtoull(space + 1, nullptr, 10) : 0;
    CHECK(grand > 1);
    CHECK(start_ticks > 0);

    int status = 0;
    while (::waitpid(leader, &status, 0) < 0 && errno == EINTR) {
    }
    reaper.pid = -1;

    ProcStat grand_stat;
    CHECK(wait_stat(grand, grand_stat));

    GroupProbe probe = probe_service_group(leader, leader, start_ticks, boot, boot);
    CHECK(probe.state == GroupState::verified);
    CHECK(probe.live_members >= 1);
    std::string error;
    CHECK(signal_service_group(probe, SIGTERM, error));
    CHECK(wait_group_absent(leader));
    probe = probe_service_group(leader, leader, start_ticks, boot, boot);
    CHECK(probe.state == GroupState::absent);
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
    test_verified_absent_and_uncertain();
    test_descendant_group_identity();
    test_native_app_program_ownership_gate();
    return edgedesk::test::finish("process_control_test");
}
