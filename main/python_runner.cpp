#include "tabby/python_runner.hpp"
#include "tabby/python_gfx.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "port/tabby_interrupt.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

extern "C" {
#include "port/micropython_embed.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
}

namespace tabby {
namespace {
constexpr char kTag[] = "tabby_py";
constexpr size_t kHeapSize = 256 * 1024;
constexpr const char kGfxMarker[] = "\x1fGFX ";
PythonRunner* g_active = nullptr;
PythonRunner::Output g_output;

bool containsStatementAssignment(const std::string& source) {
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '=') continue;
        const char prev = i > 0 ? source[i - 1] : '\0';
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (prev != '=' && prev != '!' && prev != '<' && prev != '>' && next != '=') return true;
    }
    return false;
}

}  // namespace

extern "C" void micropython_host_stdout(const char* str, size_t len) {
    if (g_active == nullptr || !g_output || str == nullptr || len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        const char c = str[i];
        if (c == '\r') continue;
        g_active->appendOutputChar(c, g_output);
    }
}

bool PythonRunner::begin() { return ensureVm(); }

void PythonRunner::requestInterrupt() {
    interrupt_requested_.store(true, std::memory_order_release);
    tabby_mp_request_interrupt();
}

bool PythonRunner::ensureVm() {
    if (started_) return true;
    if (heap_ == nullptr) {
        heap_ = heap_caps_malloc(kHeapSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (heap_ == nullptr) heap_ = heap_caps_malloc(kHeapSize, MALLOC_CAP_8BIT);
        if (heap_ == nullptr) {
            last_error_ = "failed to allocate MicroPython heap";
            return false;
        }
        heap_size_ = kHeapSize;
    }
    // stack_top is rebound in runSource(); init only needs a dummy for mp_init().
    volatile int stack_top = 0;
    mp_embed_init(heap_, heap_size_, (void *)&stack_top);
    started_ = true;
    last_error_.clear();
    ESP_LOGI(kTag, "MicroPython heap %u bytes", static_cast<unsigned>(heap_size_));
    return true;
}

void PythonRunner::flushOutput(const Output& output) {
    if (!output) {
        pending_output_.clear();
        return;
    }
    while (!pending_output_.empty()) {
        const auto newline = pending_output_.find('\n');
        if (newline == std::string::npos) {
            if (handleGfxOutputLine(pending_output_)) {
                pending_output_.clear();
                return;
            }
            output(pending_output_);
            pending_output_.clear();
            return;
        }
        const std::string line = pending_output_.substr(0, newline);
        if (!handleGfxOutputLine(line)) output(line);
        pending_output_ = pending_output_.substr(newline + 1);
    }
}

void PythonRunner::appendOutputChar(char c, const Output& output) {
    if (c == '\n') {
        if (handleGfxOutputLine(pending_output_)) {
            pending_output_.clear();
            return;
        }
        if (output) output(pending_output_);
        pending_output_.clear();
        return;
    }
    pending_output_.push_back(c);
}

bool PythonRunner::handleGfxOutputLine(const std::string& line) {
    constexpr size_t marker_len = sizeof(kGfxMarker) - 1;
    if (line.size() < marker_len || line.compare(0, marker_len, kGfxMarker) != 0) return false;
    if (!tab5_python_gfx_command(line.c_str() + marker_len)) {
        interrupted_ = true;
        tabby_mp_request_interrupt();
    }
    return true;
}

bool PythonRunner::runSource(const std::string& source, const Output& output) {
    if (!ensureVm()) return false;
    pending_output_.clear();
    interrupted_ = false;
    interrupt_requested_.store(false, std::memory_order_release);
    tabby_mp_clear_interrupt();
    running_.store(true, std::memory_order_release);
    g_active = this;
    g_output = output;
    // VM is created on main_task; scripts run on tabby_cli_work. GC walks from
    // the current SP up to stack_top, so rebind to this frame for the exec.
    volatile int stack_top = 0;
    mp_stack_set_top((void *)&stack_top);
    const int ok = mp_embed_exec_str_status(source.c_str());
    g_output = nullptr;
    g_active = nullptr;
    running_.store(false, std::memory_order_release);
    pythonGfx().setActive(false);
    flushOutput(output);
    if (!ok) {
        last_error_ = interrupted_ ? "interrupted" : "MicroPython exception";
        return false;
    }
    last_error_.clear();
    return true;
}

bool PythonRunner::shouldPrintExpression(const std::string& source) const {
    if (source.empty() || source.find('\n') != std::string::npos || source.find(';') != std::string::npos ||
        source.back() == ':') {
        return false;
    }
    std::string lower = source;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* prefixes[] = {"assert ", "break", "class ", "continue", "def ", "del ", "for ", "from ",
                                     "global ", "if ",    "import ", "nonlocal ", "pass", "raise", "return",
                                     "try",     "while ", "with ",   "yield"};
    for (const char* prefix : prefixes) {
        const size_t n = std::strlen(prefix);
        if (lower == prefix || (lower.size() >= n && lower.compare(0, n, prefix) == 0)) return false;
    }
    return !containsStatementAssignment(source);
}

void PythonRunner::reset() {
    if (started_) {
        mp_embed_deinit();
        started_ = false;
    }
    pending_output_.clear();
    last_error_.clear();
    interrupted_ = false;
    running_.store(false, std::memory_order_release);
    interrupt_requested_.store(false, std::memory_order_release);
    tabby_mp_clear_interrupt();
}

std::vector<std::string> PythonRunner::splitArgs(const std::string& args) const {
    std::vector<std::string> out;
    std::string current;
    char quote = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const char c = args[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && i + 1 < args.size()) current.push_back(args[++i]);
            else current.push_back(c);
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string PythonRunner::pythonStringLiteral(const std::string& value) const {
    std::string out = "'";
    for (char c : value) {
        if (c == '\\' || c == '\'') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string PythonRunner::buildArgvPrelude(const std::string& path, const std::string& args) const {
    std::string prelude = "argv=[" + pythonStringLiteral(path);
    for (const auto& arg : splitArgs(args)) {
        prelude += ",";
        prelude += pythonStringLiteral(arg);
    }
    prelude += "]\ntry:\n import sys\n sys.argv=argv\nexcept Exception:\n pass\n";
    return prelude;
}

std::string PythonRunner::buildGfxPrelude() const {
    std::ostringstream prelude;
    prelude << "class _Tab5Gfx:\n"
               " def _cmd(self,s): print('\\x1fGFX '+s)\n"
               " def clear(self,c=0): self._cmd('clear '+str(int(c)))\n"
               " def present(self): self._cmd('present')\n"
               " def pixel(self,x,y,c): self._cmd('px '+str(int(x))+' '+str(int(y))+' '+str(int(c)))\n"
               " def line(self,x0,y0,x1,y1,c): self._cmd('line '+str(int(x0))+' '+str(int(y0))+' '+str(int(x1))+' '+str(int(y1))+' '+str(int(c)))\n"
               " def rect(self,x,y,w,h,c,fill=1): self._cmd('rect '+str(int(x))+' '+str(int(y))+' '+str(int(w))+' '+str(int(h))+' '+str(int(c))+' '+str(int(fill)))\n"
               " def fill(self,x,y,w,h,c): self.rect(x,y,w,h,c,1)\n"
               " def circle(self,x,y,r,c,fill=1): self._cmd('circle '+str(int(x))+' '+str(int(y))+' '+str(int(r))+' '+str(int(c))+' '+str(int(fill)))\n"
               " def text(self,x,y,s,c=16777215): self._cmd('text '+str(int(x))+' '+str(int(y))+' '+str(int(c))+' '+str(s).replace('\\n',' '))\n"
               " def mono(self,x,y,cols,rows,cell,fg,bg,bits): self._cmd('mono '+str(int(x))+' '+str(int(y))+' '+str(int(cols))+' '+str(int(rows))+' '+str(int(cell))+' '+str(int(fg))+' '+str(int(bg))+' '+str(bits))\n"
            << " _w=" << tab5_python_gfx_width() << "\n"
            << " _h=" << tab5_python_gfx_height() << "\n"
            << " def size(self): return (self._w,self._h)\n"
               "gfx=_Tab5Gfx()\n";
    return prelude.str();
}

bool PythonRunner::runLine(const std::string& line, const Output& output) {
    std::string source = line;
    while (!source.empty() && std::isspace(static_cast<unsigned char>(source.front()))) source.erase(source.begin());
    while (!source.empty() && std::isspace(static_cast<unsigned char>(source.back()))) source.pop_back();
    if (source.empty()) return true;
    if (shouldPrintExpression(source)) {
        source = "__tab5_repl_result=(" + source + ")\nif __tab5_repl_result is not None:\n    print(repr(__tab5_repl_result))";
    }
    source = buildGfxPrelude() + source;
    return runSource(source, output);
}

bool PythonRunner::runFile(const std::string& path, const std::string& args, const Output& output) {
    std::ifstream file(path);
    if (!file) {
        last_error_ = "cannot open " + path;
        return false;
    }
    std::ostringstream body;
    body << file.rdbuf();
    std::string source = body.str();
    if (source.size() > 64 * 1024) {
        last_error_ = "script is too large";
        return false;
    }
    source = buildArgvPrelude(path, args) + buildGfxPrelude() + source;
    return runSource(source, output);
}

}  // namespace tabby
