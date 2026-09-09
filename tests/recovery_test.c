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
    RecordRecoveryAttempt(&state, 10000);

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
    RecordRecoveryAttempt(&state, 10000);
    RecordRecoveryAttempt(&state, 20000);
    assert(!IsRecoveryAttemptDue(&state, 30000));

    // A complete lock/unlock can occur between two polls. The event still resets
    // the backoff and gives NVIDIA a fresh settling period.
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

static void TestThirdPartyRemoteWithoutSessionEvents(void)
{
    ReplayRecovery state = ReadyRecovery(10000);
    RecordRecoveryAttempt(&state, 10000);
    assert(!IsRecoveryAttemptDue(&state, 19999));
    assert(IsRecoveryAttemptDue(&state, 20000));
    RecordRecoveryAttempt(&state, 20000);

    // Sunlogin may leave both the local desktop and its background service alive.
    // Keep trying every 30 seconds regardless of how long capture is blocked.
    for (uint64_t now = 50000; now <= 172850000; now += 30000)
    {
        assert(UpdateRecovery(&state, now - 1, true, false) == RECOVERY_POLL);
        assert(!IsRecoveryAttemptDue(&state, now - 1));
        assert(IsRecoveryAttemptDue(&state, now));
        RecordRecoveryAttempt(&state, now);
        assert(state.attempts == REPLAY_FAST_ATTEMPTS);
    }

    // Success, manual disable or a whitelist match ends the attempt streak.
    ResetRecoveryAttempts(&state);
    assert(IsRecoveryAttemptDue(&state, 172860000));
    assert(state.attempts == 0);
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
    RecordRecoveryAttempt(&state, now);
    assert(!IsRecoveryAttemptDue(&state, now + 9999));
    assert(IsRecoveryAttemptDue(&state, now + 10000));
    RecordRecoveryAttempt(&state, now + 10000);
    assert(!IsRecoveryAttemptDue(&state, now + 39999));
    assert(IsRecoveryAttemptDue(&state, now + 40000));
}

int main(void)
{
    TestRdpAndLocalLogin();
    TestStartupInRemoteSessionAndMissedNotifications();
    TestUnlockInterruptsBackoff();
    TestDisplayChangesDuringSettling();
    TestThirdPartyRemoteWithoutSessionEvents();
    TestResetDoesNotBypassDesktopWait();
    TestLongUptime();
    puts("Recovery tests passed (7 scenarios).");
    return 0;
}
