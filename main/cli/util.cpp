#include "tabby/cli/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace tabby {

std::string trimCopy(std::string text) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool startsWith(const std::string& text, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return text.size() >= n && std::equal(prefix, prefix + n, text.begin());
}

bool isAllDigits(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool parseIndex(const std::string& text, size_t& index) {
    const std::string rest = trimCopy(text);
    if (!isAllDigits(rest)) return false;
    index = static_cast<size_t>(std::strtoul(rest.c_str(), nullptr, 10));
    return true;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size()) break;
        const size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        tokens.emplace_back(text.substr(start, i - start));
    }
    return tokens;
}

std::string skipFirstToken(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    return text.substr(i);
}

bool splitTwoArgs(const std::string& rest, std::string& first, std::string& second) {
    const auto tokens = tokenize(trimCopy(rest));
    if (tokens.size() != 2) return false;
    first = tokens[0];
    second = tokens[1];
    return true;
}

}  // namespace tabby
