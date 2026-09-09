// Adapted from the 2.0 scanner. Require one unique executable address.
#include "signature_scanner.h"
#include "memory.h"
#include "utils.h"

#include <algorithm>

bool parseSignaturePatternString(const std::string& pattern, std::vector<std::optional<uint8_t>>& out) {
    return parseSignaturePattern(pattern, out);
}

static bool matches(const uint8_t* bytes, const std::vector<std::optional<uint8_t>>& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i)
        if (pattern[i] && bytes[i] != *pattern[i]) return false;
    return true;
}

std::optional<SignatureMatch> scanModule(HANDLE process, uintptr_t base, size_t size,
        const std::vector<std::vector<std::optional<uint8_t>>>& patterns) {
    if (!process || !base || !size || size > UINTPTR_MAX - base || patterns.empty()) return std::nullopt;
    size_t longest = 0;
    for (const auto& pattern : patterns) longest = (std::max)(longest, pattern.size());
    if (!longest || longest > 1024) return std::nullopt;
    const uintptr_t end = base + size;
    std::optional<SignatureMatch> result;
    std::vector<uint8_t> tail;
    uintptr_t previousEnd = 0;
    for (uintptr_t address = base; address < end;) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) return std::nullopt;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (regionEnd <= address) return std::nullopt;
        const uintptr_t scanEnd = (std::min)(end, regionEnd);
        const DWORD protection = region.Protect & 0xff;
        const bool executable = protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
                                protection == PAGE_EXECUTE_WRITECOPY;
        if (region.State != MEM_COMMIT || !executable || (region.Protect & PAGE_GUARD)) {
            tail.clear();
            previousEnd = 0;
            address = scanEnd;
            continue;
        }
        while (address < scanEnd) {
            constexpr size_t chunkSize = 1024 * 1024;
            const size_t count = (std::min)(chunkSize, static_cast<size_t>(scanEnd - address));
            if (previousEnd != address) tail.clear();
            std::vector<uint8_t> data(tail.size() + count);
            std::copy(tail.begin(), tail.end(), data.begin());
            if (!readMemory(process, address, std::span<uint8_t>(data.data() + tail.size(), count)))
                return std::nullopt; // An incomplete scan cannot establish uniqueness.
            const uintptr_t dataBase = address - tail.size();
            for (size_t p = 0; p < patterns.size(); ++p) {
                const auto& pattern = patterns[p];
                if (pattern.empty() || pattern.size() > data.size()) continue;
                for (size_t offset = 0; offset <= data.size() - pattern.size(); ++offset) {
                    if (!matches(data.data() + offset, pattern)) continue;
                    const uintptr_t found = dataBase + offset;
                    if (result && result->address != found) return std::nullopt;
                    result = SignatureMatch{found, p};
                }
            }
            const size_t kept = (std::min)(longest - 1, data.size());
            tail.assign(data.end() - kept, data.end());
            address += count;
            previousEnd = address;
        }
    }
    return result;
}
