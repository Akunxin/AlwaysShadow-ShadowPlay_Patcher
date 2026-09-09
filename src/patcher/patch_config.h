#pragma once

#include "patch_types.h"

#include <optional>
#include <string>
#include <vector>

struct PatchConfig {
    int schemaVersion = 0;
    std::vector<PatchDefinition> patches;
};

std::wstring resolveConfigPath(const std::wstring& overridePath);
std::optional<PatchConfig> loadPatchConfig(const std::wstring& path, std::string& errorMessage);
bool setPatchEnabled(const std::wstring& path, const std::string& id, bool enabled, std::string& errorMessage);
