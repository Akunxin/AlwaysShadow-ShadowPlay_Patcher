#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

struct SignatureMatch {
    uintptr_t address = 0;
    size_t patternIndex = 0;
};

bool parseSignaturePatternString(const std::string& pattern, std::vector<std::optional<uint8_t>>& out);
std::optional<SignatureMatch> scanModule(
    HANDLE hProcess,
    uintptr_t moduleBase,
    size_t moduleSize,
    const std::vector<std::vector<std::optional<uint8_t>>>& patterns);
