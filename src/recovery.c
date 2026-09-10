// Session recovery and retry timing, independent of Windows and NVIDIA APIs.
#include "recovery.h"

void ResetRecoveryAttempts(ReplayRecovery *state)
{
    state->attempts = 0;
    state->nextAttemptAt = 0;
    state->awaitingEnable = false;
    state->awaitingDisable = false;
}

RecoveryAction UpdateRecovery(ReplayRecovery *state, uint64_t now, bool desktopAvailable, bool sessionChanged)
{
    if (!desktopAvailable)
    {
        state->desktopAvailable = false;
        state->reloadPending = true;
        ResetRecoveryAttempts(state);
        state->replayWasOn = false;
        state->trackingStableReplay = false;
        return RECOVERY_WAIT;
    }

    if (!state->desktopAvailable || sessionChanged)
    {
        const bool newlyAvailable = !state->desktopAvailable;
        state->desktopAvailable = true;
        state->reloadPending = true;
        state->settleUntil = now + REPLAY_SETTLE_MS;
        state->trackingStableReplay = false;
        // Display changes are not fresh physical input. Preserve an outstanding
        // failed enable across them, so settling cannot restart an enable loop.
        if (newlyAvailable) ResetRecoveryAttempts(state);
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

void RecordRecoveryAttempt(ReplayRecovery *state, uint64_t now, bool enable)
{
    // Keep backoff for commands to turn replay off. Failed enabling waits for
    // fresh physical input instead of retrying throughout a remote session.
    if (state->attempts < REPLAY_FAST_ATTEMPTS) state->attempts++;
    state->nextAttemptAt = now + (state->attempts < REPLAY_FAST_ATTEMPTS ? REPLAY_FAST_RETRY_MS : REPLAY_RETRY_MS);
    state->awaitingEnable = enable;
    state->awaitingDisable = !enable;
}

bool ObserveReplayState(ReplayRecovery *state, uint64_t now, ReplayState replay)
{
    if (replay == REPLAY_UNKNOWN)
    {
        // An unreadable interval is not evidence of stable recording.
        state->trackingStableReplay = false;
        return false;
    }

    if (replay == REPLAY_ON)
    {
        state->replayWasOn = true;
        if (!state->trackingStableReplay)
        {
            state->trackingStableReplay = true;
            state->replayOnSince = now;
        }
        // A brief ON status during remote control must not clear failed attempts.
        if (!state->awaitingDisable && now - state->replayOnSince >= REPLAY_STABLE_MS)
            ResetRecoveryAttempts(state);
        return false;
    }

    const bool stoppedUnexpectedly = state->replayWasOn && !state->awaitingDisable;
    const bool failedToEnable = state->awaitingEnable && now >= state->nextAttemptAt;
    state->replayWasOn = false;
    state->trackingStableReplay = false;
    if (state->awaitingDisable) ResetRecoveryAttempts(state);
    return stoppedUnexpectedly || failedToEnable;
}
