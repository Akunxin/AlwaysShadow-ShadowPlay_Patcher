#pragma once

#include "patch_types.h"

#include <optional>
#include <string>
#include <vector>

struct PatchConfig {
    int schemaVersion = 0;
    std::vector<PatchDefinition> patches;
};

std::wstring resolveLegacyConfigPath();
// Settings live in HKCU. legacyPath is only read for a one-time import when no
// saved configuration exists; it is never created, modified or deleted.
std::optional<PatchConfig> loadPatchConfig(const std::wstring& legacyPath, std::string& errorMessage,
                                         bool* importedLegacy = nullptr);
bool setPatchEnabled(const std::wstring& legacyPath, const std::string& id, bool enabled, std::string& errorMessage);
