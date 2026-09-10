#include "patch_config_fixture.h"
#include <iostream>

using settingsFixture::registry;

static const char customConfig[] = R"({"schema_version":1,"note":"保留自定义字段","patches":[
  {"id":"browser_detect","description":"浏览器自定义","type":"signature_patch","module":"custom.dll",
   "enabled":true,"required":false,"overwrite_size":1,"patch_hex":"C3","signatures":["48 89 ?? C3"]},
  {"id":"custom_hook","type":"export_hook","module":"USER32.dll","export":"CustomFixture",
   "enabled":false,"required":false,"overwrite_size":5,"stub_hex":"C3","extra_field":11}
]})";

static void WriteLegacy(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(file && (file << text) && file.flush());
}

static void TestDefaultsAndPersistence(const std::filesystem::path& path) {
    registry = {};
    std::string error = "stale error";
    bool imported = true;
    auto config = loadPatchConfig(path.wstring(), error, &imported);
    assert(config && error.empty() && !imported && config->patches.size() == 3);
    assert(config->patches[0].enabled && config->patches[1].enabled && !config->patches[2].enabled);
    assert(!registry.value && !registry.writes && !std::filesystem::exists(path));
    assert(!std::filesystem::exists(path.wstring() + L".tmp"));

    assert(setPatchEnabled(path.wstring(), "browser_detect", true, error));
    config = loadPatchConfig(path.wstring(), error);
    assert(config && config->patches[2].enabled && registry.value && !std::filesystem::exists(path));
    const auto enabled = registry.value;
    const auto writes = registry.writes;
    assert(!setPatchEnabled(path.wstring(), "missing", true, error));
    assert(!error.empty() && registry.value == enabled && registry.writes == writes);

    registry.writeError = ERROR_ACCESS_DENIED;
    assert(!setPatchEnabled(path.wstring(), "browser_detect", false, error));
    assert(!error.empty() && registry.value == enabled && registry.closes == registry.creates);
    registry.writeError = ERROR_SUCCESS;
    registry.createError = ERROR_ACCESS_DENIED;
    assert(!setPatchEnabled(path.wstring(), "browser_detect", false, error));
    assert(!error.empty() && registry.value == enabled);
    registry.createError = ERROR_SUCCESS;
    assert(setPatchEnabled(path.wstring(), "browser_detect", false, error));
    config = loadPatchConfig(path.wstring(), error);
    assert(config && !config->patches[2].enabled && !std::filesystem::exists(path));
    std::cout << "Embedded defaults, persistent toggles, no config files and failed-write preservation passed.\n";
}

static void TestMigration(const std::filesystem::path& path) {
    registry = {};
    const std::string legacy = std::string("\xEF\xBB\xBF") + customConfig;
    WriteLegacy(path, legacy);
    assert(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
    std::string error;
    bool imported = false;
    auto config = loadPatchConfig(path.wstring(), error, &imported);
    assert(config && imported && registry.writes == 1 && config->patches.size() == 2);
    assert(config->patches[0].enabled && config->patches[0].module == L"custom.dll");
    assert(config->patches[0].description == "浏览器自定义" && config->patches[1].id == "custom_hook");
    assert(registry.value->find("保留自定义字段") != std::string::npos);
    assert(registry.value->find("extra_field") != std::string::npos);
    assert(readLegacyConfig(path.wstring()) == legacy);
    assert(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY);

    assert(setPatchEnabled(path.wstring(), "browser_detect", false, error));
    assert(readLegacyConfig(path.wstring()) == legacy);
    assert(registry.value->find("extra_field") != std::string::npos);
    assert(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
    std::filesystem::remove(path);
    config = loadPatchConfig(path.wstring(), error, &imported);
    assert(config && !imported && !config->patches[0].enabled && config->patches[0].module == L"custom.dll");
    assert(!std::filesystem::exists(path));

    WriteLegacy(path, "invalid JSON from an old version");
    const auto writes = registry.writes;
    config = loadPatchConfig(path.wstring(), error, &imported);
    assert(config && !imported && !config->patches[0].enabled && registry.writes == writes);

    registry = {};
    WriteLegacy(path, legacy);
    registry.writeError = ERROR_ACCESS_DENIED;
    assert(!loadPatchConfig(path.wstring(), error, &imported));
    assert(!error.empty() && !imported && !registry.value && readLegacyConfig(path.wstring()) == legacy);
    registry.writeError = ERROR_SUCCESS;
    config = loadPatchConfig(path.wstring(), error, &imported);
    assert(config && imported && config->patches[0].enabled && registry.value);
    std::filesystem::remove(path);
    std::cout << "One-time migration, read-only legacy files, Unicode/custom rules and migration retry passed.\n";
}

static void TestInvalidData(const std::filesystem::path& path) {
    const char* invalid[] = {
        R"({"schema_version":2,"patches":[]})",
        R"({"schema_version":1,"patches":[{"id":"bad","type":"export_hook","module":"USER32.dll","overwrite_size":4,"export":"GetWindowDisplayAffinity","stub_hex":"C3"}]})",
        R"({"schema_version":1,"patches":[{"id":"bad","type":"signature_patch","enabled":"false","module":"nvd3dumx.dll","overwrite_size":1,"patch_hex":"C3","signatures":["48 89 ??"]}]})",
        R"({"schema_version":1,"patches":[{"id":"bad","type":"signature_patch","module":"nvd3dumx.dll","overwrite_size":1,"patch_hex":"C3","signatures":["48 ?? ??"]}]})",
        R"({"schema_version":1,"patches":[{"id":"bad","type":"signature_patch","module":"nvd3dumx.dll","overwrite_size":1,"patch_hex":"C3","signatures":[]}]})",
        R"({"schema_version":1,"patches":[]} trailing)",
        ""
    };
    std::string error;
    for (const auto* text : invalid) {
        registry = {};
        WriteLegacy(path, text);
        assert(!loadPatchConfig(path.wstring(), error) && !error.empty());
        assert(!registry.value && !registry.writes && readLegacyConfig(path.wstring()) == text);
        registry.value = text;
        assert(!loadPatchConfig(path.wstring(), error) && !error.empty());
        assert(!setPatchEnabled(path.wstring(), "browser_detect", true, error) && !error.empty());
        assert(!registry.writes && *registry.value == text);
    }
    std::filesystem::remove(path);
    for (const auto& text : {std::string(kMaxConfigBytes + 1, ' '),
                             std::string(customConfig) + std::string("\0tail", 5)}) {
        registry = {};
        registry.value = text;
        assert(!loadPatchConfig(path.wstring(), error) && !error.empty());
        assert(registry.value == text && !registry.writes);
    }
    registry = {};
    registry.value = customConfig;
    registry.type = REG_SZ;
    assert(!loadPatchConfig(path.wstring(), error) && !error.empty() && !registry.writes);
    registry.type = REG_BINARY;
    registry.readError = ERROR_ACCESS_DENIED;
    assert(!loadPatchConfig(path.wstring(), error) && !error.empty() && !registry.writes);
    registry.readError = ERROR_SUCCESS;
    registry.dataReadError = ERROR_MORE_DATA;
    assert(!loadPatchConfig(path.wstring(), error) && !error.empty() && !registry.writes);
    std::cout << "Malformed/oversized config, registry read failures and validation before saving passed.\n";
}

int main() {
    const auto path = std::filesystem::absolute("bin/patch-settings-test-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".json");
    assert(!std::filesystem::exists(path));
    TestDefaultsAndPersistence(path);
    TestMigration(path);
    TestInvalidData(path);
    assert(!std::filesystem::exists(path) && !std::filesystem::exists(path.wstring() + L".tmp"));
    std::cout << "All settings tests passed; the real user registry was not accessed.\n";
}
