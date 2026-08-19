#include "tabby/cli.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/cli/util.hpp"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace tabby {
namespace {

constexpr size_t kMaxDownload = 32 * 1024 * 1024;

struct DownloadState {
    FILE* file{nullptr};
    size_t bytes{0};
    bool failed{false};
    std::string error;
    Cli* cli{nullptr};
};

std::string urlBasename(const std::string& url) {
    std::string path = url;
    const auto scheme = path.find("://");
    if (scheme != std::string::npos) path = path.substr(scheme + 3);
    const auto slash = path.find('/');
    path = slash == std::string::npos ? std::string() : path.substr(slash);
    const auto query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    const auto hash = path.find('#');
    if (hash != std::string::npos) path.resize(hash);
    const auto pos = path.find_last_of('/');
    std::string name = pos == std::string::npos ? path : path.substr(pos + 1);
    return name.empty() ? "download" : name;
}

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    if (event == nullptr || event->user_data == nullptr) return ESP_OK;
    auto* state = static_cast<DownloadState*>(event->user_data);
    if (state->cli != nullptr && state->cli->interrupted()) {
        state->failed = true;
        state->error = "interrupted";
        return ESP_FAIL;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr && event->data_len > 0) {
        if (state->failed || state->file == nullptr) return ESP_OK;
        const size_t incoming = static_cast<size_t>(event->data_len);
        if (state->bytes + incoming > kMaxDownload) {
            state->failed = true;
            state->error = "download too large";
            return ESP_FAIL;
        }
        if (std::fwrite(event->data, 1, incoming, state->file) != incoming) {
            state->failed = true;
            state->error = "write failed";
            return ESP_FAIL;
        }
        state->bytes += incoming;
    }
    return ESP_OK;
}

void helpWget(Cli& cli) {
    cli.appendLine("wget (curl) - Download a URL onto the SD card");
    cli.appendLine("Usage: wget <url> [sd-path]");
    cli.appendLine("       curl <url> [sd-path]");
}

bool runWget(Cli& cli, const CliArgs& args) {
    if (args.rest.empty()) {
        cli.appendLine("usage: wget <url> [sd-path]");
        return true;
    }
    if (cli.sd() == nullptr || !cli.sd()->begin()) {
        cli.appendLine(cli.sd() ? "sd: " + cli.sd()->lastError() : "sd unavailable");
        return true;
    }
    std::string url;
    std::string dest_arg;
    const auto split = args.rest.find(' ');
    if (split == std::string::npos) {
        url = args.rest;
    } else {
        url = args.rest.substr(0, split);
        dest_arg = trimCopy(args.rest.substr(split + 1));
    }
    if (!(startsWith(lowerCopy(url), "http://") || startsWith(lowerCopy(url), "https://"))) {
        cli.appendLine("wget: url must start with http:// or https://");
        return true;
    }
    std::string dest = dest_arg.empty() ? urlBasename(url) : dest_arg;
    bool dest_dir = false;
    const std::string virtual_dest = cli.sd()->virtualPath(dest);
    if (cli.sd()->exists(virtual_dest, &dest_dir) && dest_dir) {
        dest = virtual_dest + "/" + urlBasename(url);
    }
    const std::string path = cli.sd()->virtualPath(dest);
    FILE* file = std::fopen(cli.sd()->fsPath(path).c_str(), "wb");
    if (file == nullptr) {
        cli.appendLine("wget: cannot create " + path);
        return true;
    }
    DownloadState state;
    state.file = file;
    state.cli = &cli;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = onHttpEvent;
    config.user_data = &state;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    auto client = esp_http_client_init(&config);
    if (client == nullptr) {
        std::fclose(file);
        cli.appendLine("wget: failed to start HTTP client");
        return true;
    }
    cli.appendLine("GET " + url);
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    std::fclose(file);
    if (err != ESP_OK || state.failed || status < 200 || status >= 300) {
        unlink(cli.sd()->fsPath(path).c_str());
        if (state.error == "interrupted") cli.appendLine("^C");
        else if (!state.error.empty()) cli.appendLine("wget: " + state.error);
        else if (err != ESP_OK) cli.appendLine(std::string("wget: ") + esp_err_to_name(err));
        else cli.appendLine("wget: HTTP " + std::to_string(status));
        return true;
    }
    cli.appendLine("saved " + std::to_string(state.bytes) + " bytes to " + path);
    return true;
}

}  // namespace

void registerWgetCommand(CliRegistry& registry) {
    static SimpleCommand wget("wget", "Download a URL onto the SD card", {"curl"}, runWget, helpWget);
    wget.setInterceptHelp(true);
    registry.add(&wget);
}

}  // namespace tabby
