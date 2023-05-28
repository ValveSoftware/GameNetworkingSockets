//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Include the relevant platform-specific headers for socket-related
// stuff, and declare some functions make them look as similar to
// plain BSD sockets as possible.
// 
// This includes a bunch of stuff.  DO NOT INCLUDE THIS FROM A HEADER
//
// Some things that will be defined by this file:
// 
// closesocket()
// GetLastSocketError()
// SetSocketNonBlocking()
// 
// USE_EPOLL or USE_POLL
// If USE_EPOLL:
//		EPollHandle, INVALID_EPOLL_HANDLE, EPollCreate()
//
// WAKE_THREAD_USING_EVENT or WAKE_THREAD_USING_SOCKET_PAIR
// If WAKE_THREAD_USING_EVENT:
//		ThreadWakeEvent, INVALID_THREAD_WAKE_EVENT, SetWakeThreadEvent()

#ifndef TIER0_PLATFORM_SOCKETS_H
#define TIER0_PLATFORM_SOCKETS_H
#pragma once

#include "platform.h"
#include <vstdlib/strtools.h>

// !KLUDGE!
typedef char SteamNetworkingErrMsg[ 1024 ];

// Socket headers
#ifdef _WIN32
	//#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#ifndef _XBOX_ONE
		#include <iphlpapi.h>
	#endif

	// Windows is the worst
	#undef min
	#undef max
	#undef SetPort

	#define MSG_NOSIGNAL 0

	inline bool SetSocketNonBlocking( SOCKET s )
	{
		unsigned long opt = 1;
		return ioctlsocket( s, FIONBIO, &opt ) == 0;
	}

	#define WAKE_THREAD_USING_EVENT
	typedef HANDLE ThreadWakeEvent;
	#define INVALID_THREAD_WAKE_EVENT INVALID_HANDLE_VALUE
	inline void SetWakeThreadEvent( ThreadWakeEvent hEvent )
	{
		::SetEvent( hEvent );
	}

	inline int GetLastSocketError()
	{
		return (int)WSAGetLastError();
	}

#elif IsNintendoSwitch()
	// NDA-protected material, so all this is in a separate file
	#include "platform_sockets_nswitch.h"
#elif IsPlaystation()
	// NDA-protected material, so all this is in a separate file
	#include "platform_sockets_playstation.h"
#elif IsPosix()

	// POSIX-ish platform (Linux, OSX, Android, IOS)
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <sys/ioctl.h>
	#include <unistd.h>
	#include <poll.h>
	#include <errno.h>

	#if !IsAndroid()
		#include <ifaddrs.h>
	#endif
	#include <net/if.h>

	#ifndef closesocket
		#define closesocket close
	#endif
	#define WSAEWOULDBLOCK EWOULDBLOCK

	#define WAKE_THREAD_USING_SOCKET_PAIR

	inline bool SetSocketNonBlocking( SOCKET s )
	{
		unsigned long opt = 1;
		return ioctl( s, FIONBIO, &opt ) == 0;
	}

	inline int GetLastSocketError()
	{
		return errno;
	}

	#ifdef __APPLE__
		#define USE_POLL
	#else
		#define USE_EPOLL
		#include <sys/epoll.h>

		typedef int EPollHandle;
		constexpr EPollHandle INVALID_EPOLL_HANDLE = -1;
		inline EPollHandle EPollCreate( SteamNetworkingErrMsg &errMsg )
		{
			int flags = 0;
			#if IsLinux()
				flags |= EPOLL_CLOEXEC;
			#endif
			EPollHandle e = epoll_create1( flags );
			if ( e == -1 )
			{
				V_sprintf_safe( errMsg, "epoll_create1() failed, errno=%d", errno );
				return INVALID_EPOLL_HANDLE;
			}
			return e;
		}

		#define EPollClose(x) close(x)

		// FIXME - should we try to use eventfd() here
		// instead of a socket pair?

	#endif
#else
	#error "How do?"
#endif

#endif // _H
