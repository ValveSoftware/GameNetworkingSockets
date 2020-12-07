//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Include the relevant platform-specific headers for socket-related stuff,
// and declare some functions make them look similar
//

#ifndef STEAMNETWORKINGSOCKETS_PLATFORM_H
#define STEAMNETWORKINGSOCKETS_PLATFORM_H
#pragma once

// Socket headers
#ifdef _WIN32
	//#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <iphlpapi.h>
	#define MSG_NOSIGNAL 0
	#undef SetPort
#elif defined( NN_NINTENDO_SDK )
	// Sorry, but this code is covered under NDA with Nintendo, and
	// we don't have permission to distribute it.
#else
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <sys/ioctl.h>
	#include <unistd.h>
	#include <poll.h>
	#include <errno.h>

	#include <sys/types.h>
	#ifndef ANDROID
		#include <ifaddrs.h>
	#endif
	#include <sys/ioctl.h>
	#include <net/if.h>

	#ifndef closesocket
		#define closesocket close
	#endif
	#ifndef ioctlsocket
		#define ioctlsocket ioctl
	#endif
	#define WSAEWOULDBLOCK EWOULDBLOCK
#endif

// Windows is the worst
#undef min
#undef max

inline int GetLastSocketError()
{
	#if defined( _WIN32 )
		return (int)WSAGetLastError();
	#elif defined( NN_NINTENDO_SDK )
		// Sorry, but this code is covered under NDA with Nintendo, and
		// we don't have permission to distribute it.
	#else
		return errno;
	#endif
}

#endif // #ifndef STEAMNETWORKINGSOCKETS_PLATFORM_H
