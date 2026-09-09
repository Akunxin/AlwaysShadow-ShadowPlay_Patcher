#ifndef PROTECTION_POLICY_H
#define PROTECTION_POLICY_H
#include "patcher.h"

static inline PatcherPolicy GetProtectionPolicy(BOOL disabled, BOOL desktopReady,
        BOOL whitelisted, BOOL hasExclusives, BOOL exclusiveRunning) {
    if (disabled) return PATCH_PAUSED_BY_USER;
    if (!desktopReady) return PATCH_WAITING_FOR_DESKTOP;
    if (whitelisted) return PATCH_WHITELISTED;
    if (hasExclusives && !exclusiveRunning) return PATCH_OUTSIDE_EXCLUSIVES;
    return PATCH_ALLOWED;
}
#endif
