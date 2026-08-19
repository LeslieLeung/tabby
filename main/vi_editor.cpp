#include "tabby/vi_editor.hpp"

#include "tabby/terminal_emulator.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tabby {
namespace {

constexpr size_t kTabStop = 8;
constexpr size_t kUndoLimit = 8;
constexpr size_t kUndoMaxBytes = 384 * 1024;

uint8_t utf8Len(uint8_t c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t utf8Count(const std::string& text) {
    size_t n = 0;
    for (size_t i = 0; i < text.size();) {
        i += utf8Len(static_cast<uint8_t>(text[i]));
        if (i > text.size()) break;
        ++n;
    }
    return n;
}

size_t utf8Offset(const std::string& text, size_t index) {
    size_t n = 0;
    size_t i = 0;
    while (i < text.size() && n < index) {
        i += utf8Len(static_cast<uint8_t>(text[i]));
        if (i > text.size()) return text.size();
        ++n;
    }
    return i;
}

std::string utf8CharAt(const std::string& text, size_t index) {
    const size_t start = utf8Offset(text, index);
    if (start >= text.size()) return {};
    const size_t len = utf8Len(static_cast<uint8_t>(text[start]));
    if (start + len > text.size()) return text.substr(start);
    return text.substr(start, len);
}

void utf8Erase(std::string& text, size_t index, size_t count = 1) {
    const size_t start = utf8Offset(text, index);
    const size_t end = utf8Offset(text, index + count);
    if (start < end && start <= text.size()) text.erase(start, end - start);
}

void utf8Insert(std::string& text, size_t index, const std::string& chunk) {
    text.insert(utf8Offset(text, index), chunk);
}

bool isWord(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

bool isCsiKey(const std::string& key) { return key.size() >= 3 && key[0] == '\x1B' && key[1] == '['; }

std::string trimCopy(std::string text) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string basenameOf(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return path.empty() ? "[No Name]" : path;
    const std::string name = path.substr(slash + 1);
    return name.empty() ? path : name;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    if (!text.empty() && text.back() == '\n' && !lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.empty()) lines.emplace_back();
    return lines;
}

size_t visualCol(const std::string& line, size_t char_index) {
    size_t vis = 0;
    size_t n = 0;
    size_t i = 0;
    while (i < line.size() && n < char_index) {
        if (line[i] == '\t') {
            vis = (vis / kTabStop + 1) * kTabStop;
            ++i;
        } else {
            i += utf8Len(static_cast<uint8_t>(line[i]));
            ++vis;
        }
        ++n;
    }
    return vis;
}

size_t charAtVisual(const std::string& line, size_t target) {
    size_t vis = 0;
    size_t n = 0;
    size_t i = 0;
    while (i < line.size() && vis < target) {
        if (line[i] == '\t') {
            const size_t next = (vis / kTabStop + 1) * kTabStop;
            if (next > target) break;
            vis = next;
            ++i;
        } else {
            i += utf8Len(static_cast<uint8_t>(line[i]));
            ++vis;
        }
        ++n;
    }
    return n;
}

}  // namespace

void ViEditor::resetState() {
    path_.clear();
    quit_message_.clear();
    message_.clear();
    cmdline_.clear();
    search_.clear();
    yank_.clear();
    lines_.clear();
    lines_.emplace_back();
    undo_.clear();
    undo_bytes_ = 0;
    mode_ = Mode::Normal;
    op_ = Op::None;
    modified_ = false;
    file_exists_ = false;
    insert_undo_pending_ = false;
    pending_g_ = false;
    pending_r_ = false;
    yank_linewise_ = true;
    search_forward_ = true;
    has_count_ = false;
    alt_on_ = false;
    count_ = 0;
    row_ = 0;
    col_ = 0;
    want_col_ = 0;
    scroll_row_ = 0;
    scroll_col_ = 0;
    view_rows_ = 24;
}

bool ViEditor::open(SdCard* sd, const std::string& path, std::string& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (active_.load(std::memory_order_acquire)) {
        error = "vi: already open";
        return false;
    }
    resetState();
    sd_ = sd;
    path_ = path;
    if (!path.empty()) {
        if (sd_ == nullptr) {
            error = "vi: SD unavailable";
            return false;
        }
        std::string text;
        bool missing = false;
        if (!sd_->readFile(path_, text, error, kMaxBytes, &missing)) {
            if (!missing) {
                resetState();
                return false;
            }
            file_exists_ = false;
            message_ = '"' + basenameOf(path_) + "\" [New File]";
        } else {
            lines_ = splitLines(text);
            if (lines_.size() > kMaxLines) {
                error = "Too many lines";
                resetState();
                return false;
            }
            file_exists_ = true;
            message_ = '"' + basenameOf(path_) + "\" " + std::to_string(lines_.size()) + "L, " +
                       std::to_string(text.size()) + "B";
        }
    } else {
        message_ = "\"[No Name]\"";
    }
    redraw_.store(true, std::memory_order_release);
    start_.store(true, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    return true;
}

void ViEditor::requestRedraw() { redraw_.store(true, std::memory_order_release); }

void ViEditor::closeEditor(const std::string& message) {
    quit_message_ = message;
    active_.store(false, std::memory_order_release);
    redraw_.store(false, std::memory_order_release);
    alt_on_ = false;
}

void ViEditor::setMessage(const std::string& text) {
    message_ = text;
    redraw_.store(true, std::memory_order_release);
}

std::string ViEditor::currentName() const { return path_.empty() ? std::string("[No Name]") : basenameOf(path_); }

size_t ViEditor::byteCount() const {
    size_t bytes = 0;
    for (const auto& line : lines_) bytes += line.size() + 1;
    return bytes;
}

std::string ViEditor::serialize() const {
    std::string out;
    size_t bytes = 0;
    for (const auto& line : lines_) bytes += line.size() + 1;
    out.reserve(bytes);
    for (const auto& line : lines_) {
        out += line;
        out += '\n';
    }
    return out;
}

void ViEditor::clampCursor() {
    if (lines_.empty()) lines_.emplace_back();
    if (row_ >= lines_.size()) row_ = lines_.size() - 1;
    const size_t n = utf8Count(lines_[row_]);
    if (mode_ == Mode::Insert) {
        if (col_ > n) col_ = n;
    } else if (n == 0) {
        col_ = 0;
    } else if (col_ >= n) {
        col_ = n - 1;
    }
}

void ViEditor::rememberWantCol() {
    if (row_ < lines_.size()) want_col_ = visualCol(lines_[row_], col_);
}

void ViEditor::ensureVisible(size_t text_rows, size_t cols) {
    if (text_rows == 0) return;
    if (row_ < scroll_row_) scroll_row_ = row_;
    if (row_ >= scroll_row_ + text_rows) scroll_row_ = row_ - text_rows + 1;
    const size_t vis = row_ < lines_.size() ? visualCol(lines_[row_], col_) : 0;
    if (vis < scroll_col_) scroll_col_ = vis;
    if (cols > 0 && vis >= scroll_col_ + cols) scroll_col_ = vis - cols + 1;
}

void ViEditor::pushUndo() {
    Snapshot snap{lines_, row_, col_};
    size_t add = 0;
    for (const auto& line : snap.lines) add += line.size() + 1;
    undo_.push_back(std::move(snap));
    undo_bytes_ += add;
    while (undo_.size() > 1 && (undo_.size() > kUndoLimit || undo_bytes_ > kUndoMaxBytes)) {
        size_t drop = 0;
        for (const auto& line : undo_.front().lines) drop += line.size() + 1;
        if (drop > undo_bytes_) undo_bytes_ = 0;
        else undo_bytes_ -= drop;
        undo_.erase(undo_.begin());
    }
}

void ViEditor::undo() {
    if (undo_.empty()) {
        setMessage("Already at oldest change");
        return;
    }
    Snapshot snap = std::move(undo_.back());
    size_t drop = 0;
    for (const auto& line : snap.lines) drop += line.size() + 1;
    undo_.pop_back();
    if (drop > undo_bytes_) undo_bytes_ = 0;
    else undo_bytes_ -= drop;
    lines_ = std::move(snap.lines);
    row_ = snap.row;
    col_ = snap.col;
    modified_ = true;
    clampCursor();
    rememberWantCol();
    setMessage("Undo");
}

void ViEditor::enterInsert(bool push_undo) {
    if (push_undo && !insert_undo_pending_) {
        this->pushUndo();
        insert_undo_pending_ = true;
    }
    mode_ = Mode::Insert;
    op_ = Op::None;
    pending_g_ = false;
    pending_r_ = false;
    has_count_ = false;
    count_ = 0;
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::leaveInsert() {
    mode_ = Mode::Normal;
    insert_undo_pending_ = false;
    clampCursor();
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

bool ViEditor::save(const std::string& dest, std::string& error) {
    const std::string target = dest.empty() ? path_ : dest;
    if (target.empty()) {
        error = "No file name";
        return false;
    }
    if (sd_ == nullptr) {
        error = "SD unavailable";
        return false;
    }
    if (!sd_->writeFile(target, serialize(), error)) return false;
    path_ = target;
    file_exists_ = true;
    modified_ = false;
    return true;
}

void ViEditor::insertText(const std::string& text) {
    if (text.empty() || row_ >= lines_.size()) return;
    utf8Insert(lines_[row_], col_, text);
    col_ += utf8Count(text);
    modified_ = true;
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::insertNewline() {
    if (row_ >= lines_.size()) return;
    const size_t off = utf8Offset(lines_[row_], col_);
    std::string rest = lines_[row_].substr(off);
    lines_[row_].resize(off);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(row_ + 1), rest);
    ++row_;
    col_ = 0;
    modified_ = true;
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::backspace() {
    if (row_ >= lines_.size()) return;
    if (col_ > 0) {
        utf8Erase(lines_[row_], col_ - 1);
        --col_;
        modified_ = true;
    } else if (row_ > 0) {
        col_ = utf8Count(lines_[row_ - 1]);
        lines_[row_ - 1] += lines_[row_];
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(row_));
        --row_;
        modified_ = true;
    }
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::deleteCharForward() {
    if (row_ >= lines_.size()) return;
    const size_t n = utf8Count(lines_[row_]);
    if (n == 0) return;
    if (col_ >= n) col_ = n - 1;
    utf8Erase(lines_[row_], col_);
    modified_ = true;
    clampCursor();
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::deleteCharBack() {
    if (col_ == 0) return;
    --col_;
    deleteCharForward();
}

void ViEditor::yankLines(size_t count) {
    if (lines_.empty()) return;
    count = std::max<size_t>(1, std::min(count, lines_.size() - row_));
    yank_.clear();
    yank_linewise_ = true;
    for (size_t i = 0; i < count; ++i) {
        if (i) yank_ += '\n';
        yank_ += lines_[row_ + i];
    }
    setMessage(count == 1 ? std::string("1 line yanked") : std::to_string(count) + " lines yanked");
}

void ViEditor::deleteLines(size_t count) {
    if (lines_.empty()) return;
    pushUndo();
    count = std::max<size_t>(1, std::min(count, lines_.size() - row_));
    yank_.clear();
    yank_linewise_ = true;
    for (size_t i = 0; i < count; ++i) {
        if (i) yank_ += '\n';
        yank_ += lines_[row_ + i];
    }
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(row_),
                 lines_.begin() + static_cast<std::ptrdiff_t>(row_ + count));
    if (lines_.empty()) lines_.emplace_back();
    if (row_ >= lines_.size()) row_ = lines_.size() - 1;
    col_ = 0;
    modified_ = true;
    rememberWantCol();
    setMessage(count == 1 ? std::string("1 line less") : std::to_string(count) + " fewer lines");
}

void ViEditor::deleteToEnd() {
    if (row_ >= lines_.size()) return;
    pushUndo();
    const size_t n = utf8Count(lines_[row_]);
    if (col_ >= n) return;
    yank_ = lines_[row_].substr(utf8Offset(lines_[row_], col_));
    yank_linewise_ = false;
    lines_[row_].resize(utf8Offset(lines_[row_], col_));
    modified_ = true;
    clampCursor();
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::paste(bool after) {
    if (yank_.empty()) return;
    pushUndo();
    if (yank_linewise_) {
        std::vector<std::string> chunk = splitLines(yank_);
        size_t insert_at = after ? row_ + 1 : row_;
        if (insert_at > lines_.size()) insert_at = lines_.size();
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(insert_at), chunk.begin(), chunk.end());
        row_ = insert_at;
        col_ = 0;
    } else {
        if (after && utf8Count(lines_[row_]) > 0) ++col_;
        utf8Insert(lines_[row_], col_, yank_);
        col_ += utf8Count(yank_);
        if (mode_ != Mode::Insert && col_ > 0) --col_;
    }
    modified_ = true;
    clampCursor();
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::replaceChar(const std::string& ch) {
    if (ch.empty() || row_ >= lines_.size()) return;
    const size_t n = utf8Count(lines_[row_]);
    if (n == 0) return;
    pushUndo();
    if (col_ >= n) col_ = n - 1;
    utf8Erase(lines_[row_], col_);
    utf8Insert(lines_[row_], col_, ch);
    modified_ = true;
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

bool ViEditor::motion(char ch, size_t count, Pos& dest, bool& linewise, bool& inclusive) {
    dest = {row_, col_};
    linewise = false;
    inclusive = false;
    count = std::max<size_t>(1, count);
    switch (ch) {
        case 'h':
            dest.col = col_ > count ? col_ - count : 0;
            return true;
        case 'l': {
            const size_t n = row_ < lines_.size() ? utf8Count(lines_[row_]) : 0;
            if (mode_ == Mode::Insert) dest.col = std::min(col_ + count, n);
            else dest.col = n == 0 ? 0 : std::min(col_ + count, n - 1);
            return true;
        }
        case 'j':
            linewise = true;
            dest.row = std::min(row_ + count, lines_.empty() ? 0 : lines_.size() - 1);
            dest.col = charAtVisual(lines_[dest.row], want_col_);
            return true;
        case 'k':
            linewise = true;
            dest.row = row_ > count ? row_ - count : 0;
            dest.col = charAtVisual(lines_[dest.row], want_col_);
            return true;
        case '0':
            dest.col = 0;
            return true;
        case '^': {
            dest.col = 0;
            const auto& line = lines_[row_];
            size_t i = 0;
            size_t n = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                i += utf8Len(static_cast<uint8_t>(line[i]));
                ++n;
            }
            dest.col = n;
            return true;
        }
        case '$': {
            inclusive = true;
            const size_t n = utf8Count(lines_[row_]);
            dest.col = n == 0 ? 0 : n - 1;
            return true;
        }
        case 'w': {
            size_t r = row_;
            size_t c = col_;
            for (size_t n = 0; n < count; ++n) {
                if (r >= lines_.size()) break;
                const auto& line = lines_[r];
                const size_t len = utf8Count(line);
                if (c >= len) {
                    if (r + 1 < lines_.size()) {
                        ++r;
                        c = 0;
                    }
                    continue;
                }
                const std::string cur0 = utf8CharAt(line, c);
                const bool word = !cur0.empty() && isWord(static_cast<unsigned char>(cur0[0]));
                while (c < len) {
                    const std::string cur = utf8CharAt(line, c);
                    const bool now = !cur.empty() && isWord(static_cast<unsigned char>(cur[0]));
                    if (cur == " " || cur == "\t" || now != word) break;
                    ++c;
                }
                while (c < len) {
                    const std::string cur = utf8CharAt(line, c);
                    if (cur != " " && cur != "\t") break;
                    ++c;
                }
                if (c >= len && r + 1 < lines_.size()) {
                    ++r;
                    c = 0;
                }
            }
            dest = {r, c};
            return true;
        }
        case 'b': {
            size_t r = row_;
            size_t c = col_;
            for (size_t n = 0; n < count; ++n) {
                if (c == 0) {
                    if (r == 0) break;
                    --r;
                    c = utf8Count(lines_[r]);
                }
                if (c > 0) --c;
                const auto& line = lines_[r];
                while (c > 0) {
                    const std::string cur = utf8CharAt(line, c);
                    if (cur != " " && cur != "\t") break;
                    --c;
                }
                if (c == 0) continue;
                const std::string start = utf8CharAt(line, c);
                const bool word = !start.empty() && isWord(static_cast<unsigned char>(start[0]));
                while (c > 0) {
                    const std::string prev = utf8CharAt(line, c - 1);
                    const bool now = !prev.empty() && isWord(static_cast<unsigned char>(prev[0]));
                    if (prev == " " || prev == "\t" || now != word) break;
                    --c;
                }
            }
            dest = {r, c};
            return true;
        }
        case 'G':
            linewise = true;
            dest.row = has_count_ ? std::min(std::max<size_t>(count, 1), lines_.size()) - 1
                                  : (lines_.empty() ? 0 : lines_.size() - 1);
            dest.col = 0;
            return true;
        case 'g':
            linewise = true;
            dest.row = has_count_ ? std::min(std::max<size_t>(count, 1), lines_.size()) - 1 : 0;
            dest.col = 0;
            return true;
        default:
            return false;
    }
}

void ViEditor::moveTo(const Pos& dest) {
    const bool vertical = dest.row != row_;
    row_ = dest.row;
    col_ = dest.col;
    clampCursor();
    if (!vertical) rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

std::string ViEditor::copyRange(Pos a, Pos b, bool linewise, bool inclusive) const {
    if (lines_.empty()) return {};
    if (a.row > b.row || (a.row == b.row && a.col > b.col)) std::swap(a, b);
    if (inclusive && !linewise) {
        const size_t n = utf8Count(lines_[b.row]);
        if (n > 0 && b.col < n) ++b.col;
    }
    std::string out;
    if (linewise) {
        for (size_t r = a.row; r <= b.row && r < lines_.size(); ++r) {
            if (r != a.row) out += '\n';
            out += lines_[r];
        }
        return out;
    }
    if (a.row == b.row) {
        const size_t start = utf8Offset(lines_[a.row], a.col);
        const size_t end = utf8Offset(lines_[a.row], b.col);
        return end > start ? lines_[a.row].substr(start, end - start) : std::string();
    }
    out = lines_[a.row].substr(utf8Offset(lines_[a.row], a.col));
    out += '\n';
    for (size_t r = a.row + 1; r < b.row && r < lines_.size(); ++r) {
        out += lines_[r];
        out += '\n';
    }
    out += lines_[b.row].substr(0, utf8Offset(lines_[b.row], b.col));
    return out;
}

void ViEditor::deleteRange(Pos a, Pos b, bool linewise, bool inclusive, bool yank) {
    if (lines_.empty()) return;
    if (a.row > b.row || (a.row == b.row && a.col > b.col)) std::swap(a, b);
    if (inclusive && !linewise) {
        const size_t n = utf8Count(lines_[b.row]);
        if (n > 0 && b.col < n) ++b.col;
    }
    if (yank) {
        yank_ = copyRange(a, b, linewise, false);
        yank_linewise_ = linewise;
    }
    if (linewise) {
        const size_t last = std::min(b.row, lines_.size() - 1);
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(a.row),
                     lines_.begin() + static_cast<std::ptrdiff_t>(last + 1));
        if (lines_.empty()) lines_.emplace_back();
        row_ = std::min(a.row, lines_.size() - 1);
        col_ = 0;
    } else if (a.row == b.row) {
        utf8Erase(lines_[a.row], a.col, b.col > a.col ? b.col - a.col : 0);
        row_ = a.row;
        col_ = a.col;
    } else {
        std::string left = lines_[a.row].substr(0, utf8Offset(lines_[a.row], a.col));
        std::string right = lines_[b.row].substr(utf8Offset(lines_[b.row], b.col));
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(a.row),
                     lines_.begin() + static_cast<std::ptrdiff_t>(b.row + 1));
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(a.row), left + right);
        row_ = a.row;
        col_ = a.col;
    }
    modified_ = true;
    clampCursor();
    rememberWantCol();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::applyOp(Op op, const Pos& dest, bool linewise, bool inclusive) {
    const Pos start{row_, col_};
    if (op == Op::None) {
        moveTo(dest);
        return;
    }
    if (op == Op::Yank) {
        yank_ = copyRange(start, dest, linewise, inclusive);
        yank_linewise_ = linewise;
        setMessage(linewise ? "lines yanked" : "text yanked");
        return;
    }
    pushUndo();
    deleteRange(start, dest, linewise, inclusive, true);
    if (op == Op::Change) enterInsert(false);
}

void ViEditor::page(int direction, size_t text_rows) {
    if (lines_.empty()) return;
    const size_t step = std::max<size_t>(1, text_rows > 1 ? text_rows - 1 : 1);
    if (direction < 0) {
        row_ = row_ > step ? row_ - step : 0;
    } else {
        row_ = std::min(row_ + step, lines_.empty() ? 0 : lines_.size() - 1);
    }
    col_ = charAtVisual(lines_[row_], want_col_);
    clampCursor();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::startSearch(bool forward) {
    search_forward_ = forward;
    mode_ = Mode::Search;
    cmdline_.clear();
    op_ = Op::None;
    pending_g_ = false;
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::findNext(bool reverse, bool include_current) {
    if (search_.empty()) {
        setMessage("No previous search");
        return;
    }
    const bool forward = reverse ? !search_forward_ : search_forward_;
    if (lines_.empty()) return;
    const size_t total = lines_.size();
    size_t r = row_;
    size_t start_byte = utf8Offset(lines_[r], include_current || !forward ? col_ : col_ + 1);

    for (size_t i = 0; i < total + 1; ++i) {
        const auto& line = lines_[r];
        size_t found = std::string::npos;
        if (forward) {
            const size_t off = i == 0 ? start_byte : 0;
            found = line.find(search_, off);
        } else {
            if (i == 0) {
                if (start_byte > 0) found = line.rfind(search_, start_byte - 1);
            } else if (!line.empty()) {
                found = line.rfind(search_);
            }
        }
        if (found != std::string::npos) {
            row_ = r;
            col_ = utf8Count(line.substr(0, found));
            rememberWantCol();
            redraw_.store(true, std::memory_order_release);
            return;
        }
        if (forward) r = r + 1 >= total ? 0 : r + 1;
        else r = r == 0 ? total - 1 : r - 1;
    }
    setMessage("Pattern not found: " + search_);
}

void ViEditor::runCommand(const std::string& command) {
    const std::string text = trimCopy(command);
    if (text.empty()) return;
    if (text == "q" || text == "quit") {
        if (modified_) {
            setMessage("No write since last change (add ! to override)");
            return;
        }
        closeEditor("");
        return;
    }
    if (text == "q!" || text == "quit!") {
        closeEditor("");
        return;
    }
    if (text == "help") {
        setMessage(":w :q :q! :wq  hjkl i a x dd yy p u / n");
        return;
    }
    if (text == "w" || text == "write") {
        std::string error;
        if (!save({}, error)) setMessage("E: " + error);
        else setMessage('"' + currentName() + "\" " + std::to_string(lines_.size()) + "L, " +
                        std::to_string(byteCount()) + "B written");
        return;
    }
    if (text == "wq" || text == "x" || text == "xit") {
        std::string error;
        if (!save({}, error)) {
            setMessage("E: " + error);
            return;
        }
        closeEditor('"' + currentName() + "\" written");
        return;
    }
    if (text.rfind("w ", 0) == 0 || text.rfind("write ", 0) == 0) {
        const std::string dest = trimCopy(text.rfind("write ", 0) == 0 ? text.substr(6) : text.substr(2));
        std::string error;
        if (!save(dest, error)) setMessage("E: " + error);
        else setMessage('"' + currentName() + "\" written");
        return;
    }
    if (text.rfind("e ", 0) == 0 || text.rfind("edit ", 0) == 0) {
        if (modified_) {
            setMessage("No write since last change");
            return;
        }
        const std::string dest = trimCopy(text.rfind("edit ", 0) == 0 ? text.substr(5) : text.substr(2));
        std::string error;
        bool missing = false;
        std::string body;
        if (sd_ == nullptr) {
            setMessage("E: SD unavailable");
            return;
        }
        if (!sd_->readFile(dest, body, error, kMaxBytes, &missing) && !missing) {
            setMessage("E: " + error);
            return;
        }
        path_ = dest;
        lines_ = missing ? std::vector<std::string>{""} : splitLines(body);
        file_exists_ = !missing;
        modified_ = false;
        row_ = 0;
        col_ = 0;
        scroll_row_ = 0;
        setMessage(missing ? '"' + basenameOf(dest) + "\" [New File]" : '"' + basenameOf(dest) + '"');
        return;
    }
    if (!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const size_t line = static_cast<size_t>(std::strtoul(text.c_str(), nullptr, 10));
        has_count_ = true;
        Pos dest;
        bool linewise = false;
        bool inclusive = false;
        motion('G', line, dest, linewise, inclusive);
        has_count_ = false;
        moveTo(dest);
        rememberWantCol();
        return;
    }
    setMessage("Not an editor command: " + text);
}

void ViEditor::handleEscape() {
    if (mode_ == Mode::Insert) {
        leaveInsert();
        return;
    }
    if (mode_ == Mode::Command || mode_ == Mode::Search) {
        mode_ = Mode::Normal;
        cmdline_.clear();
        redraw_.store(true, std::memory_order_release);
        return;
    }
    op_ = Op::None;
    pending_g_ = false;
    pending_r_ = false;
    has_count_ = false;
    count_ = 0;
    message_.clear();
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::handleEnter() {
    if (mode_ == Mode::Insert) {
        insertNewline();
        return;
    }
    if (mode_ == Mode::Command) {
        const std::string command = cmdline_;
        cmdline_.clear();
        mode_ = Mode::Normal;
        runCommand(command);
        return;
    }
    if (mode_ == Mode::Search) {
        search_ = cmdline_;
        cmdline_.clear();
        mode_ = Mode::Normal;
        findNext(false, true);
        return;
    }
}

void ViEditor::handleLineMode(const std::string& key, bool /*search*/) {
    if (key == "\x7F" || key == "\b") {
        if (!cmdline_.empty()) {
            const size_t n = utf8Count(cmdline_);
            utf8Erase(cmdline_, n > 0 ? n - 1 : 0);
            redraw_.store(true, std::memory_order_release);
        }
        return;
    }
    if (key.empty()) return;
    const unsigned char c = static_cast<unsigned char>(key[0]);
    if (c < 32) return;
    cmdline_ += key;
    redraw_.store(true, std::memory_order_release);
}

void ViEditor::handleInsert(const std::string& key) {
    if (key == "\x7F" || key == "\b") {
        backspace();
        return;
    }
    if (key == "\t") {
        insertText("    ");
        return;
    }
    if (key == "\r" || key == "\n") {
        insertNewline();
        return;
    }
    if (isCsiKey(key)) {
        Pos dest;
        bool linewise = false;
        bool inclusive = false;
        char motion_ch = 0;
        if (key == "\x1B[A") motion_ch = 'k';
        else if (key == "\x1B[B") motion_ch = 'j';
        else if (key == "\x1B[C") motion_ch = 'l';
        else if (key == "\x1B[D") motion_ch = 'h';
        else if (key == "\x1B[H") motion_ch = '0';
        else if (key == "\x1B[F") motion_ch = '$';
        else if (key == "\x1B[3~") {
            deleteCharForward();
            return;
        }
        if (motion_ch && motion(motion_ch, 1, dest, linewise, inclusive)) moveTo(dest);
        return;
    }
    if (key.empty()) return;
    const unsigned char c = static_cast<unsigned char>(key[0]);
    if (c < 32) return;
    insertText(key);
}

void ViEditor::handleNormal(const std::string& key) {
    if (isCsiKey(key)) {
        Pos dest;
        bool linewise = false;
        bool inclusive = false;
        char motion_ch = 0;
        if (key == "\x1B[A") motion_ch = 'k';
        else if (key == "\x1B[B") motion_ch = 'j';
        else if (key == "\x1B[C") motion_ch = 'l';
        else if (key == "\x1B[D") motion_ch = 'h';
        else if (key == "\x1B[H") motion_ch = '0';
        else if (key == "\x1B[F") motion_ch = '$';
        else if (key == "\x1B[5~") {
            page(-1, view_rows_);
            return;
        } else if (key == "\x1B[6~") {
            page(1, view_rows_);
            return;
        } else if (key == "\x1B[3~") {
            pushUndo();
            deleteCharForward();
            return;
        }
        if (motion_ch) {
            const size_t n = has_count_ ? std::max<size_t>(count_, 1) : 1;
            if (motion(motion_ch, n, dest, linewise, inclusive)) {
                if (op_ == Op::None) moveTo(dest);
                else applyOp(op_, dest, linewise, inclusive);
            }
            has_count_ = false;
            count_ = 0;
            op_ = Op::None;
        }
        return;
    }
    if (key.size() != 1) {
        if (!key.empty() && static_cast<unsigned char>(key[0]) >= 32) {
            // ignore unexpected multi-byte in normal mode
        }
        return;
    }
    const char ch = key[0];
    if (pending_r_) {
        pending_r_ = false;
        if (ch != '\x1B') replaceChar(key);
        return;
    }
    if (pending_g_) {
        pending_g_ = false;
        if (ch == 'g') {
            Pos dest;
            bool linewise = false;
            bool inclusive = false;
            const size_t n = has_count_ ? std::max<size_t>(count_, 1) : 1;
            motion('g', n, dest, linewise, inclusive);
            if (op_ == Op::None) moveTo(dest);
            else applyOp(op_, dest, linewise, inclusive);
            has_count_ = false;
            count_ = 0;
            op_ = Op::None;
            rememberWantCol();
        }
        return;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) && (ch != '0' || has_count_)) {
        if (!has_count_) {
            has_count_ = true;
            count_ = 0;
        }
        count_ = count_ * 10 + static_cast<size_t>(ch - '0');
        if (count_ > 9999) count_ = 9999;
        return;
    }
    const size_t n = has_count_ ? std::max<size_t>(count_, 1) : 1;
    auto clearCount = [&]() {
        has_count_ = false;
        count_ = 0;
    };

    if (ch == 'd' || ch == 'y' || ch == 'c') {
        const Op next = ch == 'd' ? Op::Delete : (ch == 'y' ? Op::Yank : Op::Change);
        if (op_ == next) {
            if (next == Op::Yank) yankLines(n);
            else if (next == Op::Delete) deleteLines(n);
            else {
                deleteLines(n);
                enterInsert(false);
            }
            op_ = Op::None;
            clearCount();
            return;
        }
        op_ = next;
        return;
    }

    Pos dest;
    bool linewise = false;
    bool inclusive = false;
    if (motion(ch, n, dest, linewise, inclusive)) {
        if (op_ == Op::None) {
            moveTo(dest);
            if (ch != 'j' && ch != 'k' && ch != 'G' && ch != 'g') rememberWantCol();
        } else {
            applyOp(op_, dest, linewise, inclusive);
            op_ = Op::None;
        }
        clearCount();
        return;
    }

    switch (ch) {
        case 'i':
            enterInsert(true);
            break;
        case 'a':
            if (utf8Count(lines_[row_]) > 0) ++col_;
            enterInsert(true);
            break;
        case 'I': {
            Pos dest0;
            bool lw = false, inc = false;
            motion('^', 1, dest0, lw, inc);
            moveTo(dest0);
            enterInsert(true);
            break;
        }
        case 'A': {
            col_ = utf8Count(lines_[row_]);
            enterInsert(true);
            break;
        }
        case 'o':
            col_ = utf8Count(lines_[row_]);
            enterInsert(true);
            insertNewline();
            break;
        case 'O':
            enterInsert(true);
            lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(row_), "");
            col_ = 0;
            modified_ = true;
            redraw_.store(true, std::memory_order_release);
            break;
        case 'x':
            pushUndo();
            for (size_t i = 0; i < n; ++i) deleteCharForward();
            break;
        case 'X':
            pushUndo();
            for (size_t i = 0; i < n; ++i) deleteCharBack();
            break;
        case 'D':
            deleteToEnd();
            break;
        case 'p':
            for (size_t i = 0; i < n; ++i) paste(true);
            break;
        case 'P':
            for (size_t i = 0; i < n; ++i) paste(false);
            break;
        case 'u':
            undo();
            break;
        case 'r':
            pending_r_ = true;
            break;
        case 'g':
            pending_g_ = true;
            return;
        case ':':
            mode_ = Mode::Command;
            cmdline_.clear();
            op_ = Op::None;
            redraw_.store(true, std::memory_order_release);
            break;
        case '/':
            startSearch(true);
            break;
        case '?':
            startSearch(false);
            break;
        case 'n':
            findNext(false);
            break;
        case 'N':
            findNext(true);
            break;
        case '\x06':  // Ctrl-F
            page(1, view_rows_);
            break;
        case '\x02':  // Ctrl-B
            page(-1, view_rows_);
            break;
        case '\x04':  // Ctrl-D
            page(1, std::max<size_t>(1, view_rows_ / 2));
            break;
        case '\x15':  // Ctrl-U
            page(-1, std::max<size_t>(1, view_rows_ / 2));
            break;
        case '\x0C':  // Ctrl-L
            redraw_.store(true, std::memory_order_release);
            break;
        case 'Z':
            break;
        default:
            if (op_ != Op::None) {
                op_ = Op::None;
                setMessage("Unknown motion");
            }
            break;
    }
    clearCount();
    op_ = Op::None;
}

bool ViEditor::handleKey(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.load(std::memory_order_acquire)) return false;
    if (key.empty()) return true;
    if (key == "\x1B" || key == "\x03") {
        handleEscape();
        return active_.load(std::memory_order_acquire);
    }
    if (key == "\r" || key == "\n") {
        handleEnter();
        return active_.load(std::memory_order_acquire);
    }
    if (mode_ == Mode::Command) {
        handleLineMode(key, false);
        return true;
    }
    if (mode_ == Mode::Search) {
        handleLineMode(key, true);
        return true;
    }
    if (mode_ == Mode::Insert) {
        handleInsert(key);
        return true;
    }
    message_.clear();
    handleNormal(key);
    return active_.load(std::memory_order_acquire);
}

std::string ViEditor::visibleLine(const std::string& line, size_t col_off, size_t cols) const {
    std::string out;
    out.reserve(cols);
    size_t vis = 0;
    size_t shown = 0;
    size_t i = 0;
    while (i < line.size() && shown < cols) {
        size_t width = 1;
        std::string glyph;
        if (line[i] == '\t') {
            width = kTabStop - (vis % kTabStop);
            glyph.assign(width, ' ');
            ++i;
        } else {
            const size_t len = utf8Len(static_cast<uint8_t>(line[i]));
            glyph = line.substr(i, std::min(len, line.size() - i));
            i += glyph.size();
        }
        if (vis + width > col_off) {
            const size_t skip = vis >= col_off ? 0 : col_off - vis;
            const size_t room = cols - shown;
            size_t take = width > skip ? width - skip : 0;
            if (take > room) take = room;
            if (take > 0) {
                if (glyph.size() == width) out.append(glyph, skip, take);
                else out += glyph;
                shown += take;
            }
        }
        vis += width;
    }
    return out;
}

std::string ViEditor::statusText(size_t cols) const {
    std::string left;
    if (mode_ == Mode::Command) left = ":" + cmdline_;
    else if (mode_ == Mode::Search) left = (search_forward_ ? "/" : "?") + cmdline_;
    else if (!message_.empty()) left = message_;
    else {
        left = '"' + currentName() + '"';
        if (!file_exists_) left += " [New]";
        if (modified_) left += " [+]";
        if (mode_ == Mode::Insert) left += "  -- INSERT --";
    }
    char right[48];
    std::snprintf(right, sizeof(right), "%u,%u", static_cast<unsigned>(row_ + 1), static_cast<unsigned>(col_ + 1));
    if (left.size() >= cols) return left.substr(0, cols);
    std::string pad(cols - left.size(), ' ');
    std::string out = left + pad;
    const size_t rlen = std::strlen(right);
    if (rlen < out.size()) out.replace(out.size() - rlen, rlen, right);
    return out;
}

void ViEditor::paint(TerminalEmulator& vt) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.load(std::memory_order_acquire)) return;
    const bool force = redraw_.exchange(false, std::memory_order_acq_rel);
    if (!force && alt_on_) return;
    const size_t cols = vt.columns() ? vt.columns() : 80;
    const size_t rows = vt.rows() ? vt.rows() : 24;
    const size_t text_rows = rows > 0 ? rows - 1 : 0;
    view_rows_ = text_rows;
    ensureVisible(text_rows, cols);

    if (!alt_on_) {
        vt.setAlternateScreen(true);
        alt_on_ = true;
    }
    for (size_t r = 0; r < text_rows; ++r) {
        const size_t line_index = scroll_row_ + r;
        if (line_index < lines_.size()) {
            vt.putRow(r, visibleLine(lines_[line_index], scroll_col_, cols));
        } else {
            vt.putRow(r, "~", 4);
        }
    }
    std::string status = statusText(cols);
    if (status.size() > cols) status.resize(cols);
    while (status.size() < cols) status.push_back(' ');
    vt.putRow(text_rows, status, 7, 0, true);

    size_t cursor_row = 0;
    size_t cursor_col = 0;
    if (mode_ == Mode::Command || mode_ == Mode::Search) {
        cursor_row = text_rows;
        cursor_col = std::min(cols > 0 ? cols - 1 : 0, 1 + utf8Count(cmdline_));
    } else {
        const size_t vis = row_ < lines_.size() ? visualCol(lines_[row_], col_) : 0;
        cursor_row = row_ >= scroll_row_ ? row_ - scroll_row_ : 0;
        cursor_col = vis >= scroll_col_ ? vis - scroll_col_ : 0;
        if (cursor_row >= text_rows) cursor_row = text_rows ? text_rows - 1 : 0;
        if (cols > 0 && cursor_col >= cols) cursor_col = cols - 1;
    }
    vt.moveCursor(cursor_row, cursor_col);
}

}  // namespace tabby
