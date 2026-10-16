#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// Bounded, per-IP token buckets and a concurrency gate.
// Memory is capped: map entries are evicted when the cap is reached.

namespace edgedesk {

class RateLimiter {
public:
    RateLimiter(double rate_per_second, double burst);

    // Consumes one token for `key`. Returns false when the bucket is empty.
    bool allow(std::string_view key, std::int64_t now_ms);

    // Drops buckets that refilled long ago; bounded work per call.
    void sweep(std::int64_t now_ms);

private:
    struct Bucket {
        double tokens = 0.0;
        std::int64_t last_ms = 0;
    };

    const double rate_per_ms_;
    const double burst_;
    const std::size_t max_entries_ = 4096;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
};

// Simple bounded counting gate for streaming transfers / sessions.
class ConcurrencyGate {
public:
    explicit ConcurrencyGate(int limit) : limit_(limit < 1 ? 1 : limit) {}

    bool try_acquire() {
        int current = in_use_.load(std::memory_order_relaxed);
        while (current < limit_) {
            if (in_use_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    void release() { in_use_.fetch_sub(1, std::memory_order_acq_rel); }
    int in_use() const { return in_use_.load(std::memory_order_relaxed); }
    int limit() const { return limit_; }

private:
    const int limit_;
    std::atomic<int> in_use_{0};
};

} // namespace edgedesk
