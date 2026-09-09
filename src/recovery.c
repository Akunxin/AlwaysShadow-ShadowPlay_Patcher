// Session recovery and retry timing, independent of Windows and NVIDIA APIs.
#include "recovery.h"

void ResetRecoveryAttempts(ReplayRecovery *state)
{
    state->attempts = 0;
    state->nextAttemptAt = 0;
}

RecoveryAction UpdateRecovery(ReplayRecovery *state, uint64_t now, bool desktopAvailable, bool sessionChanged)
{
    if (!desktopAvailable)
    {
        state->desktopAvailable = false;
        state->reloadPending = true;
        ResetRecoveryAttempts(state);
        return RECOVERY_WAIT;
    }

    if (!state->desktopAvailable || sessionChanged)
    {
        state->desktopAvailable = true;
        state->reloadPending = true;
        state->settleUntil = now + REPLAY_SETTLE_MS;
        ResetRecoveryAttempts(state);
    }

    if (now < state->settleUntil) return RECOVERY_WAIT;

    if (state->reloadPending)
    {
        state->reloadPending = false;
        return RECOVERY_RELOAD;
    }

    return RECOVERY_POLL;
}

bool IsRecoveryAttemptDue(const ReplayRecovery *state, uint64_t now)
{
    return state->desktopAvailable && now >= state->settleUntil && now >= state->nextAttemptAt;
}

void RecordRecoveryAttempt(ReplayRecovery *state, uint64_t now)
{
    // Saturate the counter: a remote-control app can keep capture unavailable for days.
    if (state->attempts < REPLAY_FAST_ATTEMPTS) state->attempts++;
    state->nextAttemptAt = now + (state->attempts < REPLAY_FAST_ATTEMPTS ? REPLAY_FAST_RETRY_MS : REPLAY_RETRY_MS);
}
