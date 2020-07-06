//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================
//
// Simplified interface to a ICE session
//
// This wrapper is designed to provide the ICE peer connection functionality in a separate
// shared library to encapsulate the build environment and C++ runtime requirements of WebRTC.
//
//==================================================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>


//-----------------------------------------------------------------------------
// Increment this if the delegate classes below change interfaces
//-----------------------------------------------------------------------------
#define ICESESSION_INTERFACE_VERSION	3


enum EICERole
{
	k_EICERole_Controlling, // usually the "client" who initiated the connection
	k_EICERole_Controlled, // usually the "server" who accepted the connection
	k_EICERole_Unknown,
};

enum EICECandidateType : int
{
	k_EICECandidate_Invalid = 0,

	k_EICECandidate_IPv4_Relay = 0x01,
	k_EICECandidate_IPv4_HostPrivate = 0x02,
	k_EICECandidate_IPv4_HostPublic = 0x04,
	k_EICECandidate_IPv4_Reflexive = 0x08,

	k_EICECandidate_IPv6_Relay = 0x100,
	k_EICECandidate_IPv6_HostPrivate_Unsupported = 0x200, // NOTE: Not currently used.  All IPv6 addresses (even fc00::/7) are considered "public"
	k_EICECandidate_IPv6_HostPublic = 0x400,
	k_EICECandidate_IPv6_Reflexive = 0x800,
};

enum EProtocolType {
	k_EProtocolTypeUDP,
	k_EProtocolTypeTCP,
	k_EProtocolTypeSSLTCP,  // Pseudo-TLS.
	k_EProtocolTypeTLS,
};

constexpr int k_EICECandidate_Any_Relay = k_EICECandidate_IPv4_Relay | k_EICECandidate_IPv6_Relay;
constexpr int k_EICECandidate_Any_HostPrivate = k_EICECandidate_IPv4_HostPrivate | k_EICECandidate_IPv6_HostPrivate_Unsupported;
constexpr int k_EICECandidate_Any_HostPublic = k_EICECandidate_IPv4_HostPublic | k_EICECandidate_IPv6_HostPublic;
constexpr int k_EICECandidate_Any_Reflexive = k_EICECandidate_IPv4_Reflexive | k_EICECandidate_IPv6_Reflexive;
constexpr int k_EICECandidate_Any_IPv4 = 0x00ff;
constexpr int k_EICECandidate_Any_IPv6 = 0xff00;
constexpr int k_EICECandidate_Any = 0xffff;

/// You implement this class, which will receive callbacks frim the ICE session
class IICESessionDelegate
{
public:
	enum ELogPriority
	{
		k_ELogPriorityDebug,
		k_ELogPriorityVerbose,
		k_ELogPriorityInfo,
		k_ELogPriorityWarning,
		k_ELogPriorityError
	};

public:
	virtual void Log( ELogPriority ePriority, const char *pszMessageFormat, ... ) = 0;

	//
	// Callbacks that happen during operation
	//

	// Called when a local ICE candidate becomes available
	virtual void OnLocalCandidateGathered( EICECandidateType eType, const char *pszCandidate ) { }

	// Called when the writable state changes.
	// Use IICESession::IsWritable to get current state, and IICESession::GetPing for a RTT estimate
	virtual void OnWritableStateChanged() { }

	// Called when data is received on the data channel
	virtual void OnData( const void *pData, size_t nSize ) { }

	// Called when the route has changed.  Use GetRoute to get more info
	virtual void OnRouteChanged() { }
};

/// An ICE session with a peer
class IICESession
{
public:
	virtual void Destroy() = 0;

	/// Return true if it looks like we are connected and we think you could send data
	virtual bool GetWritableState() = 0;

	/// Get RTT estimate, in ms.  Returns -1 if we don't know
	virtual int GetPing() = 0;

	/// Return the route being used.
	typedef char CandidateAddressString[64];
	virtual bool GetRoute( EICECandidateType &eLocalCandidate, EICECandidateType &eRemoteCandidate, CandidateAddressString &szRemoteAddress ) = 0;

	/// Set credentials of the peer
	virtual void SetRemoteAuth( const char *pszUserFrag, const char *pszPwd ) = 0;

	/// Called when we get a signal with a candidate of the other guy,
	/// Returns the type of the candidate, or k_EICECandidate_Invalid if we failed
	virtual EICECandidateType AddRemoteIceCandidate( const char *pszCandidate ) = 0;

	/// Send a datagram to the peer.  Returns false if we know that we failed
	virtual bool BSendData( const void *pData, size_t nSize ) = 0;

	// !TEST! ETW callbacks
	virtual void SetWriteEvent_setsockopt( void (*fn)( int slevel, int sopt, int value ) ) = 0;
	virtual void SetWriteEvent_send( void (*fn)( int length ) ) = 0;
	virtual void SetWriteEvent_sendto( void (*fn)( void *addr, int length ) ) = 0;
};

struct ICESessionConfig
{
	typedef const char *StunServer;
	struct TurnServer
	{
		const char *m_pszHost = nullptr;
		const char *m_pszUsername = nullptr;
		const char *m_pszPwd = nullptr;
		const EProtocolType m_protocolType = k_EProtocolTypeUDP;
	};

	EICERole m_eRole = k_EICERole_Unknown;
	int m_nStunServers = 0;
	const StunServer *m_pStunServers = nullptr;
	int m_nTurnServers = 0;
	const TurnServer *m_pTurnServers = nullptr;
	int m_nCandidateTypes = k_EICECandidate_Any;
	const char *m_pszLocalUserFrag;
	const char *m_pszLocalPwd;
};

/// Factory function prototype.  How you get this factory will depend on how you are linking with
/// this code.
typedef IICESession *( *CreateICESession_t )( const ICESessionConfig &cfg, IICESessionDelegate *pDelegate, int nInterfaceVersion );
