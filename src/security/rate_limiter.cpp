#include "security/rate_limiter.hpp"

#include <algorithm>

namespace edgedesk {

RateLimiter::RateLimiter(double rate_per_second, double burst)
    : rate_per_ms_((rate_per_second > 0.0 ? rate_per_second : 1.0) / 1000.0),
      burst_(burst > 1.0 ? burst : 1.0) {}

bool RateLimiter::allow(std::string_view key, std::int64_t now_ms) {
    if (key.empty()) key = "-";
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buckets_.find(std::string(key));
    if (it == buckets_.end()) {
        if (buckets_.size() >= max_entries_) {
            // Evict the least recently used entry (bounded scan).
            auto oldest = buckets_.begin();
            for (auto scan = buckets_.begin(); scan != buckets_.end(); ++scan) {
                if (scan->second.last_ms < oldest->second.last_ms) oldest = scan;
            }
            buckets_.erase(oldest);
        }
        Bucket bucket;
        bucket.tokens = burst_;
        bucket.last_ms = now_ms;
        it = buckets_.emplace(std::string(key), bucket).first;
    }
    Bucket& bucket = it->second;
    const auto elapsed = std::max<std::int64_t>(0, now_ms - bucket.last_ms);
    bucket.last_ms = now_ms;
    bucket.tokens = std::min(burst_, bucket.tokens + static_cast<double>(elapsed) * rate_per_ms_);
    if (bucket.tokens < 1.0) return false;
    bucket.tokens -= 1.0;
    return true;
}

void RateLimiter::sweep(std::int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr std::int64_t kIdleMs = 10 * 60 * 1000;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (now_ms - it->second.last_ms > kIdleMs) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace edgedesk
