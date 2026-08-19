#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace tabby {

class PythonRunner {
public:
    using Output = std::function<void(const std::string&)>;
    bool begin();
    bool runLine(const std::string& line, const Output& output);
    bool runFile(const std::string& path, const std::string& args, const Output& output);
    void reset();
    bool running() const { return running_.load(std::memory_order_acquire); }
    void requestInterrupt();
    bool interruptRequested() const { return interrupt_requested_.load(std::memory_order_acquire); }
    bool takeInterrupt() { return interrupt_requested_.exchange(false, std::memory_order_acq_rel); }
    const std::string& lastError() const { return last_error_; }
    void appendOutputChar(char c, const Output& output);

private:
    bool ensureVm();
    bool runSource(const std::string& source, const Output& output);
    bool shouldPrintExpression(const std::string& source) const;
    std::string buildArgvPrelude(const std::string& path, const std::string& args) const;
    std::string buildGfxPrelude() const;
    std::vector<std::string> splitArgs(const std::string& args) const;
    std::string pythonStringLiteral(const std::string& value) const;
    bool handleGfxOutputLine(const std::string& line);
    void flushOutput(const Output& output);

    void* heap_{nullptr};
    size_t heap_size_{0};
    bool started_{false};
    bool interrupted_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> interrupt_requested_{false};
    std::string last_error_{"MicroPython not started"};
    std::string pending_output_;
};

}  // namespace tabby
