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
#define ICESESSION_INTERFACE_VERSION	2


enum EICERole
{
	k_EICERole_Controlling, // usually the "client" who initiated the connection
	k_EICERole_Controlled, // usually the "server" who accepted the connection
	k_EICERole_Unknown,
};

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

	// Called when an ICE candidate becomes available
	virtual void OnIceCandidateAdded( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) { }

	// Called when the writable state changes.
	// Use IICESession::IsWritable to get current state
	virtual void OnWritableStateChanged() { }

	// Called when data is received on the data channel
	virtual void OnData( const void *pData, size_t nSize ) { }
};

/// An ICE session with a peer
class IICESession
{
public:
	virtual void Destroy() = 0;
	virtual bool GetWritableState() = 0;
	virtual void SetRemoteAuth( const char *pszUserFrag, const char *pszPwd ) = 0;
	virtual bool BAddRemoteIceCandidate( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) = 0;
	virtual bool BSendData( const void *pData, size_t nSize ) = 0;
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
	};

	EICERole m_eRole = k_EICERole_Unknown;
	int m_nStunServers = 0;
	const StunServer *m_pStunServers = nullptr;
	int m_nTurnServers = 0;
	const TurnServer *m_pTurnServers = nullptr;
	const char *m_pszLocalUserFrag;
	const char *m_pszLocalPwd;
};

/// Factory function prototype.  How you get this factory will depend on how you are linking with
/// this code.
typedef IICESession *( *CreateICESession_t )( const ICESessionConfig &cfg, IICESessionDelegate *pDelegate, int nInterfaceVersion );
