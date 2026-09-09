// Tests patch only this test process's dedicated fixture code and allocations.
// No NVIDIA process, desktop capture, startup registration or keyboard input is used.
#include "memory.h"
#include "patch_config.h"
#include "patch_manager.h"
#include "remote_hook.h"
#include "signature_patch.h"
#include "signature_scanner.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

extern "C" __attribute__((naked, noinline, used)) int PatcherFixtureFunction() {
    __asm__ volatile("mov $0x13579bdf, %eax\n\tret");
}

static std::vector<std::optional<uint8_t>> pattern(const char* text) {
    std::vector<std::optional<uint8_t>> result;
    assert(parseSignaturePatternString(text, result));
    return result;
}

static void TestConfig() {
    const auto path = std::filesystem::absolute("bin/patcher-config-test-" + std::to_string(GetCurrentProcessId()) + ".json");
    std::string error;
    auto config = loadPatchConfig(path.wstring(), error);
    assert(config && config->patches.size() == 3 && !config->patches[2].enabled);
    assert(setPatchEnabled(path.wstring(), "browser_detect", true, error));
    config = loadPatchConfig(path.wstring(), error);
    assert(config && config->patches[2].enabled && config->patches[0].required);
    assert(!setPatchEnabled(path.wstring(), "missing", true, error));
    auto invalid = [&](const char* json) {
        { std::ofstream file(path, std::ios::binary); file << json; }
        assert(!loadPatchConfig(path.wstring(), error));
        assert(!error.empty());
    };
    invalid(R"({"schema_version":2,"patches":[]})");
    invalid(R"({"schema_version":1,"patches":[{"id":"bad","type":"export_hook","module":"USER32.dll","overwrite_size":4,"export":"GetWindowDisplayAffinity","stub_hex":"C3"}]})");
    invalid(R"({"schema_version":1,"patches":[{"id":"bad","type":"signature_patch","enabled":"false","module":"nvd3dumx.dll","overwrite_size":1,"patch_hex":"C3","signatures":["48 89 ??"]}]})");
    invalid(R"({"schema_version":1,"patches":[{"id":"bad","type":"signature_patch","module":"nvd3dumx.dll","overwrite_size":1,"patch_hex":"C3","signatures":["48 ?? ??"]}]})");
    invalid(R"({"schema_version":1,"patches":[{"id":"bad","type":"signature_patch","module":"nvd3dumx.dll","overwrite_size":1,"patch_hex":"C3","signatures":[]}]})");
    invalid(R"({"schema_version":1,"patches":[]} trailing)");
    std::filesystem::remove(path);
    std::cout << "Configuration validation and atomic enable/disable passed.\n";
}

static void TestScanner() {
    constexpr size_t size = 2 * 1024 * 1024;
    auto* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    assert(data);
    std::memset(data, 0x90, size);
    const uint8_t bytes[] = {0xD1, 0xA5, 0xB6, 0xC7, 0xE8};
    const size_t offset = 1024 * 1024 - 2; // Cross the scanner's chunk boundary.
    std::memcpy(data + offset, bytes, sizeof(bytes));
    auto patterns = std::vector{pattern("D1 A5 ?? C7 E8"), pattern("D1 A5 B6 C7 E8")};
    auto found = scanModule(GetCurrentProcess(), reinterpret_cast<uintptr_t>(data), size, patterns);
    assert(found && found->address == reinterpret_cast<uintptr_t>(data + offset));
    std::memcpy(data + 64, bytes, sizeof(bytes));
    assert(!scanModule(GetCurrentProcess(), reinterpret_cast<uintptr_t>(data), size, patterns));
    std::memset(data + 64, 0x90, sizeof(bytes));
    DWORD old;
    assert(VirtualProtect(data, size, PAGE_READWRITE, &old));
    assert(!scanModule(GetCurrentProcess(), reinterpret_cast<uintptr_t>(data), size, patterns));
    assert(VirtualFree(data, 0, MEM_RELEASE));
    std::cout << "Unique signatures, wildcard matching, chunk boundaries and executable-page filtering passed.\n";
}

static void TestRemoteHook() {
    auto* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    assert(data);
    const std::vector<uint8_t> original{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};
    const std::vector<uint8_t> stub{0x48, 0x31, 0xC0, 0xC3};
    std::memcpy(data, original.data(), original.size());
    RemoteHook hook(GetCurrentProcess(), reinterpret_cast<uintptr_t>(data), original.size(), stub, original);
    assert(hook.detect() == PatchState::Unpatched);
    AppliedPatch record;
    assert(hook.install(record) && record.originalBytes == original && record.trampolineAddress);
    assert(hook.detect() == PatchState::PatchedByUs);
    AppliedPatch repeated;
    assert(hook.install(repeated) && repeated.originalBytes.empty());
    auto wrong = record;
    ++wrong.processCreated;
    assert(!hook.uninstall(wrong) && hook.detect() == PatchState::PatchedByUs);
    data[0] = 0xCC; // Another tool changes the entry point.
    assert(hook.detect() == PatchState::PatchedUnknown);
    assert(!hook.uninstall(record));
    std::memcpy(data, record.patchedBytes.data(), record.patchedBytes.size());
    assert(hook.uninstall(record));
    assert(std::memcmp(data, original.data(), original.size()) == 0);
    data[0] = 0xCC;
    assert(!hook.install(repeated));
    assert(VirtualFree(data, 0, MEM_RELEASE));
    const auto kernel = getRemoteModuleInfo(GetCurrentProcess(), L"kernel32.dll");
    const auto remote = getExportedFunctionAddress(GetCurrentProcess(), kernel.baseAddress, L"kernel32.dll", "HeapAlloc");
    assert(remote == reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapAlloc")));
    std::cout << "Hook application, idempotence, ownership, restoration and forwarded exports passed.\n";
}

static void TestManagerAndSignatureUndo() {
    wchar_t executable[32768];
    assert(GetModuleFileNameW(nullptr, executable, _countof(executable)));
    PatchDefinition definition;
    definition.id = "fixture";
    definition.type = PatchType::SignaturePatch;
    definition.module = std::filesystem::path(executable).filename().wstring();
    definition.signatures = {pattern("B8 DF 9B 57 13 C3")};
    definition.patchBytes = {0xC3};
    definition.overwriteSize = 1;
    PatchConfig config;
    config.schemaVersion = 1;
    config.patches = {definition};
    PatchManager manager(config);
    std::string error;
    assert(manager.loadStrategies(error));
    assert(manager.applyAll(GetCurrentProcess()).success);
    assert(manager.appliedPatches().size() == 1);
    assert(manager.applyAll(GetCurrentProcess()).success);
    assert(manager.appliedPatches().size() == 1);
    assert(manager.status(GetCurrentProcess()).success);
    SignaturePatch strategy(definition);
    AppliedPatch empty;
    assert(!strategy.undo(GetCurrentProcess(), empty));
    PatchManager withoutHistory(config);
    assert(withoutHistory.loadStrategies(error));
    assert(!withoutHistory.undoAll(GetCurrentProcess()).success);
    assert(strategy.detect(GetCurrentProcess()) == PatchState::PatchedByUs);
    auto wrong = manager.appliedPatches().front();
    ++wrong.processId;
    assert(!strategy.undo(GetCurrentProcess(), wrong));
    assert(manager.undoAll(GetCurrentProcess()).success);
    assert(manager.appliedPatches().empty());
    assert(PatcherFixtureFunction() == 0x13579bdf);
    assert(manager.undoAll(GetCurrentProcess()).success);
    config.patches[0].signatures = {pattern("F0 F1 F2 F3 F4 F5 F6 F7 F8 F9 FA FB FC FD FE")};
    PatchManager missingRequired(config);
    assert(missingRequired.loadStrategies(error));
    assert(!missingRequired.applyAll(GetCurrentProcess()).success);
    config.patches[0].required = false;
    PatchManager missingOptional(config);
    assert(missingOptional.loadStrategies(error));
    const auto report = missingOptional.applyAll(GetCurrentProcess());
    assert(report.success && report.entries[0].status == PatchResultStatus::Skipped);
    std::cout << "Manager lifecycle, original-byte retention, truthful undo and optional failures passed.\n";
}

int main() {
    TestConfig();
    TestScanner();
    TestRemoteHook();
    TestManagerAndSignatureUndo();
    std::cout << "All patch engine tests passed; NVIDIA was not accessed.\n";
}
