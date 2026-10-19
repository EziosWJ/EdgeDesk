#pragma once

#include "core/error.hpp"
#include "supervisor/supervisor_types.hpp"

#include <cstdint>
#include <string>

// Low-level Linux process inspection and spawning helpers.
//
// The Supervisor is single-threaded, so fork() never runs while another thread
// holds malloc/NSS locks. Identity and NSS resolution happen in the parent
// before fork; the child only performs async-signal-safe syscalls before
// fexecve().

namespace edgedesk::supervisor {

struct ProcStat {
    bool valid = false;
    int pid = 0;
    char state = '?';
    int ppid = 0;
    int pgrp = 0;
    int session = 0;
    std::uint64_t start_ticks = 0;
    std::uint64_t utime_ticks = 0;
    std::uint64_t stime_ticks = 0;
    std::int64_t rss_pages = 0;
};

// Reads /proc/sys/kernel/random/boot_id. Empty on failure.
std::string read_boot_id();

bool read_proc_stat(int pid, ProcStat& out);

// Same boot + same kernel start time means the PID still names the process we
// started. A mismatched start time means the PID was reused.
bool process_identity_matches(int pid, std::string_view boot_id, std::uint64_t start_ticks);

// True when at least one process still belongs to `pgid`. Linux keeps the
// numeric PID reserved while a process group has members, so a match cannot be
// a recycled, unrelated group. `minimum_start_ticks` filters members created
// before our service instance.
bool process_group_has_members(int pgid, std::uint64_t minimum_start_ticks);

long clock_ticks_per_second();
std::int64_t page_size_bytes();

struct SpawnOutcome {
    Error error;
    int pid = -1;
    int pgid = 0;
    std::uint64_t start_ticks = 0;
    int log_read_fd = -1;
    int exec_read_fd = -1;
};

// Starts `config` with `identity`. stdout/stderr share one non-blocking pipe;
// the exec/identity error channel is CLOEXEC and reports failure before exec.
// The caller owns and must close both returned descriptors.
SpawnOutcome spawn_service(const ServiceConfig& config, const ResolvedIdentity& identity);

// Sends a signal to the process group only after verifying that the group still
// exists. Never takes a client-supplied PID/PGID.
bool signal_service_group(int pgid, std::uint64_t minimum_start_ticks, int signal,
                          std::string& error);

} // namespace edgedesk::supervisor
