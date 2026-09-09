// Exercise the production C bridge with a fixture in this process, a controlled
// clock and a fake target locator. No NVIDIA process or saved user setting is used.
#include "patcher.h"
#include "patcher/patch_config.h"
#include "patcher/shadowplay_target.h"
#include "patcher/utils.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

static std::wstring testConfig;
static ULONGLONG testNow = 100000;
static bool targetAvailable = false;
static unsigned targetLookups = 0;
static std::wstring FakeResolveConfigPath(const std::wstring&) { return testConfig; }
static ULONGLONG FakeClock() { return testNow; }
static std::optional<ShadowPlayTarget> FakeFindTarget(std::string& error) {
    ++targetLookups;
    if (!targetAvailable) { error = "Test target is not running"; return std::nullopt; }
    HANDLE duplicate = nullptr;
    assert(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                           &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
    return ShadowPlayTarget{GetCurrentProcessId(), duplicate};
}
#define resolveConfigPath FakeResolveConfigPath
#define findShadowPlayProcess FakeFindTarget
#define GetTickCount64 FakeClock
#include "../src/patcher_bridge.cpp"
#undef GetTickCount64
#undef findShadowPlayProcess
#undef resolveConfigPath

extern "C" __attribute__((naked, noinline, used)) int BridgeFixture() {
    __asm__ volatile("mov $0x2468ace1, %eax\n\tret");
}

static void WriteConfig(bool missingRequired = false) {
    wchar_t executable[32768];
    assert(GetModuleFileNameW(nullptr, executable, _countof(executable)));
    std::ofstream file{std::filesystem::path(testConfig), std::ios::binary};
    file << "{\"schema_version\":1,\"patches\":[{\"id\":\"fixture\",\"type\":\"signature_patch\","
            "\"module\":\"" << wstringToString(std::filesystem::path(executable).filename().wstring()) <<
            "\",\"required\":true,\"enabled\":true,\"overwrite_size\":1,\"patch_hex\":\"C3\","
            "\"signatures\":[\"" << (missingRequired ? "D0 D1 D2 D3 D4 D5 D6 D7 D8 D9 DA DB DC DD" : "B8 E1 AC 68 24 C3") <<
            "\"]},{\"id\":\"browser_detect\",\"type\":\"signature_patch\",\"module\":\"absent.dll\","
            "\"required\":false,\"enabled\":false,\"overwrite_size\":1,\"patch_hex\":\"C3\","
            "\"signatures\":[\"B8 01 23 45 67 C3\"]}]}";
}

static PatcherSnapshot Snapshot() {
    PatcherSnapshot result;
    PatcherGetSnapshot(&result);
    return result;
}

int main() {
    testConfig = std::filesystem::absolute("bin/bridge-config-test-" + std::to_string(GetCurrentProcessId()) + ".json").wstring();
    WriteConfig();
    PatcherInitialize(nullptr, FALSE);
    PatcherTick(TRUE, PATCH_ALLOWED, FALSE);
    assert(Snapshot().state == PATCHER_WAITING && targetLookups == 1);
    targetAvailable = true;
    PatcherTick(TRUE, PATCH_ALLOWED, FALSE);
    assert(Snapshot().state == PATCHER_ACTIVE && Snapshot().processId == GetCurrentProcessId());

    PatcherTick(TRUE, PATCH_WHITELISTED, FALSE);
    assert(Snapshot().state == PATCHER_PAUSED && BridgeFixture() == 0x2468ace1);
    const auto beforePaused = targetLookups;
    PatcherTick(TRUE, PATCH_WAITING_FOR_DESKTOP, FALSE);
    assert(targetLookups == beforePaused);
    PatcherTick(TRUE, PATCH_ALLOWED, FALSE);
    assert(Snapshot().state == PATCHER_ACTIVE);
    PatcherTick(FALSE, PATCH_ALLOWED, FALSE);
    assert(Snapshot().state == PATCHER_OFF && BridgeFixture() == 0x2468ace1);

    PatcherTick(TRUE, PATCH_ALLOWED, FALSE);
    PatcherRequestBrowser(TRUE);
    PatcherTick(TRUE, PATCH_ALLOWED, FALSE);
    assert(Snapshot().state == PATCHER_PARTIAL && Snapshot().browserEnabled);
    assert(Snapshot().items[0].state == PATCH_ITEM_APPLIED && Snapshot().items[1].state == PATCH_ITEM_SKIPPED);
    PatcherRequestBrowser(FALSE);
    PatcherTick(TRUE, PATCH_ALLOWED, FALSE);
    assert(Snapshot().state == PATCHER_ACTIVE && !Snapshot().browserEnabled);

    { std::ofstream invalid{std::filesystem::path(testConfig)}; invalid << "invalid"; }
    PatcherTick(TRUE, PATCH_ALLOWED, TRUE);
    assert(Snapshot().state == PATCHER_ERROR && BridgeFixture() == 0x2468ace1);
    WriteConfig(true);
    testNow += 10000;
    PatcherTick(TRUE, PATCH_ALLOWED, FALSE);
    assert(Snapshot().state == PATCHER_ERROR);
    WriteConfig();
    PatcherTick(TRUE, PATCH_ALLOWED, TRUE);
    assert(Snapshot().state == PATCHER_ACTIVE);
    PatcherShutdown();
    assert(BridgeFixture() == 0x2468ace1);
    std::filesystem::remove(testConfig);
    std::cout << "Bridge tests passed: target wait, pause, whitelist, remote session, resume, config refresh, optional failures, retries and shutdown.\n";
}
