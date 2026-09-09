// Adapted from ShadowPlay Patcher 2.0. Use AlwaysShadow's existing cJSON library.
#include "patch_config.h"
#include "utils.h"
#include "cJSON.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <windows.h>

namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

constexpr const char kDefaultConfig[] = R"({
  "schema_version": 1,
  "patches": [
    {
      "id": "display_affinity",
      "description": "Bypass invisible-window check",
      "type": "export_hook",
      "enabled": true,
      "required": true,
      "module": "USER32.dll",
      "export": "GetWindowDisplayAffinity",
      "stub_hex": "48 31 C0 C3",
      "overwrite_size": 6
    },
    {
      "id": "module_enum",
      "description": "Bypass Widevine module scan",
      "type": "export_hook",
      "enabled": true,
      "required": true,
      "module": "KERNEL32.dll",
      "export": "Module32FirstW",
      "stub_hex": "48 31 C0 C3",
      "overwrite_size": 7
    },
    {
      "id": "browser_detect",
      "description": "Experimental browser name scan patch; verify your driver first",
      "type": "signature_patch",
      "enabled": false,
      "required": false,
      "module": "nvd3dumx.dll",
      "signatures": [
        "4C 8B DC 55 53 49 8D AB 68 FF FF FF 48 81 EC 88 01 00 00",
        "4C 8B DC 55 53 49 8D AB 68 FF",
        "4C 8B DC 55 53 56 57 41 54 41 55"
      ],
      "patch_hex": "C3",
      "overwrite_size": 1
    }
  ]
}
)";

bool writeConfig(const std::wstring& path, const char* text, std::string& error) {
    const std::wstring temporary = path + L".tmp";
    {
        std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!output || !(output << text) || !output.flush()) {
            error = "Cannot write patches.json: " + wstringToString(path);
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace patches.json (Windows error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

Json readDocument(const std::wstring& path) {
    std::ifstream input{std::filesystem::path(path), std::ios::binary};
    if (!input) throw std::runtime_error("Cannot open patches.json: " + wstringToString(path));
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || size > 1024 * 1024) throw std::runtime_error("patches.json exceeds the 1 MiB limit");
    input.seekg(0);
    std::string text(static_cast<size_t>(size), '\0');
    if (!input.read(text.data(), size)) throw std::runtime_error("Cannot read patches.json");
    // Accept UTF-8 files saved with a BOM by Windows editors.
    const char* start = text.c_str();
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) start += 3;
    Json document(cJSON_ParseWithOpts(start, nullptr, true), cJSON_Delete);
    if (!document || !cJSON_IsObject(document.get())) throw std::runtime_error("Invalid JSON object in patches.json");
    return document;
}

const cJSON* member(const cJSON* object, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

std::string textField(const cJSON* object, const char* name) {
    const auto* item = member(object, name);
    if (!cJSON_IsString(item) || !item->valuestring || !*item->valuestring)
        throw std::runtime_error(std::string("Missing or invalid string: ") + name);
    return item->valuestring;
}

bool boolField(const cJSON* object, const char* name, bool fallback) {
    const auto* item = member(object, name);
    if (!item) return fallback;
    if (!cJSON_IsBool(item)) throw std::runtime_error(std::string("Expected boolean: ") + name);
    return cJSON_IsTrue(item);
}

size_t sizeField(const cJSON* object, const char* name, size_t minimum, size_t maximum) {
    const auto* item = member(object, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum ||
        std::floor(item->valuedouble) != item->valuedouble)
        throw std::runtime_error(std::string("Invalid size: ") + name);
    return static_cast<size_t>(item->valuedouble);
}
} // namespace

std::wstring resolveConfigPath(const std::wstring& overridePath) {
    if (!overridePath.empty()) return overridePath;
    const auto directory = getExecutableDirectory();
    return directory.empty() ? L"patches.json" : directory + L"\\patches.json";
}

std::optional<PatchConfig> loadPatchConfig(const std::wstring& path, std::string& errorMessage) {
    errorMessage.clear();
    try {
        if (!fileExists(path) && !writeConfig(path, kDefaultConfig, errorMessage)) return std::nullopt;
        auto document = readDocument(path);
        if (sizeField(document.get(), "schema_version", 1, 1) != 1) return std::nullopt;
        const auto* patches = member(document.get(), "patches");
        if (!cJSON_IsArray(patches) || cJSON_GetArraySize(patches) > 64)
            throw std::runtime_error("patches must be an array of at most 64 entries");
        PatchConfig config;
        config.schemaVersion = 1;
        std::set<std::string> ids;
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, patches) {
            PatchDefinition patch;
            patch.id = textField(entry, "id");
            if (patch.id.size() > 95) throw std::runtime_error("Patch id exceeds 95 UTF-8 bytes");
            if (!ids.insert(patch.id).second) throw std::runtime_error("Duplicate patch id: " + patch.id);
            if (cJSON_IsString(member(entry, "description"))) patch.description = member(entry, "description")->valuestring;
            patch.enabled = boolField(entry, "enabled", true);
            patch.required = boolField(entry, "required", true);
            patch.module = stringToWstring(textField(entry, "module"));
            if (patch.module.find_first_of(L"\\/:") != std::wstring::npos)
                throw std::runtime_error("module must be a DLL name, not a path");
            const auto type = textField(entry, "type");
            if (type == "export_hook") {
                patch.type = PatchType::ExportHook;
                patch.overwriteSize = sizeField(entry, "overwrite_size", 5, 64);
                patch.exportName = textField(entry, "export");
                if (!parseHexString(textField(entry, "stub_hex"), patch.stubBytes) || patch.stubBytes.size() > 4096)
                    throw std::runtime_error("Invalid or oversized stub_hex: " + patch.id);
            } else if (type == "signature_patch") {
                patch.type = PatchType::SignaturePatch;
                patch.overwriteSize = sizeField(entry, "overwrite_size", 1, 512);
                if (!parseHexString(textField(entry, "patch_hex"), patch.patchBytes) ||
                    patch.patchBytes.size() != patch.overwriteSize)
                    throw std::runtime_error("patch_hex must match overwrite_size: " + patch.id);
                const auto* signatures = member(entry, "signatures");
                if (!cJSON_IsArray(signatures) || cJSON_GetArraySize(signatures) == 0 || cJSON_GetArraySize(signatures) > 32)
                    throw std::runtime_error("Expected 1-32 signatures: " + patch.id);
                const cJSON* signature = nullptr;
                cJSON_ArrayForEach(signature, signatures) {
                    std::vector<std::optional<uint8_t>> pattern;
                    if (!cJSON_IsString(signature) || !parseSignaturePattern(signature->valuestring, pattern) ||
                        pattern.size() <= patch.overwriteSize || pattern.size() > 1024)
                        throw std::runtime_error("Signature must extend past the overwritten bytes: " + patch.id);
                    // A tail made entirely of wildcards cannot identify an already-patched function.
                    bool hasTail = false;
                    for (size_t i = patch.overwriteSize; i < pattern.size(); ++i) hasTail |= pattern[i].has_value();
                    if (!hasTail) throw std::runtime_error("Signature needs fixed bytes after the patch: " + patch.id);
                    patch.signatures.push_back(std::move(pattern));
                }
            } else {
                throw std::runtime_error("Unknown patch type: " + type);
            }
            config.patches.push_back(std::move(patch));
        }
        return config;
    } catch (const std::exception& error) {
        errorMessage = error.what();
        return std::nullopt;
    }
}

bool setPatchEnabled(const std::wstring& path, const std::string& id, bool enabled, std::string& errorMessage) {
    errorMessage.clear();
    try {
        // Validate before editing; keep all other fields and custom entries.
        if (!loadPatchConfig(path, errorMessage)) return false;
        auto document = readDocument(path);
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(document.get(), "patches")) {
            if (textField(entry, "id") != id) continue;
            auto* value = cJSON_GetObjectItemCaseSensitive(entry, "enabled");
            if (value) cJSON_SetBoolValue(value, enabled);
            else cJSON_AddBoolToObject(entry, "enabled", enabled);
            std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_Print(document.get()), cJSON_free);
            if (!text) throw std::runtime_error("Cannot serialize patches.json");
            return writeConfig(path, text.get(), errorMessage);
        }
        errorMessage = "Patch id not found: " + id;
    } catch (const std::exception& error) {
        errorMessage = error.what();
    }
    return false;
}
