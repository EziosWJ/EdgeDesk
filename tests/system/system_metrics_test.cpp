#include "test_util.hpp"

#include "system/cpu_metrics.hpp"
#include "system/disk_metrics.hpp"
#include "system/memory_metrics.hpp"
#include "system/network_metrics.hpp"
#include "system/system_metrics.hpp"

#include <atomic>
#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <netinet/in.h>
#include <thread>

using namespace edgedesk;

namespace {

void test_proc_stat() {
    const std::string content =
        "cpu  100 20 30 400 50 10 5 0 0 0\n"
        "cpu0 50 10 15 200 25 5 2 0 0 0\n";
    CpuCounters counters;
    CHECK(parse_proc_stat_cpu(content, counters));
    CHECK_EQ(counters.user, std::uint64_t{100});
    CHECK_EQ(counters.nice, std::uint64_t{20});
    CHECK_EQ(counters.system, std::uint64_t{30});
    CHECK_EQ(counters.idle, std::uint64_t{400});
    CHECK_EQ(counters.iowait, std::uint64_t{50});
    CHECK_EQ(counters.irq, std::uint64_t{10});
    CHECK_EQ(counters.softirq, std::uint64_t{5});
    CHECK_EQ(counters.total(), std::uint64_t{615});
    CHECK_EQ(counters.busy(), std::uint64_t{165});

    CHECK(!parse_proc_stat_cpu("cpu0 1 2 3 4\n", counters));
    CHECK(!parse_proc_stat_cpu("garbage", counters));

    CpuCounters later = counters;
    later.idle += 100;
    later.user += 100;
    const auto usage = cpu_usage_percent(counters, later);
    CHECK(usage.has_value());
    CHECK(*usage > 49.0 && *usage < 51.0);

    CpuCounters same = counters;
    CHECK(!cpu_usage_percent(counters, same).has_value());
}

void test_loadavg_uptime() {
    LoadAverage load;
    CHECK(parse_loadavg("0.52 0.34 0.28 1/234 5678\n", load));
    CHECK(load.load1 > 0.51 && load.load1 < 0.53);
    CHECK(load.load5 > 0.33 && load.load5 < 0.35);
    CHECK(load.load15 > 0.27 && load.load15 < 0.29);
    CHECK(!parse_loadavg("no numbers here", load));
    CHECK(!parse_loadavg("", load));

    double uptime = 0.0;
    CHECK(parse_uptime("12345.67 89012.34\n", uptime));
    CHECK(uptime > 12345.0);
    CHECK(!parse_uptime("oops", uptime));
}

void test_meminfo() {
    const std::string content =
        "MemTotal:       16384000 kB\n"
        "MemFree:         2048000 kB\n"
        "MemAvailable:    8192000 kB\n"
        "Buffers:          102400 kB\n"
        "Cached:          4096000 kB\n"
        "SwapTotal:       1024000 kB\n"
        "SwapFree:        1000000 kB\n";
    MemoryInfo info;
    CHECK(parse_meminfo(content, info));
    CHECK_EQ(info.total_kb, std::uint64_t{16384000});
    CHECK_EQ(info.available_kb, std::uint64_t{8192000});
    CHECK_EQ(info.used_kb(), std::uint64_t{8192000});
    CHECK_EQ(info.swap_free_kb, std::uint64_t{1000000});

    // Older kernels without MemAvailable fall back to free+buffers+cached.
    const std::string legacy =
        "MemTotal:       1000 kB\n"
        "MemFree:         100 kB\n"
        "Buffers:          50 kB\n"
        "Cached:          200 kB\n";
    MemoryInfo legacy_info;
    CHECK(parse_meminfo(legacy, legacy_info));
    CHECK_EQ(legacy_info.available_kb, std::uint64_t{350});

    MemoryInfo empty;
    CHECK(!parse_meminfo("nothing", empty));
}

void test_netdev() {
    const std::string content =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 1000       1    0    0    0     0          0         0     1000       1    0    0    0     0       0          0\n"
        "  eth0: 1234567     100    0    0    0     0          0         0  7654321     200    0    0    0     0       0          0\n";
    std::vector<NetInterfaceCounters> counters;
    CHECK(parse_proc_net_dev(content, counters));
    CHECK_EQ(counters.size(), std::size_t{1});
    CHECK_STR_EQ(counters.front().name, "eth0");
    CHECK_EQ(counters.front().rx_bytes, std::uint64_t{1234567});
    CHECK_EQ(counters.front().tx_bytes, std::uint64_t{7654321});
    CHECK_EQ(counters.front().rx_packets, std::uint64_t{100});

    auto previous = counters;
    auto current = counters;
    current[0].rx_bytes += 1000;
    current[0].tx_bytes += 2000;
    const auto rates = compute_network_rates(previous, current, 2.0);
    CHECK_EQ(rates.size(), std::size_t{1});
    CHECK(rates[0].rx_bytes_per_second > 499.0 && rates[0].rx_bytes_per_second < 501.0);
    CHECK(rates[0].tx_bytes_per_second > 999.0 && rates[0].tx_bytes_per_second < 1001.0);
    CHECK(compute_network_rates(previous, current, 0.0).empty());
}

void test_interface_addresses() {
    in_addr ipv4{};
    CHECK(::inet_pton(AF_INET, "192.0.2.10", &ipv4) == 1);
    CHECK_STR_EQ(format_ip_address(AF_INET, &ipv4), "192.0.2.10");

    in6_addr ipv6{};
    CHECK(::inet_pton(AF_INET6, "2001:db8::1", &ipv6) == 1);
    CHECK_STR_EQ(format_ip_address(AF_INET6, &ipv6), "2001:db8::1");

    CHECK(format_ip_address(AF_INET, nullptr).empty());
    CHECK(format_ip_address(-1, &ipv4).empty());

    const auto interfaces = read_interface_addresses();
    for (const auto& interface : interfaces) {
        CHECK(!interface.name.empty());
        CHECK(interface.name != "lo");
        CHECK(!interface.addresses.empty());
    }
}

void test_mounts() {
    const std::string content =
        "/dev/sda1 / ext4 rw,relatime 0 0\n"
        "proc /proc proc rw,nosuid 0 0\n"
        "sysfs /sys sysfs rw 0 0\n"
        "tmpfs /run tmpfs rw 0 0\n"
        "devtmpfs /dev devtmpfs rw 0 0\n"
        "/dev/sda2 /data\\040disk ext4 rw 0 0\n"
        "overlay /var/lib/docker/overlay2 overlay rw 0 0\n"
        "nfs.example:/export /mnt/nfs nfs4 rw 0 0\n";
    const auto mounts = parse_proc_mounts(content);
    CHECK_EQ(mounts.size(), std::size_t{2});
    CHECK_STR_EQ(mounts[0].mount, "/");
    CHECK_STR_EQ(mounts[1].mount, "/data disk");
    CHECK(is_pseudo_filesystem("proc"));
    CHECK(is_pseudo_filesystem("tmpfs"));
    CHECK(is_pseudo_filesystem("overlay"));
    CHECK(!is_pseudo_filesystem("ext4"));

    DiskUsage usage;
    CHECK(read_mount_capacity("/", usage));
    CHECK(usage.total_bytes > 0);
    CHECK(usage.used_bytes <= usage.total_bytes);
    CHECK(!read_mount_capacity("/definitely/not/a/mount", usage));
}

void test_sampler() {
    SystemMetrics::Options options;
    options.interval_ms = 200;
    options.history = 3;
    SystemMetrics metrics(options);

    std::mutex mutex;
    std::condition_variable wakeup;
    std::atomic<int> callbacks{0};
    const auto subscription = metrics.subscribe([&](const MetricsSample&) {
        callbacks.fetch_add(1);
        wakeup.notify_all();
    });
    CHECK(subscription != 0);

    metrics.start();
    {
        std::unique_lock<std::mutex> lock(mutex);
        wakeup.wait_for(lock, std::chrono::milliseconds(3000), [&] { return callbacks.load() >= 2; });
    }
    CHECK(callbacks.load() >= 1);
    const auto sample = metrics.latest();
    CHECK(sample.timestamp_ms > 0);
    CHECK(sample.cpu_cores >= 1);
    CHECK(sample.hostname.size() > 0);
    CHECK(sample.uptime_seconds > 0.0);
    CHECK(!sample.memory.valid || sample.memory.total_kb > 0);
    CHECK(!sample.disks.empty() || !sample.unavailable.empty());
    CHECK(sample.unavailable.size() < 8);

    CHECK(metrics.history_samples(100).size() <= 3);
    metrics.unsubscribe(subscription);
    metrics.stop();
}

} // namespace

int main() {
    test_proc_stat();
    test_loadavg_uptime();
    test_meminfo();
    test_netdev();
    test_interface_addresses();
    test_mounts();
    test_sampler();
    return edgedesk::test::finish("system_metrics_test");
}
