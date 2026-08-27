#pragma once

#include "tabby/app_config.hpp"
#include "tabby/bsp.hpp"
#include "tabby/cli/command.hpp"
#include "tabby/keyboard_input.hpp"
#include "tabby/python_runner.hpp"
#include "tabby/sd_card.hpp"
#include "tabby/settings_store.hpp"
#include "tabby/ssh_client.hpp"
#include "tabby/terminal_buffer.hpp"
#include "tabby/time_sync.hpp"
#include "tabby/usb_msc.hpp"
#include "tabby/usb_serial.hpp"
#include "tabby/vi_editor.hpp"
#include "tabby/wifi_station.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace tabby {

class Cli {
public:
    void attach(AppConfig* config, SettingsStore* settings, WifiStation* wifi, SshClient* ssh, PythonRunner* python,
                TerminalBuffer* terminal, SdCard* sd, TimeSync* time_sync, ViEditor* editor, BoardBsp* bsp,
                KeyboardInput* keyboard, UsbMsc* usb_msc, UsbSerial* serial);
    void appendLine(const std::string& line);
    void appendText(const std::string& text);
    bool execute(const std::string& line);
    std::vector<std::string> history() const;
    bool pythonRepl() const { return python_repl_.load(std::memory_order_acquire); }
    void setPythonRepl(bool on) { python_repl_.store(on, std::memory_order_release); }
    bool busy() const { return busy_.load(std::memory_order_acquire); }
    void requestInterrupt() { interrupt_.store(true, std::memory_order_release); }
    bool interrupted() const { return interrupt_.load(std::memory_order_acquire); }
    void markSshSessionStart() { ssh_session_start_.store(true, std::memory_order_release); }
    bool takeSshSessionStart() { return ssh_session_start_.exchange(false, std::memory_order_acq_rel); }
    void markSerialSessionStart() { serial_session_start_.store(true, std::memory_order_release); }
    bool takeSerialSessionStart() { return serial_session_start_.exchange(false, std::memory_order_acq_rel); }
    std::string prompt() const;
    const CliRegistry& registry() const { return registry_; }
    const std::atomic<bool>* interruptFlag() const { return &interrupt_; }

    AppConfig* config() { return config_; }
    SettingsStore* settings() { return settings_; }
    WifiStation* wifi() { return wifi_; }
    SshClient* ssh() { return ssh_; }
    PythonRunner* python() { return python_; }
    TerminalBuffer* terminal() { return terminal_; }
    SdCard* sd() { return sd_; }
    TimeSync* time() { return time_; }
    ViEditor* editor() { return editor_; }
    BoardBsp* bsp() { return bsp_; }
    KeyboardInput* keyboard() { return keyboard_; }
    UsbMsc* usb() { return usb_; }
    UsbSerial* serial() { return serial_; }

    void inheritCredentials(SshProfile& profile) const;

private:
    bool startWorker();
    static void workerEntry(void* context);
    void workerLoop();
    bool executeQueued(const std::string& line);

    CliRegistry registry_;
    AppConfig* config_{nullptr};
    SettingsStore* settings_{nullptr};
    WifiStation* wifi_{nullptr};
    SshClient* ssh_{nullptr};
    PythonRunner* python_{nullptr};
    TerminalBuffer* terminal_{nullptr};
    SdCard* sd_{nullptr};
    TimeSync* time_{nullptr};
    ViEditor* editor_{nullptr};
    BoardBsp* bsp_{nullptr};
    KeyboardInput* keyboard_{nullptr};
    UsbMsc* usb_{nullptr};
    UsbSerial* serial_{nullptr};
    void* command_queue_{nullptr};
    mutable std::mutex history_mutex_;
    std::vector<std::string> history_;
    static constexpr size_t kHistoryLimit = 64;
    std::atomic<bool> python_repl_{false};
    std::atomic<bool> ssh_session_start_{false};
    std::atomic<bool> serial_session_start_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> interrupt_{false};
};

}  // namespace tabby
