#include "http/routes/system_route.hpp"

#include "core/util.hpp"
#include "http/ws_state.hpp"

#include <algorithm>

namespace edgedesk {
namespace {

constexpr std::size_t kMaxWsMessage = 64 * 1024;
constexpr std::size_t kMaxWsBacklog = 1024 * 1024;

hv::Json disk_json(const DiskUsage& disk) {
    hv::Json json;
    json["mount"] = disk.mount;
    json["filesystem"] = disk.filesystem;
    json["total_bytes"] = disk.total_bytes;
    json["used_bytes"] = disk.used_bytes;
    json["available_bytes"] = disk.available_bytes;
    json["used_percent"] = disk.total_bytes > 0
                               ? (100.0 * static_cast<double>(disk.used_bytes) / static_cast<double>(disk.total_bytes))
                               : 0.0;
    return json;
}

hv::Json metrics_json(const MetricsSample& sample) {
    hv::Json json;
    json["ts"] = sample.timestamp_ms;
    json["uptime_seconds"] = sample.uptime_seconds;
    json["hostname"] = sample.hostname;
    json["kernel"] = sample.kernel_release;
    json["arch"] = sample.architecture;

    hv::Json cpu;
    cpu["cores"] = sample.cpu_cores;
    if (sample.cpu_usage_percent.has_value()) {
        cpu["usage_percent"] = *sample.cpu_usage_percent;
    } else {
        cpu["usage_percent"] = nullptr;
    }
    if (sample.load.valid) {
        cpu["load1"] = sample.load.load1;
        cpu["load5"] = sample.load.load5;
        cpu["load15"] = sample.load.load15;
    } else {
        cpu["load1"] = nullptr;
        cpu["load5"] = nullptr;
        cpu["load15"] = nullptr;
    }
    json["cpu"] = std::move(cpu);

    hv::Json memory;
    if (sample.memory.valid) {
        memory["total_kb"] = sample.memory.total_kb;
        memory["available_kb"] = sample.memory.available_kb;
        memory["used_kb"] = sample.memory.used_kb();
        memory["free_kb"] = sample.memory.free_kb;
        memory["buffers_kb"] = sample.memory.buffers_kb;
        memory["cached_kb"] = sample.memory.cached_kb;
        memory["swap_total_kb"] = sample.memory.swap_total_kb;
        memory["swap_free_kb"] = sample.memory.swap_free_kb;
        memory["used_percent"] = sample.memory.total_kb > 0
                                     ? (100.0 * static_cast<double>(sample.memory.used_kb()) /
                                        static_cast<double>(sample.memory.total_kb))
                                     : 0.0;
    } else {
        memory = nullptr;
    }
    json["memory"] = std::move(memory);

    hv::Json disks = hv::Json::array();
    for (const auto& disk : sample.disks) disks.push_back(disk_json(disk));
    json["disks"] = std::move(disks);

    hv::Json network = hv::Json::array();
    for (const auto& interface : sample.network) {
        hv::Json item;
        item["name"] = interface.name;
        item["addresses"] = interface.addresses;
        item["rx_bytes_per_second"] = interface.rx_bytes_per_second;
        item["tx_bytes_per_second"] = interface.tx_bytes_per_second;
        item["rx_bytes_total"] = interface.rx_bytes_total;
        item["tx_bytes_total"] = interface.tx_bytes_total;
        network.push_back(std::move(item));
    }
    json["network"] = std::move(network);
    json["unavailable"] = sample.unavailable;
    return json;
}

hv::Json history_json(const std::vector<MetricsSample>& samples) {
    hv::Json array = hv::Json::array();
    for (const auto& sample : samples) array.push_back(metrics_json(sample));
    return array;
}

} // namespace

void register_system_routes(hv::HttpService& router, RouteContext& ctx) {
    router.GET("/api/system/summary", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        const auto history_points = http::query_int(req, "history", 0, 0, 3600);
        hv::Json body;
        body["ok"] = true;
        body["interval_ms"] = ctx.config->metrics_interval_ms;
        body["sample"] = metrics_json(ctx.metrics->latest());
        if (history_points > 0) {
            body["history"] = history_json(ctx.metrics->history_samples(static_cast<std::size_t>(history_points)));
        }
        return http::write_json(resp, 200, body);
    });

    router.GET("/api/system/history", [&ctx](HttpRequest* req, HttpResponse* resp) -> int {
        const auto window = http::query_int(req, "window", 300, 1, 3600);
        hv::Json body;
        body["ok"] = true;
        body["interval_ms"] = ctx.config->metrics_interval_ms;
        body["samples"] = history_json(ctx.metrics->history_samples(static_cast<std::size_t>(window)));
        return http::write_json(resp, 200, body);
    });

    router.GET("/api/system/info", [&ctx](HttpRequest*, HttpResponse* resp) -> int {
        const auto sample = ctx.metrics->latest();
        hv::Json body;
        body["ok"] = true;
        body["version"] = EDGEDESK_VERSION;
        body["build"] = EDGEDESK_BUILD_TYPE;
        body["hostname"] = sample.hostname;
        body["arch"] = sample.architecture;
        body["kernel"] = sample.kernel_release;
        body["uptime_seconds"] = sample.uptime_seconds;
        body["listen"]["host"] = ctx.config->host;
        body["listen"]["port"] = ctx.config->port;
        body["file_root"] = ctx.config->file_root;
        body["follow_symlinks"] = ctx.config->follow_symlinks;
        body["bash"] = ctx.config->bash_path;
        body["audit_enabled"] = ctx.config->audit_enabled;
        body["audit_log"] = ctx.config->audit_enabled ? ctx.config->audit_log_path : "";
        body["data_dir"] = ctx.config->data_dir;
        body["state_dir"] = ctx.config->state_dir;
        body["max_upload_size"] = ctx.config->max_upload_size;
        body["max_terminal_sessions"] = ctx.config->max_terminal_sessions;
        body["terminal_idle_timeout"] = ctx.config->terminal_idle_timeout_seconds;
        body["metrics_interval_ms"] = ctx.config->metrics_interval_ms;
        body["api_token_configured"] = ctx.auth->token_configured();
        return http::write_json(resp, 200, body);
    });
}

void ws_metrics_onopen(RouteContext& ctx, const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    auto state = std::make_shared<WsClientState>();
    state->path = req->Path();
    state->peer_ip = http::client_ip(*ctx.config, req.get());
    channel->setContextPtr(state);

    hv::Json snapshot;
    snapshot["type"] = "snapshot";
    snapshot["interval_ms"] = ctx.config->metrics_interval_ms;
    snapshot["sample"] = metrics_json(ctx.metrics->latest());
    snapshot["history"] = history_json(
        ctx.metrics->history_samples(static_cast<std::size_t>(std::max(1, ctx.config->metrics_history))));
    channel->send(snapshot.dump(), WS_OPCODE_TEXT);

    const std::weak_ptr<hv::WebSocketChannel> weak_channel(channel);
    state->metrics_subscription = ctx.metrics->subscribe([weak_channel](const MetricsSample& sample) {
        const auto target = weak_channel.lock();
        if (!target) return;
        // Metrics are idempotent: when the socket queue is congested the sample
        // is dropped instead of buffering without bound.
        if (target->writeBufsize() > kMaxWsBacklog) return;
        hv::Json body;
        body["type"] = "metrics";
        body["sample"] = metrics_json(sample);
        target->send(body.dump(), WS_OPCODE_TEXT);
    });
    if (state->metrics_subscription == 0) {
        hv::Json body;
        body["type"] = "error";
        body["code"] = "subscriber_limit";
        body["message"] = "too many metrics streams are open";
        channel->send(body.dump(), WS_OPCODE_TEXT);
        channel->close();
    }
}

void ws_metrics_onmessage(RouteContext& ctx, const WebSocketChannelPtr& channel, const std::string& message) {
    (void)ctx;
    auto state = channel->getContextPtr<WsClientState>();
    if (!state) return;
    if (message.size() > kMaxWsMessage) {
        channel->close();
        return;
    }
    hv::Json parsed;
    try {
        parsed = hv::Json::parse(message);
    } catch (const std::exception&) {
        return;
    }
    if (parsed.is_object() && parsed.value("type", "") == "ping") {
        hv::Json body;
        body["type"] = "pong";
        channel->send(body.dump(), WS_OPCODE_TEXT);
    }
}

void ws_metrics_onclose(RouteContext& ctx, const WebSocketChannelPtr& channel) {
    auto state = channel->getContextPtr<WsClientState>();
    if (!state) return;
    state->closed = true;
    if (state->metrics_subscription != 0) {
        ctx.metrics->unsubscribe(state->metrics_subscription);
        state->metrics_subscription = 0;
    }
    channel->deleteContextPtr();
}

} // namespace edgedesk
