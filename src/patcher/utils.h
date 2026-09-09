#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

std::wstring toLower(const std::wstring& str);
std::string bytesToHexString(const uint8_t* bytes, size_t size);
void pressAnyKeyToExit();
std::unordered_map<std::string, bool> parseCommandLineArgs(int argc, char* argv[]);

bool parseHexString(const std::string& hex, std::vector<uint8_t>& out);
bool parseSignaturePattern(const std::string& pattern, std::vector<std::optional<uint8_t>>& out);
std::wstring stringToWstring(const std::string& str);
std::string wstringToString(const std::wstring& str);
std::wstring getExecutableDirectory();
bool fileExists(const std::wstring& path);