#ifndef ALWAYS_SHADOW_STARTUP_H
#define ALWAYS_SHADOW_STARTUP_H
#include <windows.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
BOOL StartupIsElevated(void);
BOOL StartupTaskExists(void);
BOOL StartupSetTask(BOOL enabled, wchar_t *error, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
