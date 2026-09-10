# NVIDIA App overlay recovery

The opt-in **Fix replay after RDP (restart overlay)** switch works around the
capture backend retaining an unusable state after an RDP session. The Instant
Replay ON registry flag cannot distinguish a healthy replay buffer from the
reported zero-second buffer, so this is an event-triggered workaround rather
than buffer-length detection.

The worker latches RDP connect/disconnect notifications for its own Windows
session. A live protocol query also detects startup in RDP or missing WTS
notifications. Enabling the option explicitly schedules one repair of an
already-affected session. Ordinary local startup, locking/unlocking, display
changes and settings reload do not schedule a repair. Disabling the option
cancels a pending repair.

Repair starts only after the normal physical-input confirmation, ten-second
settling period and pause/whitelist/exclusive checks. The worker suspends patch
protection before calling NVIDIA, then checks the current rules again immediately
before disabling the overlay. It completes the off/on operation and resets replay
observation so the deliberate stop is not mistaken for another capture failure.
Normal settling and control reload then run again before replay is resumed.

## Native adapter

`src/nvidia_overlay.c` uses the same versioned `nvspapi64.dll` interface used by
NVIDIA App's `NvIGOUtil.dll` for `ToggleIGO`. It neither writes NVIDIA registry
values directly nor kills NVIDIA processes. It reads the current user's
`IsShadowPlayEnabledUser` value and the API's current status; an already-disabled
overlay is preserved. The DLL is located from the 64-bit machine `NvApp\FullPath`
installation entry, with the standard Program Files location as a fallback.
DLL loading excludes the working directory and PATH.

This is an undocumented vendor ABI. The minimal adapter was checked against
NVIDIA App **11.0.9.251**, including a successful read-only call to the installed
DLL and a preflight through the production adapter that cancelled before any
mutating call. No NVIDIA binaries or SDK implementation are redistributed.

The 64-bit v1 ABI used here is:

| Operation | ABI |
| --- | --- |
| Factory | Export `CreateShadowPlayApiInterface`; `{ version=0x10018, interfaceVersion=0x10008, client=6, reserved=0, interface** }` |
| Release | Virtual slot 0; `{ version=0x10008, timeoutMs }` |
| Enable / disable | Virtual slots 3 / 4; `{ version=0x10010, timeoutMs, flags=0, origin=1 }` |
| Status | Virtual slot 5; `{ version=0x10010, enabled, userEnabled, serverState }`; the last three fields are outputs |

Methods return `HRESULT`. A running enabled overlay reports server state **3**.
Known not-running, idle and disabled backend states (1, 2 and 4) are also eligible
when both the registry and API still report the user's overlay preference as ON;
these can be the very states needing recovery after RDP. Unknown states and
invalid boolean outputs do not authorize a reset. The API checks the requested structure and
interface versions; missing exports, rejected versions and failed queries cause
the repair to be skipped and logged.

Client **6** is NVIDIA's `ShadowPlayApi_TestingTool` endpoint. Client 4 belongs to
the NVIDIA App UI: using it while the app is open causes an address collision and
a rejected message-bus join. The diagnostic endpoint allows this adapter and the
open NVIDIA App to coexist. If another diagnostic client owns that address, a
failed query is handled without changing the overlay.

Enable/disable requests supply a five-second timeout; release supplies one
second. These fields do not bound all vendor work: in particular, the status
query has no timeout field, and the vendor's initial message-bus join may wait
about thirty seconds before reporting an error. Work runs on the existing
recovery worker, not the window thread.

Once disable is attempted, enable is attempted even if disable returned an
error, since the backend might already have stopped. Enable can be retried once;
disable is never repeated in that repair. A pause, exit or session change during
the call does not cancel restoration of the original enabled setting. Further
Instant Replay commands still require a confirmed physical desktop. Failure to
restore produces a localized warning and requires fresh local input after the
user has manually re-enabled the overlay. A process crash or forced termination
during the off/on operation cannot guarantee restoration.

## Validation

`make test` exercises the adapter with fake registry, DLL and API boundaries,
including version/load/query failures, disabled preferences, cancellation before
mutation, partial disable, restore retries and cleanup. Worker integration tests
cover stale ON state, RDP startup/return, physical input, policy deferral, late
cancellation, post-reset settling and prevention of repeated resets.

For hardware validation, enable the switch, connect and disconnect RDP, then
sign in to the same account at the PC and use its physical keyboard or mouse.
After settling and the single overlay reset, verify that replay time grows above
zero, save a clip and play it. Repeat with the option off and with AlwaysShadow
paused. Resetting the overlay interrupts any current recording and discards its
unsaved replay buffer, just as toggling the NVIDIA setting manually does.
The automated tests do not establish driver-wide compatibility or reproduce an
actual RDP transition.
