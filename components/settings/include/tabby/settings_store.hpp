#pragma once

#include "tabby/app_config.hpp"

#include <mutex>
#include <string>

namespace tabby {

class SettingsStore {
public:
    bool begin();
    bool load(AppConfig& config);
    bool save(const AppConfig& config);
    std::string lastError() const;

private:
    bool writeFile(const std::string& json);
    bool startWriter();
    static void writerEntry(void* context);
    void writerLoop();
    void setError(const std::string& error);

    mutable std::mutex error_mutex_;
    std::mutex writer_mutex_;
    std::string last_error_;
    void* save_queue_{nullptr};
};

}  // namespace tabby
