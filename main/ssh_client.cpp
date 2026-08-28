#include "tabby/ssh_client.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <libssh/libssh.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <vector>

namespace tabby {
namespace {
constexpr char kTag[] = "tabby_ssh";
constexpr char kSshHome[] = "/littlefs";
constexpr char kKnownHosts[] = "/littlefs/.ssh/known_hosts";
constexpr size_t kRxCap = 32 * 1024;
constexpr size_t kTxCap = 2048;

// RFC 4254 section 8 terminal-mode opcodes. Do not inherit the ESP-IDF
// console's termios: its stdin is intentionally raw/no-echo, which would make
// the remote PTY accept and execute commands without displaying typed input.
enum : uint8_t {
    kTtyOpVintr = 1,
    kTtyOpVquit = 2,
    kTtyOpVerase = 3,
    kTtyOpVkill = 4,
    kTtyOpVeof = 5,
    kTtyOpVstart = 8,
    kTtyOpVstop = 9,
    kTtyOpVsusp = 10,
    kTtyOpIcrnl = 36,
    kTtyOpIxon = 38,
    kTtyOpIsig = 50,
    kTtyOpIcanon = 51,
    kTtyOpEcho = 53,
    kTtyOpEchoe = 54,
    kTtyOpEchok = 55,
    kTtyOpIexten = 59,
    kTtyOpEchoctl = 60,
    kTtyOpOpost = 70,
    kTtyOpOnlcr = 72,
    kTtyOpCs8 = 91,
    kTtyOpIspeed = 128,
    kTtyOpOspeed = 129,
};

void appendPtyMode(std::array<unsigned char, 128>& modes, size_t& length, uint8_t opcode, uint32_t value) {
    modes[length++] = opcode;
    modes[length++] = static_cast<unsigned char>((value >> 24) & 0xFF);
    modes[length++] = static_cast<unsigned char>((value >> 16) & 0xFF);
    modes[length++] = static_cast<unsigned char>((value >> 8) & 0xFF);
    modes[length++] = static_cast<unsigned char>(value & 0xFF);
}

size_t interactivePtyModes(std::array<unsigned char, 128>& modes) {
    size_t length = 0;
    appendPtyMode(modes, length, kTtyOpVintr, 003);
    appendPtyMode(modes, length, kTtyOpVquit, 034);
    appendPtyMode(modes, length, kTtyOpVerase, 0177);
    appendPtyMode(modes, length, kTtyOpVkill, 025);
    appendPtyMode(modes, length, kTtyOpVeof, 004);
    appendPtyMode(modes, length, kTtyOpVstart, 021);
    appendPtyMode(modes, length, kTtyOpVstop, 023);
    appendPtyMode(modes, length, kTtyOpVsusp, 032);
    appendPtyMode(modes, length, kTtyOpIcrnl, 1);
    appendPtyMode(modes, length, kTtyOpIxon, 1);
    appendPtyMode(modes, length, kTtyOpIsig, 1);
    appendPtyMode(modes, length, kTtyOpIcanon, 1);
    appendPtyMode(modes, length, kTtyOpEcho, 1);
    appendPtyMode(modes, length, kTtyOpEchoe, 1);
    appendPtyMode(modes, length, kTtyOpEchok, 1);
    appendPtyMode(modes, length, kTtyOpIexten, 1);
    appendPtyMode(modes, length, kTtyOpEchoctl, 1);
    appendPtyMode(modes, length, kTtyOpOpost, 1);
    appendPtyMode(modes, length, kTtyOpOnlcr, 1);
    appendPtyMode(modes, length, kTtyOpCs8, 1);
    appendPtyMode(modes, length, kTtyOpIspeed, 38400);
    appendPtyMode(modes, length, kTtyOpOspeed, 38400);
    modes[length++] = 0;  // TTY_OP_END
    return length;
}

void ensureLibssh() {
    static std::once_flag once;
    std::call_once(once, []() {
        // libssh expands %d (identities, ~/.ssh/config) via getpwuid_r/HOME.
        // ESP-IDF has neither a passwd database nor HOME by default.
        if (getenv("HOME") == nullptr) {
            setenv("HOME", kSshHome, 0);
        }
        ssh_init();
    });
}

void closeConnection(ssh_session session, ssh_channel channel) {
    if (channel != nullptr) {
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    if (session != nullptr) {
        ssh_disconnect(session);
        ssh_free(session);
    }
}

void applySessionOptions(ssh_session session, const SshProfile& profile) {
    const int verbosity = SSH_LOG_NOLOG;
    const int port = profile.port;
    const bool process_config = false;
    int timeout = 12;
    ssh_options_set(session, SSH_OPTIONS_HOST, profile.host.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, profile.user.c_str());
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, kKnownHosts);
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
    // Skip /etc/ssh/ssh_config and ~/.ssh/config — they do not exist on-device,
    // and parsing them is what produces "Failed to process system configuration files".
    ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &process_config);
    ssh_options_set(session, SSH_OPTIONS_SSH_DIR, kSshHome);
}

void ensureKnownHostsDir() {
    mkdir("/littlefs/.ssh", 0755);
}

bool verifyHostKey(ssh_session session, std::string& error) {
    ensureKnownHostsDir();
    const enum ssh_known_hosts_e known = ssh_session_is_known_server(session);
    switch (known) {
        case SSH_KNOWN_HOSTS_OK:
            return true;
        case SSH_KNOWN_HOSTS_UNKNOWN:
        case SSH_KNOWN_HOSTS_NOT_FOUND:
            if (ssh_session_update_known_hosts(session) != SSH_OK) {
                ESP_LOGW(kTag, "could not save host key: %s", ssh_get_error(session));
            } else {
                ESP_LOGI(kTag, "stored host key in %s", kKnownHosts);
            }
            return true;
        case SSH_KNOWN_HOSTS_CHANGED:
            error = "host key changed";
            return false;
        case SSH_KNOWN_HOSTS_OTHER:
            error = "host key type mismatch";
            return false;
        case SSH_KNOWN_HOSTS_ERROR:
        default:
            error = ssh_get_error(session);
            if (error.empty()) error = "host key verification failed";
            return false;
    }
}

size_t ringUsed(size_t r, size_t w, size_t cap) {
    return (w + cap - r) % cap;
}

size_t ringSpace(size_t r, size_t w, size_t cap) {
    return cap ? cap - 1 - ringUsed(r, w, cap) : 0;
}

size_t ringPush(std::vector<uint8_t>& buf, size_t& w, size_t r, const uint8_t* data, size_t len) {
    const size_t cap = buf.size();
    size_t wrote = 0;
    while (wrote < len && ringSpace(r, w, cap) > 0) {
        buf[w] = data[wrote++];
        w = (w + 1) % cap;
    }
    return wrote;
}

size_t ringPop(std::vector<uint8_t>& buf, size_t& r, size_t w, uint8_t* data, size_t len) {
    const size_t cap = buf.size();
    size_t got = 0;
    while (got < len && ringUsed(r, w, cap) > 0) {
        data[got++] = buf[r];
        r = (r + 1) % cap;
    }
    return got;
}

}  // namespace

bool SshClient::begin() {
    // All libssh channel read/write/flush runs on a single I/O task. The UI
    // only touches TX/RX rings so a 500 ms flush cannot stall LVGL.
    ensureLibssh();
    if (io_task_ != nullptr) return true;
    rx_.assign(kRxCap, 0);
    tx_.assign(kTxCap, 0);
    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(&SshClient::ioEntry, "tabby_ssh_io", 16384, this, 3, &handle, 0) != pdPASS) {
        return false;
    }
    io_task_ = handle;
    return true;
}

bool SshClient::connect(const SshProfile& profile, std::string& error, int columns, int rows) {
    const std::lock_guard<std::mutex> connect_lock(connect_mutex_);
    const uint32_t generation = generation_.load(std::memory_order_acquire);
    ensureLibssh();
    ssh_session session = ssh_new();
    if (session == nullptr) {
        error = "ssh_new failed";
        return false;
    }
    applySessionOptions(session, profile);

    if (ssh_connect(session) != SSH_OK) {
        error = ssh_get_error(session);
        ssh_free(session);
        return false;
    }
    if (!verifyHostKey(session, error)) {
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    if (ssh_userauth_password(session, nullptr, profile.password.c_str()) != SSH_AUTH_SUCCESS) {
        error = ssh_get_error(session);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    ssh_channel channel = ssh_channel_new(session);
    if (channel == nullptr) {
        error = "ssh_channel_new failed";
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    const int pty_cols = columns > 0 ? columns : pty_cols_.load(std::memory_order_acquire);
    const int pty_rows = rows > 0 ? rows : pty_rows_.load(std::memory_order_acquire);
    const char* term = profile.terminal.empty() ? "xterm-256color" : profile.terminal.c_str();
    std::array<unsigned char, 128> pty_modes{};
    const size_t pty_modes_len = interactivePtyModes(pty_modes);
    if (ssh_channel_open_session(channel) != SSH_OK ||
        ssh_channel_request_pty_size_modes(channel, term, pty_cols, pty_rows, pty_modes.data(), pty_modes_len) !=
            SSH_OK ||
        ssh_channel_request_shell(channel) != SSH_OK) {
        error = ssh_get_error(session);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    ssh_set_blocking(session, 0);
    ssh_session old_session = nullptr;
    ssh_channel old_channel = nullptr;
    bool cancelled = false;
    {
        const std::lock_guard<std::mutex> lock(session_mutex_);
        if (generation != generation_.load(std::memory_order_acquire)) {
            cancelled = true;
        } else {
            old_session = static_cast<ssh_session>(session_);
            old_channel = static_cast<ssh_channel>(channel_);
            session_ = session;
            channel_ = channel;
            generation_.fetch_add(1, std::memory_order_acq_rel);
            clearRings();
            remote_eof_.store(false, std::memory_order_release);
            live_.store(true, std::memory_order_release);
        }
    }
    if (cancelled) {
        closeConnection(session, channel);
        error = "connection cancelled";
        return false;
    }
    closeConnection(old_session, old_channel);
    pty_cols_.store(pty_cols, std::memory_order_release);
    pty_rows_.store(pty_rows, std::memory_order_release);
    ESP_LOGI(kTag, "connected %s@%s:%u pty=%dx%d", profile.user.c_str(), profile.host.c_str(), profile.port,
             pty_cols, pty_rows);
    return true;
}

void SshClient::disconnect() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    live_.store(false, std::memory_order_release);
    remote_eof_.store(true, std::memory_order_release);
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    {
        const std::lock_guard<std::mutex> lock(session_mutex_);
        session = static_cast<ssh_session>(session_);
        channel = static_cast<ssh_channel>(channel_);
        channel_ = nullptr;
        session_ = nullptr;
    }
    clearRings();
    closeConnection(session, channel);
}

bool SshClient::connected() const {
    return live_.load(std::memory_order_acquire);
}

int SshClient::read(char* buffer, size_t len) {
    if (buffer == nullptr || len == 0) return 0;
    const size_t n = rxPop(reinterpret_cast<uint8_t*>(buffer), len);
    if (n > 0) return static_cast<int>(n);
    if (remote_eof_.load(std::memory_order_acquire)) return -1;
    if (!live_.load(std::memory_order_acquire)) return -1;
    return 0;
}

bool SshClient::write(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return false;
    if (!live_.load(std::memory_order_acquire)) return false;
    const size_t n = txPush(data, len);
    if (n < len) {
        ESP_LOGW(kTag, "tx ring full, dropped %u bytes", static_cast<unsigned>(len - n));
    }
    return n == len;
}

void SshClient::ioEntry(void* arg) {
    static_cast<SshClient*>(arg)->ioLoop();
}

void SshClient::ioLoop() {
    uint8_t tx_buf[256];
    char rx_buf[512];
    for (;;) {
        bool work = false;
        const uint32_t gen = generation_.load(std::memory_order_acquire);
        const size_t ntx = txPop(tx_buf, sizeof(tx_buf));
        if (ntx > 0) {
            work = true;
            writePending(tx_buf, ntx, gen);
        }

        size_t space = 0;
        {
            const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
            space = ringSpace(rx_r_, rx_w_, rx_.size());
        }
        if (space > 0 && live_.load(std::memory_order_acquire)) {
            const size_t want = space < sizeof(rx_buf) ? space : sizeof(rx_buf);
            int n = 0;
            bool eof = false;
            {
                const std::lock_guard<std::mutex> lock(session_mutex_);
                if (gen == generation_.load(std::memory_order_acquire) && session_ && channel_) {
                    ssh_channel channel = static_cast<ssh_channel>(channel_);
                    n = ssh_channel_read_nonblocking(channel, rx_buf, want, 0);
                    if (n == 0 || n == SSH_AGAIN) {
                        n = ssh_channel_read_nonblocking(channel, rx_buf, want, 1);
                    }
                    if (n == SSH_AGAIN) n = 0;
                    if (n == SSH_EOF || n == SSH_ERROR) {
                        eof = true;
                        n = 0;
                    } else if (n == 0 && (ssh_channel_is_eof(channel) || ssh_channel_is_closed(channel))) {
                        eof = true;
                    }
                }
            }
            if (n > 0) {
                rxPush(reinterpret_cast<const uint8_t*>(rx_buf), static_cast<size_t>(n));
                work = true;
            }
            if (eof) remote_eof_.store(true, std::memory_order_release);
        }

        vTaskDelay(work ? 0 : pdMS_TO_TICKS(1));
    }
}

void SshClient::clearRings() {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    rx_r_ = rx_w_ = tx_r_ = tx_w_ = 0;
}

size_t SshClient::txPush(const uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPush(tx_, tx_w_, tx_r_, data, len);
}

size_t SshClient::txPop(uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPop(tx_, tx_r_, tx_w_, data, len);
}

size_t SshClient::rxPush(const uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPush(rx_, rx_w_, rx_r_, data, len);
}

size_t SshClient::rxPop(uint8_t* data, size_t len) {
    const std::lock_guard<std::mutex> ring_lock(ring_mutex_);
    return ringPop(rx_, rx_r_, rx_w_, data, len);
}

bool SshClient::writePending(const uint8_t* data, size_t len, uint32_t generation) {
    size_t sent = 0;
    const int64_t start = esp_timer_get_time();
    // ssh_channel_write() may accept bytes into libssh's socket buffer while a
    // non-blocking session still has pending encrypted data. Keep track of the
    // accepted payload first, then explicitly flush below before reporting
    // success to the terminal input path.
    while (sent < len && (esp_timer_get_time() - start) < 500000) {
        int n = SSH_ERROR;
        {
            const std::lock_guard<std::mutex> lock(session_mutex_);
            if (generation != generation_.load(std::memory_order_acquire) || !session_ || !channel_ ||
                ssh_channel_is_closed(static_cast<ssh_channel>(channel_))) {
                return false;
            }
            ssh_session session = static_cast<ssh_session>(session_);
            ssh_channel channel = static_cast<ssh_channel>(channel_);
            ssh_set_blocking(session, 0);
            n = ssh_channel_write(channel, data + sent, len - sent);
            if (n == SSH_AGAIN || n == 0) ssh_channel_poll(channel, 0);
            if (n < 0 && n != SSH_AGAIN) ESP_LOGW(kTag, "write failed: %s", ssh_get_error(session));
        }
        if (n == SSH_AGAIN || n == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (n < 0) return false;
        sent += static_cast<size_t>(n);
    }
    if (sent != len) {
        ESP_LOGW(kTag, "write timeout sent %u/%u", static_cast<unsigned>(sent), static_cast<unsigned>(len));
        return false;
    }

    // channel_write_common() calls ssh_channel_flush(), but for a non-blocking
    // session that flush is allowed to return SSH_AGAIN after the payload has
    // already been counted as written. Drive the socket until all queued bytes
    // are actually handed off, otherwise short keyboard packets can remain
    // buffered indefinitely while the UI only performs non-blocking reads.
    while ((esp_timer_get_time() - start) < 500000) {
        int rc = SSH_ERROR;
        {
            const std::lock_guard<std::mutex> lock(session_mutex_);
            if (generation != generation_.load(std::memory_order_acquire) || !session_ || !channel_ ||
                ssh_channel_is_closed(static_cast<ssh_channel>(channel_))) {
                return false;
            }
            ssh_session session = static_cast<ssh_session>(session_);
            rc = ssh_blocking_flush(session, 25);
            if (rc == SSH_ERROR) ESP_LOGW(kTag, "flush failed: %s", ssh_get_error(session));
        }
        if (rc == SSH_OK) return true;
        if (rc == SSH_ERROR) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(kTag, "flush timeout after %u bytes", static_cast<unsigned>(sent));
    return false;
}

void SshClient::setPtyHint(int columns, int rows) {
    if (columns > 0) pty_cols_.store(columns, std::memory_order_release);
    if (rows > 0) pty_rows_.store(rows, std::memory_order_release);
}

bool SshClient::resizePty(int columns, int rows) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        int rc = SSH_ERROR;
        {
            const std::lock_guard<std::mutex> lock(session_mutex_);
            if (!session_ || !channel_ || ssh_channel_is_closed(static_cast<ssh_channel>(channel_))) return false;
            ssh_channel channel = static_cast<ssh_channel>(channel_);
            rc = ssh_channel_change_pty_size(channel, columns, rows);
            if (rc == SSH_OK) {
                pty_cols_.store(columns, std::memory_order_release);
                pty_rows_.store(rows, std::memory_order_release);
                return true;
            }
            if (rc == SSH_AGAIN) ssh_channel_poll(channel, 0);
        }
        if (rc != SSH_AGAIN) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(kTag, "resize pty %dx%d failed", columns, rows);
    return false;
}

}  // namespace tabby
