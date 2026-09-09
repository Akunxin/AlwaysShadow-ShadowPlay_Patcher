#ifndef ALWAYS_SHADOW_PATCHER_H
#define ALWAYS_SHADOW_PATCHER_H

#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PATCHER_OFF, PATCHER_PAUSED, PATCHER_WAITING, PATCHER_ACTIVE,
    PATCHER_PARTIAL, PATCHER_ERROR, PATCHER_PREVIEW
} PatcherState;

typedef enum {
    PATCH_ALLOWED, PATCH_PAUSED_BY_USER, PATCH_WAITING_FOR_DESKTOP,
    PATCH_WHITELISTED, PATCH_OUTSIDE_EXCLUSIVES
} PatcherPolicy;

typedef enum {
    PATCH_ITEM_DISABLED, PATCH_ITEM_WAITING, PATCH_ITEM_APPLIED,
    PATCH_ITEM_SKIPPED, PATCH_ITEM_FAILED
} PatcherItemState;

typedef struct {
    char id[96];
    BOOL required;
    PatcherItemState state;
} PatcherItem;

typedef struct {
    PatcherState state;
    PatcherPolicy policy;
    BOOL browserEnabled;
    BOOL browserAvailable;
    DWORD processId;
    unsigned itemCount;
    PatcherItem items[64];
    wchar_t detail[1024];
} PatcherSnapshot;

typedef void (*PatcherLogFn)(const char *message);
void PatcherInitialize(PatcherLogFn log, BOOL preview);
void PatcherTick(BOOL enabled, PatcherPolicy policy, BOOL reload);
void PatcherRequestBrowser(BOOL enabled);
void PatcherGetSnapshot(PatcherSnapshot *snapshot);
void PatcherShutdown(void);

#ifdef __cplusplus
}
#endif
#endif
