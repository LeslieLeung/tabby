#pragma once

#include "tabby/sd_card.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class TerminalEmulator;

namespace tabby {

class ViEditor {
public:
    static constexpr size_t kMaxBytes = 256 * 1024;
    static constexpr size_t kMaxLines = 8000;

    bool open(SdCard* sd, const std::string& path, std::string& error);
    bool active() const { return active_.load(std::memory_order_acquire); }
    bool handleKey(const std::string& key);
    void paint(TerminalEmulator& vt);
    void requestRedraw();
    bool takeStart() { return start_.exchange(false, std::memory_order_acq_rel); }
    const std::string& quitMessage() const { return quit_message_; }

private:
    enum class Mode : uint8_t { Normal, Insert, Command, Search };
    enum class Op : uint8_t { None, Delete, Yank, Change };

    struct Pos {
        size_t row{0};
        size_t col{0};
    };
    struct Snapshot {
        std::vector<std::string> lines;
        size_t row{0};
        size_t col{0};
    };

    void resetState();
    void closeEditor(const std::string& message);
    void setMessage(const std::string& text);
    void enterInsert(bool push_undo);
    void leaveInsert();
    void clampCursor();
    void rememberWantCol();
    void ensureVisible(size_t text_rows, size_t cols);
    void pushUndo();
    void undo();
    bool save(const std::string& dest, std::string& error);
    std::string serialize() const;
    std::string currentName() const;
    size_t byteCount() const;
    void insertText(const std::string& text);
    void insertNewline();
    void backspace();
    void deleteCharForward();
    void deleteCharBack();
    void deleteLines(size_t count);
    void deleteToEnd();
    void yankLines(size_t count);
    void paste(bool after);
    void replaceChar(const std::string& ch);
    bool motion(char ch, size_t count, Pos& dest, bool& linewise, bool& inclusive);
    void moveTo(const Pos& dest);
    void applyOp(Op op, const Pos& dest, bool linewise, bool inclusive);
    std::string copyRange(Pos a, Pos b, bool linewise, bool inclusive) const;
    void deleteRange(Pos a, Pos b, bool linewise, bool inclusive, bool yank);
    void handleNormal(const std::string& key);
    void handleInsert(const std::string& key);
    void handleLineMode(const std::string& key, bool search);
    void handleEscape();
    void handleEnter();
    void runCommand(const std::string& command);
    void startSearch(bool forward);
    void findNext(bool reverse, bool include_current = false);
    void page(int direction, size_t text_rows);
    std::string statusText(size_t cols) const;
    std::string visibleLine(const std::string& line, size_t col_off, size_t cols) const;

    SdCard* sd_{nullptr};
    std::string path_;
    std::string quit_message_;
    std::string message_;
    std::string cmdline_;
    std::string search_;
    std::string yank_;
    std::vector<std::string> lines_;
    std::vector<Snapshot> undo_;
    size_t undo_bytes_{0};
    std::mutex mutex_;
    std::atomic<bool> active_{false};
    std::atomic<bool> start_{false};
    std::atomic<bool> redraw_{false};
    Mode mode_{Mode::Normal};
    Op op_{Op::None};
    bool modified_{false};
    bool file_exists_{false};
    bool insert_undo_pending_{false};
    bool pending_g_{false};
    bool pending_r_{false};
    bool yank_linewise_{true};
    bool search_forward_{true};
    bool has_count_{false};
    bool alt_on_{false};
    size_t count_{0};
    size_t row_{0};
    size_t col_{0};
    size_t want_col_{0};
    size_t scroll_row_{0};
    size_t scroll_col_{0};
    size_t view_rows_{24};
};

}  // namespace tabby
