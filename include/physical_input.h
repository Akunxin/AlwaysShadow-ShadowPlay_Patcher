#ifndef PHYSICAL_INPUT_H
#define PHYSICAL_INPUT_H

#include <windows.h>

// Register on the window thread before starting the worker. No key values or
// mouse coordinates are retained; only evidence of local hardware input is used.
BOOL PhysicalInputInitialize(HWND window, HANDLE wakeEvent);
void PhysicalInputShutdown(void);
void PhysicalInputHandle(HRAWINPUT input);
void PhysicalInputForgetDevices(void);

// Thread-safe. A remote/locked session or failed replay attempt revokes the
// confirmation; only subsequent input from local hardware can restore it.
void PhysicalInputRequireConfirmation(void);
BOOL PhysicalInputIsConfirmed(void);

#endif
