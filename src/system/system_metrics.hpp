#pragma once

#include "system/cpu_metrics.hpp"
#include "system/disk_metrics.hpp"
#include "system/memory_metrics.hpp"
#include "system/network_metrics.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Background sampler with a fixed-size in-memory ring buffer (ADR-0006).
// No database, no disk history; everything is lost on restart by design.

namespace edgedesk {

struct MetricsSample {
    std::int64_t timestamp_ms = 0;
    double uptime_seconds = 0.0;
    std::optional<double> cpu_usage_percent;
    LoadAverage load;
    int cpu_cores = 0;
    MemoryInfo memory;
    std::vector<DiskUsage> disks;
    std::vector<NetInterfaceRate> network;
    std::vector<std::string> unavailable;
    std::string hostname;
    std::string kernel_release;
    std::string architecture;
};

class SystemMetrics {
public:
    struct Options {
        int interval_ms = 1000;
        int history = 300;
        int disk_refresh_cycles = 10;
        std::vector<std::string> disk_mounts;
    };

    using SampleCallback = std::function<void(const MetricsSample&)>;

    explicit SystemMetrics(Options options);
    ~SystemMetrics();

    SystemMetrics(const SystemMetrics&) = delete;
    SystemMetrics& operator=(const SystemMetrics&) = delete;

    // Starts the sampler thread; performs one synchronous sample first so the
    // first API call already has data.
    void start();
    void stop();

    MetricsSample latest() const;

    // Newest-last window of at most `max_points` samples.
    std::vector<MetricsSample> history_samples(std::size_t max_points) const;

    // Called from the sampler thread; returns 0 when the subscriber cap is
    // reached. Callbacks must not block for long.
    std::uint64_t subscribe(SampleCallback callback);
    void unsubscribe(std::uint64_t id);

private:
    void run();
    MetricsSample sample_once(bool refresh_disks);

    Options options_;
    const std::size_t history_capacity_;
    mutable std::mutex mutex_;    std::vector<MetricsSample> history_;
    std::condition_variable wakeup_;
    std::mutex wakeup_mutex_;
    std::thread thread_;
    bool stop_requested_ = false;
    bool started_ = false;

    // sampler-thread state
    CpuCounters previous_cpu_;
    bool has_previous_cpu_ = false;
    std::vector<NetInterfaceCounters> previous_net_;
    std::int64_t previous_net_timestamp_ms_ = 0;
    std::vector<DiskUsage> cached_disks_;
    int cycle_ = 0;
    std::string hostname_;
    std::string kernel_release_;
    std::string architecture_;

    std::uint64_t next_subscriber_id_ = 1;
    std::map<std::uint64_t, SampleCallback> subscribers_;
    static constexpr std::size_t kMaxSubscribers = 16;
};

} // namespace edgedesk
