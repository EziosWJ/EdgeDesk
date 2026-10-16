#include "system/system_metrics.hpp"

#include "core/util.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>

namespace edgedesk {
namespace {

constexpr std::size_t kMaxProcFileSize = 512 * 1024;

bool read_small_file(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad() || data.size() > kMaxProcFileSize) return false;
    out = std::move(data);
    return true;
}

} // namespace

SystemMetrics::SystemMetrics(Options options)
    : options_(std::move(options)),
      history_capacity_(static_cast<std::size_t>(std::max(10, options_.history))) {
    if (options_.interval_ms < 200) options_.interval_ms = 200;
    if (options_.disk_refresh_cycles < 1) options_.disk_refresh_cycles = 1;
}

SystemMetrics::~SystemMetrics() {
    stop();
}

void SystemMetrics::start() {
    if (started_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        struct utsname info {};
        if (::uname(&info) == 0) {
            kernel_release_ = info.release;
            architecture_ = info.machine;
        }
        char name_buffer[256] = {0};
        if (::gethostname(name_buffer, sizeof(name_buffer) - 1) == 0) {
            hostname_ = name_buffer;
        }
    }
    started_ = true;
    sample_once(true);
    thread_ = std::thread(&SystemMetrics::run, this);
}

void SystemMetrics::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lock(wakeup_mutex_);
        stop_requested_ = true;
    }
    wakeup_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

MetricsSample SystemMetrics::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.empty()) return MetricsSample{};
    return history_.back();
}

std::vector<MetricsSample> SystemMetrics::history_samples(std::size_t max_points) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.empty()) return {};
    const std::size_t take = std::min(max_points, history_.size());
    return std::vector<MetricsSample>(history_.end() - static_cast<std::ptrdiff_t>(take), history_.end());
}

std::uint64_t SystemMetrics::subscribe(SampleCallback callback) {
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscribers_.size() >= kMaxSubscribers) return 0;
    const std::uint64_t id = next_subscriber_id_++;
    subscribers_.emplace(id, std::move(callback));
    return id;
}

void SystemMetrics::unsubscribe(std::uint64_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(id);
}

MetricsSample SystemMetrics::sample_once(bool refresh_disks) {
    MetricsSample sample;
    sample.timestamp_ms = now_unix_millis();
    sample.cpu_cores = static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN));
    sample.hostname = hostname_;
    sample.kernel_release = kernel_release_;
    sample.architecture = architecture_;

    std::string content;
    if (read_small_file("/proc/stat", content)) {
        CpuCounters counters;
        if (parse_proc_stat_cpu(content, counters)) {
            if (has_previous_cpu_) {
                sample.cpu_usage_percent = cpu_usage_percent(previous_cpu_, counters);
            }
            previous_cpu_ = counters;
            has_previous_cpu_ = true;
        }
    }
    if (!sample.cpu_usage_percent.has_value()) {
        sample.unavailable.push_back("cpu_usage");
    }

    content.clear();
    if (read_small_file("/proc/loadavg", content)) {
        parse_loadavg(content, sample.load);
    }
    if (!sample.load.valid) sample.unavailable.push_back("loadavg");

    content.clear();
    if (read_small_file("/proc/meminfo", content)) {
        parse_meminfo(content, sample.memory);
    }
    if (!sample.memory.valid) sample.unavailable.push_back("memory");

    content.clear();
    if (read_small_file("/proc/uptime", content)) {
        parse_uptime(content, sample.uptime_seconds);
    } else {
        sample.unavailable.push_back("uptime");
    }

    content.clear();
    if (read_small_file("/proc/net/dev", content)) {
        std::vector<NetInterfaceCounters> counters;
        parse_proc_net_dev(content, counters);
        if (previous_net_.empty() || previous_net_timestamp_ms_ == 0) {
            sample.unavailable.push_back("network_rate_initial");
        } else {
            const double elapsed = static_cast<double>(sample.timestamp_ms - previous_net_timestamp_ms_) / 1000.0;
            sample.network = compute_network_rates(previous_net_, counters, elapsed);
        }
        previous_net_ = counters;
        previous_net_timestamp_ms_ = sample.timestamp_ms;
        if (counters.empty()) sample.unavailable.push_back("network_interfaces");
    } else {
        sample.unavailable.push_back("network");
    }

    if (refresh_disks && !options_.disk_mounts.empty()) {
        cached_disks_.clear();
        for (const auto& mount : options_.disk_mounts) {
            DiskUsage usage;
            if (read_mount_capacity(mount, usage)) {
                cached_disks_.push_back(std::move(usage));
            }
        }
    } else if (refresh_disks) {
        content.clear();
        if (read_small_file("/proc/mounts", content)) {
            auto mounts = parse_proc_mounts(content);
            std::vector<DiskUsage> refreshed;
            refreshed.reserve(mounts.size());
            for (auto& usage : mounts) {
                if (read_mount_capacity(usage.mount, usage)) {
                    refreshed.push_back(std::move(usage));
                }
            }
            cached_disks_ = std::move(refreshed);
        }
    }
    if (cached_disks_.empty()) {
        sample.unavailable.push_back("disk");
    }
    sample.disks = cached_disks_;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(sample);
        if (history_.size() > history_capacity_) {
            history_.erase(history_.begin(),
                           history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - history_capacity_));
        }
    }

    std::vector<SampleCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks.reserve(subscribers_.size());
        for (const auto& item : subscribers_) callbacks.push_back(item.second);
    }
    for (const auto& callback : callbacks) {
        callback(sample);
    }
    return sample;
}

void SystemMetrics::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(wakeup_mutex_);
            if (wakeup_.wait_for(lock, std::chrono::milliseconds(options_.interval_ms),
                                 [this] { return stop_requested_; })) {
                return;
            }
        }
        ++cycle_;
        const bool refresh_disks = cycle_ % options_.disk_refresh_cycles == 0;
        sample_once(refresh_disks);
    }
}

} // namespace edgedesk
