#pragma once

#include <string>
#include <vector>

namespace tabby {

std::string trimCopy(std::string text);
std::string lowerCopy(std::string text);
bool startsWith(const std::string& text, const char* prefix);
bool isAllDigits(const std::string& text);
bool parseIndex(const std::string& text, size_t& index);
std::vector<std::string> tokenize(const std::string& text);
std::string skipFirstToken(const std::string& text);
bool splitTwoArgs(const std::string& rest, std::string& first, std::string& second);

}  // namespace tabby
