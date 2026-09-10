#include "recovery.h"
#include <assert.h>
#include <stdio.h>

static ReplayRecovery ReadyRecovery(uint64_t now)
{
    ReplayRecovery state = {0};
    assert(UpdateRecovery(&state, now - REPLAY_SETTLE_MS, true, false) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, now, true, false) == RECOVERY_RELOAD);
    assert(IsRecoveryAttemptDue(&state, now));
    return state;
}

static void TestRdpAndLocalLogin(void)
{
    ReplayRecovery state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000, false);

    // RDP takes the session away from the console. A disconnect alone does not
    // make the locked desktop ready, even after the old retry delay has expired.
    assert(UpdateRecovery(&state, 11000, false, true) == RECOVERY_WAIT);
    assert(!IsRecoveryAttemptDue(&state, 11000));
    assert(UpdateRecovery(&state, 60000, false, true) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 900000, false, false) == RECOVERY_WAIT);
    assert(!IsRecoveryAttemptDue(&state, 900000));

    assert(UpdateRecovery(&state, 910000, true, true) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 919999, true, false) == RECOVERY_WAIT);
    assert(!IsRecoveryAttemptDue(&state, 919999));
    assert(UpdateRecovery(&state, 920000, true, false) == RECOVERY_RELOAD);
    assert(IsRecoveryAttemptDue(&state, 920000));
    // Controls should be reloaded once, not every poll after a reconnect.
    assert(UpdateRecovery(&state, 930000, true, false) == RECOVERY_POLL);
}

static void TestStartupInRemoteSessionAndMissedNotifications(void)
{
    ReplayRecovery state = {0};
    assert(UpdateRecovery(&state, 10000, false, false) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 20000, false, false) == RECOVERY_WAIT);
    // The polling fallback also handles failure to register WTS notifications.
    assert(UpdateRecovery(&state, 30000, true, false) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 40000, true, false) == RECOVERY_RELOAD);
    assert(IsRecoveryAttemptDue(&state, 40000));
}

static void TestUnlockInterruptsBackoff(void)
{
    ReplayRecovery state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000, false);
    RecordRecoveryAttempt(&state, 20000, false);
    assert(!IsRecoveryAttemptDue(&state, 30000));

    // Locking revokes physical confirmation. A subsequent local hardware event
    // starts a new settling period and clears the old attempt history.
    assert(UpdateRecovery(&state, 30000, false, true) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 30000, true, true) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 40000, true, false) == RECOVERY_RELOAD);
    assert(IsRecoveryAttemptDue(&state, 40000));
    assert(state.attempts == 0);
}

static void TestDisplayChangesDuringSettling(void)
{
    ReplayRecovery state = {0};
    assert(UpdateRecovery(&state, 10000, true, true) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 15000, true, true) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 20000, true, false) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 25000, true, false) == RECOVERY_RELOAD);
    // Becoming unavailable again cancels readiness, including during settling.
    assert(UpdateRecovery(&state, 26000, false, false) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 30000, true, false) == RECOVERY_WAIT);
    assert(UpdateRecovery(&state, 40000, true, false) == RECOVERY_RELOAD);
}

static void TestDisplayChangeKeepsPendingFailure(void)
{
    ReplayRecovery state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000, true);
    assert(UpdateRecovery(&state, 15000, true, true) == RECOVERY_WAIT);
    assert(state.awaitingEnable && state.attempts == 1);
    assert(UpdateRecovery(&state, 25000, true, false) == RECOVERY_RELOAD);
    assert(ObserveReplayState(&state, 25000, REPLAY_OFF));
}

static void TestThirdPartyRemoteWithoutSessionEvents(void)
{
    ReplayRecovery state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000, true);
    assert(!ObserveReplayState(&state, 19999, REPLAY_OFF));
    assert(ObserveReplayState(&state, 20000, REPLAY_OFF));

    // A failed start revokes physical confirmation. A resident Sunlogin service,
    // display changes and elapsed time must not cause periodic enable attempts.
    assert(UpdateRecovery(&state, 20000, false, false) == RECOVERY_WAIT);
    for (uint64_t now = 50000; now <= 172850000; now += 30000)
    {
        assert(UpdateRecovery(&state, now, false, true) == RECOVERY_WAIT);
        assert(!IsRecoveryAttemptDue(&state, now));
    }

    // Only fresh local hardware input allows a new settling period and attempt.
    assert(UpdateRecovery(&state, 172860000, true, false) == RECOVERY_WAIT);
    assert(!IsRecoveryAttemptDue(&state, 172869999));
    assert(UpdateRecovery(&state, 172870000, true, false) == RECOVERY_RELOAD);
    assert(IsRecoveryAttemptDue(&state, 172870000));
}

static void TestBriefSuccessDoesNotResetFailures(void)
{
    ReplayRecovery state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000, true);
    assert(!ObserveReplayState(&state, 12000, REPLAY_ON));
    assert(state.attempts == 1 && state.awaitingEnable);
    // This was the six-second ON/OFF loop: pause immediately on the drop.
    assert(ObserveReplayState(&state, 16000, REPLAY_OFF));
    assert(state.attempts == 1);

    state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000, true);
    assert(!ObserveReplayState(&state, 12000, REPLAY_ON));
    assert(!ObserveReplayState(&state, 41999, REPLAY_ON));
    assert(state.attempts == 1);
    assert(!ObserveReplayState(&state, 42000, REPLAY_ON));
    assert(state.attempts == 0);
    // Even after stable local recording, an unexpected stop requires local input.
    assert(ObserveReplayState(&state, 50000, REPLAY_OFF));
}

static void TestUnknownStateAndExpectedStop(void)
{
    ReplayRecovery state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000, true);
    assert(!ObserveReplayState(&state, 12000, REPLAY_ON));
    assert(!ObserveReplayState(&state, 14000, REPLAY_UNKNOWN));
    assert(state.attempts == 1);
    assert(!ObserveReplayState(&state, 50000, REPLAY_ON));
    assert(state.attempts == 1); // Unknown time did not count toward stability.
    assert(ObserveReplayState(&state, 52000, REPLAY_OFF));

    state = ReadyRecovery(10000);
    assert(!ObserveReplayState(&state, 10000, REPLAY_ON));
    RecordRecoveryAttempt(&state, 12000, false); // Exclusive game has exited.
    assert(!ObserveReplayState(&state, 14000, REPLAY_OFF));
    assert(state.attempts == 0 && !state.awaitingDisable);
}

static void TestResetDoesNotBypassDesktopWait(void)
{
    ReplayRecovery state = {0};
    assert(UpdateRecovery(&state, 10000, true, false) == RECOVERY_WAIT);
    ResetRecoveryAttempts(&state);
    assert(!IsRecoveryAttemptDue(&state, 15000));
    assert(UpdateRecovery(&state, 15000, false, true) == RECOVERY_WAIT);
    ResetRecoveryAttempts(&state);
    assert(!IsRecoveryAttemptDue(&state, 100000));
}

static void TestLongUptime(void)
{
    uint64_t now = UINT64_C(0x100000000) + 10000;
    ReplayRecovery state = ReadyRecovery(now);
    RecordRecoveryAttempt(&state, now, false);
    assert(!IsRecoveryAttemptDue(&state, now + 9999));
    assert(IsRecoveryAttemptDue(&state, now + 10000));
    RecordRecoveryAttempt(&state, now + 10000, false);
    assert(!IsRecoveryAttemptDue(&state, now + 39999));
    assert(IsRecoveryAttemptDue(&state, now + 40000));
}

int main(void)
{
    TestRdpAndLocalLogin();
    TestStartupInRemoteSessionAndMissedNotifications();
    TestUnlockInterruptsBackoff();
    TestDisplayChangesDuringSettling();
    TestDisplayChangeKeepsPendingFailure();
    TestThirdPartyRemoteWithoutSessionEvents();
    TestBriefSuccessDoesNotResetFailures();
    TestUnknownStateAndExpectedStop();
    TestResetDoesNotBypassDesktopWait();
    TestLongUptime();
    puts("Recovery tests passed (10 scenarios).");
    return 0;
}
