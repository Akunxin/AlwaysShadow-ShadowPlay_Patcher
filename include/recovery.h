#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#define REPLAY_SETTLE_MS UINT64_C(10000)
#define REPLAY_FAST_RETRY_MS UINT64_C(10000)
#define REPLAY_RETRY_MS UINT64_C(30000)
#define REPLAY_FAST_ATTEMPTS 2u
#define REPLAY_STABLE_MS UINT64_C(30000)

typedef enum
{
    REPLAY_UNKNOWN = -1,
    REPLAY_OFF = 0,
    REPLAY_ON = 1,
} ReplayState;

typedef enum
{
    RECOVERY_WAIT,
    RECOVERY_POLL,
    RECOVERY_RELOAD,
} RecoveryAction;

// Zero-initialize before use. All times come from the same monotonic clock.
typedef struct
{
    bool desktopAvailable;
    bool reloadPending;
    uint64_t settleUntil;
    uint64_t nextAttemptAt;
    unsigned attempts;
    bool awaitingEnable;
    bool awaitingDisable;
    bool replayWasOn;
    bool trackingStableReplay;
    uint64_t replayOnSince;
} ReplayRecovery;

RecoveryAction UpdateRecovery(ReplayRecovery *state, uint64_t now, bool desktopAvailable, bool sessionChanged);
bool IsRecoveryAttemptDue(const ReplayRecovery *state, uint64_t now);
void RecordRecoveryAttempt(ReplayRecovery *state, uint64_t now, bool enable);
void ResetRecoveryAttempts(ReplayRecovery *state);
// Returns true when capture failed or unexpectedly stopped. The caller must
// revoke physical-input confirmation before performing any further work.
bool ObserveReplayState(ReplayRecovery *state, uint64_t now, ReplayState replay);

#endif
