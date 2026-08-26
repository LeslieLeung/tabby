#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace tabby {
namespace {

struct PingState {
    std::mutex mutex;
    std::vector<std::string> lines;
    EventGroupHandle_t done{nullptr};
};

void pingLine(PingState* state, const std::string& line) {
    const std::lock_guard<std::mutex> lock(state->mutex);
    state->lines.push_back(line);
}

void flushPingLines(Cli& cli, PingState* state) {
    std::vector<std::string> lines;
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        lines.swap(state->lines);
    }
    for (const auto& line : lines) cli.appendLine(line);
}

void onPingSuccess(esp_ping_handle_t hdl, void* args) {
    auto* state = static_cast<PingState*>(args);
    uint8_t ttl = 0;
    uint16_t seqno = 0;
    uint32_t elapsed_time = 0;
    uint32_t recv_len = 0;
    ip_addr_t target_addr = {};
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    char line[128];
    std::snprintf(line, sizeof(line), "%u bytes from %s icmp_seq=%u ttl=%u time=%u ms",
                  static_cast<unsigned>(recv_len), ipaddr_ntoa(&target_addr), static_cast<unsigned>(seqno),
                  static_cast<unsigned>(ttl), static_cast<unsigned>(elapsed_time));
    pingLine(state, line);
}

void onPingTimeout(esp_ping_handle_t hdl, void* args) {
    auto* state = static_cast<PingState*>(args);
    uint16_t seqno = 0;
    ip_addr_t target_addr = {};
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    char line[96];
    std::snprintf(line, sizeof(line), "From %s icmp_seq=%u timeout", ipaddr_ntoa(&target_addr),
                  static_cast<unsigned>(seqno));
    pingLine(state, line);
}

void onPingEnd(esp_ping_handle_t hdl, void* args) {
    auto* state = static_cast<PingState*>(args);
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t total_time_ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));
    char line[96];
    std::snprintf(line, sizeof(line), "%u packets transmitted, %u received, time %u ms",
                  static_cast<unsigned>(transmitted), static_cast<unsigned>(received),
                  static_cast<unsigned>(total_time_ms));
    pingLine(state, line);
    if (state->done) xEventGroupSetBits(state->done, 1);
}

bool resolveHost(const std::string& host, ip_addr_t& target, std::string& error) {
    struct addrinfo hint = {};
    hint.ai_family = AF_INET;
    struct addrinfo* res = nullptr;
    const int err = getaddrinfo(host.c_str(), nullptr, &hint, &res);
    if (err != 0 || res == nullptr) {
        error = "ping: unknown host " + host;
        if (res) freeaddrinfo(res);
        return false;
    }
    auto* addr4 = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    std::memset(&target, 0, sizeof(target));
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = addr4->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

void helpPing(Cli& cli) {
    cli.appendLine("ping - ICMP echo to a host");
    cli.appendLine("Usage: ping <host>");
}

bool runPing(Cli& cli, const CliArgs& args) {
    const std::string host = trimCopy(args.rest);
    if (host.empty()) {
        cli.appendLine("usage: ping <host>");
        return true;
    }
    ip_addr_t target = {};
    std::string error;
    if (!resolveHost(host, target, error)) {
        cli.appendLine(error);
        return true;
    }
    PingState state;
    state.done = xEventGroupCreate();
    if (state.done == nullptr) {
        cli.appendLine("ping: out of memory");
        return true;
    }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = 4;
    config.timeout_ms = 1000;
    config.interval_ms = 1000;
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = onPingSuccess;
    cbs.on_ping_timeout = onPingTimeout;
    cbs.on_ping_end = onPingEnd;
    cbs.cb_args = &state;
    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&config, &cbs, &ping) != ESP_OK) {
        vEventGroupDelete(state.done);
        cli.appendLine("ping: failed to start session");
        return true;
    }
    cli.appendLine(std::string("PING ") + host + " (" + ipaddr_ntoa(&target) + ")");
    esp_ping_start(ping);
    bool cancelled = false;
    for (;;) {
        const EventBits_t bits = xEventGroupWaitBits(state.done, 1, pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
        flushPingLines(cli, &state);
        if ((bits & 1) != 0) break;
        if (cli.interrupted()) {
            cancelled = true;
            esp_ping_stop(ping);
            break;
        }
    }
    if (!cancelled) esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    flushPingLines(cli, &state);
    vEventGroupDelete(state.done);
    if (cancelled) cli.appendLine("^C");
    return true;
}

}  // namespace

void registerPingCommand(CliRegistry& registry) {
    static SimpleCommand ping("ping", "ICMP echo to a host", {}, runPing, helpPing);
    ping.setInterceptHelp(true);
    registry.add(&ping);
}

}  // namespace tabby
