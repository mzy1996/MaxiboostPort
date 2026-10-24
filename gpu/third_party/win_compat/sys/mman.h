/* The Windows build's stand-in for <sys/mman.h>: the page protection goes through the
   host services (gpu/shim/host_services.h), which use the Windows API. */
#pragma once
#include <stddef.h>
#include <host_services.h>
