#include "utils.h"

#include <algorithm>
#include <cctype>
#include <conio.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include <windows.h>

std::wstring toLower(const std::wstring& str) {
    std::wstring result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

std::string bytesToHexString(const uint8_t* bytes, size_t size) {
    std::ostringstream oss;
    for (size_t i = 0; i < size; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i] << " ";
    }
    return oss.str();
}

void pressAnyKeyToExit() {
    std::cout << "Press any key to exit..." << std::endl;
    int _ = _getch(); // Wait for a key press
}

std::unordered_map<std::string, bool> parseCommandLineArgs(int argc, char* argv[]) {
    std::unordered_map<std::string, bool> args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        args[arg] = true;
    }

    return args;
}

static bool isHexDigit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool parseHexString(const std::string& hex, std::vector<uint8_t>& out) {
    out.clear();
    std::string token;
    for (char c : hex) {
        if (c == ' ' || c == '\t') {
            if (!token.empty()) {
                if (token.size() != 2 || !isHexDigit(token[0]) || !isHexDigit(token[1])) {
                    return false;
                }
                out.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
                token.clear();
            }
            continue;
        }
        token.push_back(c);
    }
    if (!token.empty()) {
        if (token.size() != 2 || !isHexDigit(token[0]) || !isHexDigit(token[1])) {
            return false;
        }
        out.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
    }
    return !out.empty();
}

bool parseSignaturePattern(const std::string& pattern, std::vector<std::optional<uint8_t>>& out) {
    out.clear();
    std::string token;
    auto flushToken = [&]() -> bool {
        if (token.empty()) {
            return true;
        }
        if (token == "??" || token == "?") {
            out.push_back(std::nullopt);
        }
        else if (token.size() == 2 && isHexDigit(token[0]) && isHexDigit(token[1])) {
            out.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
        }
        else {
            return false;
        }
        token.clear();
        return true;
    };

    for (char c : pattern) {
        if (c == ' ' || c == '\t') {
            if (!flushToken()) {
                return false;
            }
            continue;
        }
        token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return flushToken() && !out.empty();
}

std::wstring stringToWstring(const std::string& str) {
    if (str.empty()) {
        return L"";
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), size);
    return result;
}

std::string wstringToString(const std::wstring& str) {
    if (str.empty()) {
        return "";
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring getExecutableDirectory() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"";
    }
    std::wstring full(path, len);
    const auto pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L"";
    }
    return full.substr(0, pos);
}

bool fileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}