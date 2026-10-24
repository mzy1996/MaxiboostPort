/* The Windows build's compatibility with the code:
   - the headers of Windows define the ancient "near" and "far" keywords as macros, and the
     code uses "near" as a name;
   - <windows.h> brings the old WinSock, which the sockets of asio refuse, so the new one is
     taken in first. */
#pragma once

#ifdef _WIN32
#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif
#undef near
#undef far
#undef small
#undef ERROR
#endif
