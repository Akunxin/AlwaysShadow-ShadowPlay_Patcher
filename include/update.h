#ifndef UPDATE_H
#define UPDATE_H

#include <windows.h>

// FALSE means the check failed; available is always cleared on failure.
BOOL CheckForReleaseUpdate(const char *currentVersion, BOOL *available);

#endif
