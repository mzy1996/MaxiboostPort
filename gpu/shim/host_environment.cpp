// SPDX-License-Identifier: GPL-2.0-or-later
// The host services both builds share (gpu/shim/host_services.h).
#include "host_services.h"

void HostSetEnv(const char* name, const char* value, int overwrite) {
    if (!name || !value) {
        return;
    }
#ifdef _WIN32
    // SetEnvironmentVariable has no "do not overwrite" switch, so the variable is looked
    // up first.
    if (!overwrite) {
        char existing[1024] = {};
        const DWORD length = GetEnvironmentVariableA(name, existing, sizeof(existing));
        if (length > 0 && length < sizeof(existing)) {
            return;
        }
    }
    SetEnvironmentVariableA(name, value);
#else
    setenv(name, value, overwrite);
#endif
}
