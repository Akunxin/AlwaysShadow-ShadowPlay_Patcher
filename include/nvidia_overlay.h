#ifndef NVIDIA_OVERLAY_H
#define NVIDIA_OVERLAY_H

#include <windows.h>

typedef enum
{
    OVERLAY_REPAIR_OK,
    OVERLAY_REPAIR_DISABLED,
    OVERLAY_REPAIR_UNAVAILABLE,
    OVERLAY_REPAIR_CANCELLED,
    OVERLAY_REPAIR_FAILED,
    OVERLAY_REPAIR_RESTORE_FAILED,
} OverlayRepairResult;

// Rechecked after loading NVIDIA's API and immediately before disabling the
// overlay. Once disabled, restoring its original enabled state takes priority
// over cancellation. This does not send an Instant Replay shortcut.
typedef BOOL (*OverlayRepairAllowedFn)(void);
OverlayRepairResult RepairNvidiaOverlay(OverlayRepairAllowedFn allowed, HRESULT *error);

#endif
