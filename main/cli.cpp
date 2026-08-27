#include "tabby/cli.hpp"

#include "tabby/cli/util.hpp"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <new>

namespace tabby {

void Cli::attach(AppConfig* config, SettingsStore* settings, WifiStation* wifi, SshClient* ssh, PythonRunner* python,
                 TerminalBuffer* terminal, SdCard* sd, TimeSync* time_sync, ViEditor* editor, BoardBsp* bsp,
                 KeyboardInput* keyboard, UsbMsc* usb_msc, UsbSerial* serial) {
    config_ = config;
    settings_ = settings;
    wifi_ = wifi;
    ssh_ = ssh;
    python_ = python;
    terminal_ = terminal;
    sd_ = sd;
    time_ = time_sync;
    editor_ = editor;
    bsp_ = bsp;
    keyboard_ = keyboard;
    usb_ = usb_msc;
    serial_ = serial;
    if (registry_.commands().empty()) registerBuiltinCommands(registry_);
    startWorker();
}

std::string Cli::prompt() const {
    if (pythonRepl()) return ">>> ";
    if (sd_ && sd_->ready()) return "[tabby " + sd_->cwd() + "] ";
    return "[tabby] ";
}

bool Cli::startWorker() {
    if (command_queue_ != nullptr) return true;
    QueueHandle_t queue = xQueueCreate(8, sizeof(std::string*));
    if (queue == nullptr) return false;
    command_queue_ = queue;
    if (xTaskCreatePinnedToCore(workerEntry, "tabby_cli_work", 24576, this, 3, nullptr, 0) != pdPASS) {
        vQueueDelete(queue);
        command_queue_ = nullptr;
        return false;
    }
    return true;
}

void Cli::workerEntry(void* context) { static_cast<Cli*>(context)->workerLoop(); }

void Cli::workerLoop() {
    auto queue = static_cast<QueueHandle_t>(command_queue_);
    for (;;) {
        std::string* command = nullptr;
        if (xQueueReceive(queue, &command, portMAX_DELAY) != pdTRUE || command == nullptr) continue;
        executeQueued(*command);
        delete command;
        std::string* more = nullptr;
        if (xQueuePeek(queue, &more, 0) != pdTRUE) {
            busy_.store(false, std::memory_order_release);
            interrupt_.store(false, std::memory_order_release);
        }
    }
}

std::vector<std::string> Cli::history() const {
    const std::lock_guard<std::mutex> lock(history_mutex_);
    return history_;
}

void Cli::appendLine(const std::string& line) {
    if (terminal_ == nullptr) return;
    terminal_->append(line);
    terminal_->append("\n", 1);
}

void Cli::appendText(const std::string& text) {
    std::string rest = text;
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        appendLine(nl == std::string::npos ? rest : rest.substr(0, nl));
        if (nl == std::string::npos) break;
        rest = rest.substr(nl + 1);
    }
}

void Cli::inheritCredentials(SshProfile& profile) const {
    if (config_ == nullptr) return;
    for (const auto& saved : config_->ssh) {
        if (saved.host == profile.host && saved.user == profile.user && saved.port == profile.port) {
            if (profile.password.empty()) profile.password = saved.password;
            if (profile.terminal.empty()) profile.terminal = saved.terminal;
            return;
        }
    }
    for (const auto& saved : config_->ssh) {
        if (saved.host == profile.host && saved.user == profile.user) {
            if (profile.password.empty()) profile.password = saved.password;
            if (profile.terminal.empty()) profile.terminal = saved.terminal;
            return;
        }
    }
}

bool Cli::execute(const std::string& line) {
    const std::string trimmed = trimCopy(line);
    if (terminal_) {
        terminal_->append(prompt());
        terminal_->append(trimmed);
        terminal_->append("\n", 1);
    }
    if (!trimmed.empty()) {
        const std::lock_guard<std::mutex> lock(history_mutex_);
        history_.push_back(trimmed);
        while (history_.size() > kHistoryLimit) history_.erase(history_.begin());
    }
    if (trimmed.empty()) return true;
    auto* queued = new (std::nothrow) std::string(trimmed);
    if (queued == nullptr) {
        appendLine("command: out of memory");
        return false;
    }
    auto queue = static_cast<QueueHandle_t>(command_queue_);
    busy_.store(true, std::memory_order_release);
    interrupt_.store(false, std::memory_order_release);
    if (queue == nullptr || xQueueSend(queue, &queued, 0) != pdTRUE) {
        busy_.store(false, std::memory_order_release);
        delete queued;
        appendLine("command queue full; wait for the current operation");
        return false;
    }
    return true;
}

bool Cli::executeQueued(const std::string& trimmed) {
    if (pythonRepl()) {
        if (trimmed == "exit()" || trimmed == "quit()" || trimmed == "exit" || trimmed == "quit") {
            python_repl_.store(false, std::memory_order_release);
            appendLine("python: exit");
            return true;
        }
        auto out = [this](const std::string& text) { appendLine(text); };
        if (python_ && !python_->runLine(trimmed, out)) appendLine("python: " + python_->lastError());
        return true;
    }
    if (registry_.dispatch(*this, trimmed)) return true;
    appendLine(trimmed + ": command not found");
    appendLine("type 'help' for Tabby CLI commands");
    return false;
}

}  // namespace tabby
