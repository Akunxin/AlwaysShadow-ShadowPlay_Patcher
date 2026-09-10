#pragma once

// Compile the production settings code against an in-memory registry. Tests
// never read or write HKCU or change an installed application's preferences.
#include <windows.h>
#include <cassert>
#include <cstring>
#include <optional>
#include <string>

namespace settingsFixture {
struct Registry {
    std::optional<std::string> value;
    DWORD type = REG_BINARY;
    LSTATUS readError = ERROR_SUCCESS;
    LSTATUS dataReadError = ERROR_SUCCESS;
    LSTATUS createError = ERROR_SUCCESS;
    LSTATUS writeError = ERROR_SUCCESS;
    unsigned reads = 0, creates = 0, writes = 0, closes = 0;
};
inline Registry registry;

inline LSTATUS WINAPI GetValue(HKEY key, LPCWSTR subkey, LPCWSTR name, DWORD flags,
                                LPDWORD type, PVOID data, LPDWORD size) {
    assert(key == HKEY_CURRENT_USER && wcscmp(subkey, L"Software\\AlwaysShadow") == 0);
    assert(wcscmp(name, L"PatchConfig") == 0 && flags == RRF_RT_REG_BINARY && !type && size);
    ++registry.reads;
    if (registry.readError != ERROR_SUCCESS) return registry.readError;
    if (!registry.value) return ERROR_FILE_NOT_FOUND;
    if (registry.type != REG_BINARY) return ERROR_UNSUPPORTED_TYPE;
    const DWORD required = static_cast<DWORD>(registry.value->size());
    if (data) {
        if (registry.dataReadError != ERROR_SUCCESS) return registry.dataReadError;
        if (*size < required) { *size = required; return ERROR_MORE_DATA; }
        memcpy(data, registry.value->data(), required);
    }
    *size = required;
    return ERROR_SUCCESS;
}

inline LSTATUS WINAPI CreateKey(HKEY key, LPCWSTR subkey, DWORD reserved, LPWSTR className,
                                 DWORD options, REGSAM access, const LPSECURITY_ATTRIBUTES security,
                                 PHKEY result, LPDWORD disposition) {
    assert(key == HKEY_CURRENT_USER && wcscmp(subkey, L"Software\\AlwaysShadow") == 0);
    assert(!reserved && !className && options == REG_OPTION_NON_VOLATILE && access == KEY_SET_VALUE);
    assert(!security && result && !disposition);
    ++registry.creates;
    if (registry.createError != ERROR_SUCCESS) return registry.createError;
    *result = reinterpret_cast<HKEY>(&registry);
    return ERROR_SUCCESS;
}

inline LSTATUS WINAPI SetValue(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
                                const BYTE* data, DWORD size) {
    assert(key == reinterpret_cast<HKEY>(&registry) && wcscmp(name, L"PatchConfig") == 0);
    assert(!reserved && type == REG_BINARY && data && size);
    ++registry.writes;
    if (registry.writeError != ERROR_SUCCESS) return registry.writeError;
    registry.value = std::string(reinterpret_cast<const char*>(data), size);
    registry.type = type;
    return ERROR_SUCCESS;
}

inline LSTATUS WINAPI CloseKey(HKEY key) {
    assert(key == reinterpret_cast<HKEY>(&registry));
    ++registry.closes;
    return ERROR_SUCCESS;
}
} // namespace settingsFixture

#define RegGetValueW settingsFixture::GetValue
#define RegCreateKeyExW settingsFixture::CreateKey
#define RegSetValueExW settingsFixture::SetValue
#define RegCloseKey settingsFixture::CloseKey
#include "../src/patcher/patch_config.cpp"
#undef RegCloseKey
#undef RegSetValueExW
#undef RegCreateKeyExW
#undef RegGetValueW
