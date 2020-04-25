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
#define ICESESSION_INTERFACE_VERSION	1


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
	// Called during initialization to get configuration
	//
	virtual EICERole GetRole() { return k_EICERole_Unknown; }

	// Return any STUN servers that should be used (default is stun:stun.l.google.com:19302)
	virtual int GetNumStunServers() { return 0; }
	virtual const char *GetStunServer( int iIndex ) { return nullptr; }

	// Return any TURN servers that should be used
	virtual int GetNumTurnServers() { return 0; }
	virtual const char *GetTurnServer( int iIndex ) { return nullptr; }
	virtual const char *GetTurnServerUsername() { return nullptr; }
	virtual const char *GetTurnServerPassword() { return nullptr; }

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
	virtual bool BAddRemoteIceCandidate( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) = 0;
	virtual bool BSendData( const void *pData, size_t nSize ) = 0;
};

/// Factory function prototype.  How you get this factory will depend on how you are linking with
/// this code.
typedef IICESession *( *CreateICESession_t )( IICESessionDelegate *pDelegate, int nInterfaceVersion );
