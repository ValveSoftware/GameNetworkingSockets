//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_LOWLEVEL_H
#define STEAMNETWORKINGSOCKETS_LOWLEVEL_H
#ifdef _WIN32
#pragma once
#endif

#include <cstdint>
#include <steamnetworkingsockets/steamnetworkingtypes.h>
#include <steamnetworkingsockets/isteamnetworkingsockets.h>
#include "tier1/netadr.h"
#include "tier1/utlmap.h"
//#include <logger.h>
//#include <globals.h>

struct iovec;

namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Low level sockets
//
/////////////////////////////////////////////////////////////////////////////

/// Store the callback and its context together
class CRecvPacketCallback
{
public:
	/// Prototype of the callback
	typedef void (*FCallbackRecvPacket)( const void *pPkt, int cbPkt, const netadr_t &adrFrom, void *pContext );

	/// Default constructor sets stuff to null
	inline CRecvPacketCallback() : m_fnCallback( nullptr ), m_pContext( nullptr ) {}

	/// A template constructor so you can use type safe context and avoid messy casting
	template< typename T >
	inline CRecvPacketCallback( void (*fnCallback)( const void *pPkt, int cbPkt, const netadr_t &adrFrom, T context ), T context )
	: m_fnCallback ( reinterpret_cast< FCallbackRecvPacket>( fnCallback ) )
	, m_pContext( reinterpret_cast< void * >( context ) )
	{
		COMPILE_TIME_ASSERT( sizeof(T) == sizeof(void*) );
	}

	FCallbackRecvPacket m_fnCallback;
	void *m_pContext;

	/// Shortcut notation to execute the callback
	inline void operator()( const void *pPkt, int cbPkt, const netadr_t &adrFrom ) const
	{
		if ( m_fnCallback )
			m_fnCallback( pPkt, cbPkt, adrFrom, m_pContext );
	}
};

/// Interface object for a low-level Berkeley socket.  We always use non-blocking, UDP sockets.
class IRawUDPSocket
{
public:
	/// A thin wrapper around ::sendto
	///
	/// Packets sent through this method are subject to fake loss (steamdatagram_fakepacketloss_send)
	/// and fake lag (steamdatagram_fakepacketlag_send)
	bool BSendRawPacket( const void *pPkt, int cbPkt, const netadr_t &adrTo ) const;

	/// Gather-based send
	bool BSendRawPacketGather( int nChunks, const iovec *pChunks, const netadr_t &adrTo ) const;

	/// Logically close the socket.  This might not actually close the socket IMMEDIATELY,
	/// there may be a slight delay.  (On the order of a few milliseconds.)  But you will not
	/// get any further callbacks.
	void Close();

	/// The port we ended up binding to
	uint16 m_nPort;

protected:
	IRawUDPSocket();
	~IRawUDPSocket();
};

/// Create a UDP socket, set all the socket options for non-blocking, etc, bind it to the desired interface and port (or use 0
/// to bind to "any" interface and get an ephemeral port), and make sure we're setup to poll the socket efficiently and deliver
/// packets received to the specified callback.
extern IRawUDPSocket *OpenRawUDPSocket( uint32 nIP, uint16 nPort, CRecvPacketCallback callback, SteamDatagramErrMsg &errMsg );

/// A single socket could, in theory, be used to communicate with every single remote host.
/// Or we may decide to open up one socket per remote host, to workaround weird firewall/NAT
/// bugs.  A IBoundUDPSocket abstracts this.  If you need to talk to a single remote host
/// over UDP, you can get one of these and not worry about whether you got your own socket
/// or are sharing a socket.  And you don't need to worry about polling the socket.  You'll
/// just get your callback when a packet is received.
class IBoundUDPSocket
{
public:

	/// Send a packet on this socket to the bound remote host
	inline bool BSendRawPacket( const void *pPkt, int cbPkt ) const
	{
		return m_pRawSock->BSendRawPacket( pPkt, cbPkt, m_adr );
	}

	/// Gather-based send to the bound remote host
	inline bool BSendRawPacketGather( int nChunks, const iovec *pChunks ) const
	{
		return m_pRawSock->BSendRawPacketGather( nChunks, pChunks, m_adr );
	}

	/// Close this socket and stop talking to the specified remote host
	virtual void Close() = 0;

	/// Who are we talking to?
	const netadr_t &GetRemoteHostAddr() const { return m_adr; }

	/// Access the underlying socket we are using (which might be shared)
	IRawUDPSocket *GetRawSock() const { return m_pRawSock; }

protected:
	inline IBoundUDPSocket( IRawUDPSocket *pRawSock, const netadr_t &adr ) : m_adr( adr ), m_pRawSock( pRawSock ) {}
	inline ~IBoundUDPSocket() {}

	/// Address of remote host
	netadr_t m_adr;

	/// The raw socket that is being shared
	IRawUDPSocket *m_pRawSock;
};

/// Get a socket to talk to a single host.  The underlying socket won't be
/// shared with anybody else.
extern IBoundUDPSocket *OpenUDPSocketBoundToHost( uint32 nLocalIP, uint16 nLocalPort, const netadr_t &adrRemote, CRecvPacketCallback callback, SteamDatagramErrMsg &errMsg );

/// Create a pair of sockets that are bound to talk to each other.
extern bool CreateBoundSocketPair( CRecvPacketCallback callback1, CRecvPacketCallback callback2, IBoundUDPSocket **ppOutSockets, SteamDatagramErrMsg &errMsg );

/// Manage a single underlying socket that is used to talk to multiple remote hosts
class CSharedSocket
{
public:
	CSharedSocket();
	~CSharedSocket();

	/// Allocate a raw socket and setup bookkeeping structures so we can add
	/// clients that will talk using it.
	bool BInit( uint32 nIP, uint16 nPort, CRecvPacketCallback callbackDefault, SteamDatagramErrMsg &errMsg );

	/// Close all sockets and clean up all resources
	void Kill();

	/// Add a client to talk to a given remote address.  Use IBoundUDPSocket::Close when you
	/// are done.
	IBoundUDPSocket *AddRemoteHost( const netadr_t &adrRemote, CRecvPacketCallback callback );

	/// Send a packet to a remove host.  It doesn't matter if the remote host
	/// is in the client table a client already or not.
	bool BSendRawPacket( const void *pPkt, int cbPkt, const netadr_t &adrTo ) const
	{
		return m_pRawSock->BSendRawPacket( pPkt, cbPkt, adrTo );
	}

private:

	/// Call this if we get a packet from somebody we don't recognize
	CRecvPacketCallback m_callbackDefault;

	/// The raw socket that is being shared
	IRawUDPSocket *m_pRawSock;

	class RemoteHost : public IBoundUDPSocket
	{
	private:
		friend class CSharedSocket;
		inline virtual ~RemoteHost() {}
	public:
		inline RemoteHost( IRawUDPSocket *pRawSock, const netadr_t &adr ) : IBoundUDPSocket( pRawSock, adr ) {}
		CRecvPacketCallback m_callback;
		CSharedSocket *m_pOwner;
		virtual void Close() OVERRIDE;
	};
	friend class RemoteHost;

	/// List of remote hosts we're talking to.  It's sort of silly to use a map,
	/// which duplicates the address in the key as well as a member of the
	/// RemoteHost.
	/// Perhaps a better approach would be to use an RBTree, but then we'd
	/// need to be able to search the tree given an address, and RBTRee class
	/// doesn't have that interface yet.  Also, it's probably better to
	/// waste a tiny bit of space and put the keys close together in memory,
	/// anyway.
	CUtlOrderedMap<netadr_t, RemoteHost *> m_mapRemoteHosts;

	void CloseRemoteHostByIndex( int idx );

	static void CallbackRecvPacket( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSharedSocket *pSock );
};

/////////////////////////////////////////////////////////////////////////////
//
// Periodic processing
//
/////////////////////////////////////////////////////////////////////////////

const SteamNetworkingMicroseconds k_nThinkTime_Never = INT64_MAX;
class ThinkerSetIndex;

class IThinker
{
public:
	virtual ~IThinker();

	/// Callback to do whatever periodic processing you need.  If you don't
	/// explicitly call SetNextThinkTime inside this function, then thinking
	/// will be disabled.
	///
	/// Think callbacks will always happen from the service thread,
	/// with the lock held.
	///
	/// Note that we assume a limited precision of the thread scheduler,
	/// and you won't get your callback exactly when you request.
	virtual void Think( SteamNetworkingMicroseconds usecNow ) = 0;

	/// Called to set when you next want to get your Think() callback.
	///
	/// nSlackMS indicates whether you want your callback a bit early or
	/// a bit late, and exactly how much earlier or later you are willing
	/// to accept.  A negative number means you are willing to be woken up
	/// at least N milliseconds early, but would prefer to not be woken up late.
	/// A positive number means that you are willing to be woken up late, but
	/// not early.
	///
	/// You must accept at least 1ms of slack in one direction or the other.  2ms
	/// is better.
	void SetNextThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime, int nSlackMS = +2 );

	/// Adjust schedule time to the earlier of the current schedule time,
	/// or the given time.
	void EnsureMinThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime, int nSlackMS = +2 );

	/// Clear the next think time.  You won't get a callback.
	void ClearNextThinkTime() { SetNextThinkTime( k_nThinkTime_Never, 0 ); }

	/// Request an immediate wakeup.
	void SetNextThinkTimeASAP() { SetNextThinkTime( 1, +1 ); }

	/// Fetch time when the next Think() call is currently scheduled to
	/// happen.
	inline SteamNetworkingMicroseconds GetTargetThinkTime() const { return m_usecNextThinkTimeTarget; }

	/// Get earliest time when we want to be woken up
	inline SteamNetworkingMicroseconds GetEarliestThinkTime() const { return m_usecNextThinkTimeEarliest; }

	/// Get latest time when we want to be woken up
	inline SteamNetworkingMicroseconds GetLatestThinkTime() const { return m_usecNextThinkTimeLatest; }

	/// Return true if we are scheduled to get our callback
	inline bool IsScheduled() const { return m_usecNextThinkTimeTarget != k_nThinkTime_Never; }

protected:
	IThinker();

private:
	SteamNetworkingMicroseconds m_usecNextThinkTimeTarget;
	SteamNetworkingMicroseconds m_usecNextThinkTimeLatest;
	SteamNetworkingMicroseconds m_usecNextThinkTimeEarliest;
	int m_queueIndex;
	friend class ThinkerSetIndex;
};

/////////////////////////////////////////////////////////////////////////////
//
// Misc low level service thread stuff
//
/////////////////////////////////////////////////////////////////////////////

/// Flag to signal that we want to be active.  If this is false, either
/// we haven't activated a service that needs the service thread, or
/// we've failed to initialize, or we're shutting down.
extern volatile bool g_bWantThreadRunning;
extern volatile bool g_bThreadInMainThread;

/// If running in main thread pump the thread
extern void CallDatagramThreadProc();

/// Called when we know it's safe to actually destroy sockets pending deletion.
/// This is when: 1.) We own the lock and 2.) we aren't polling in the service thread.
extern void ProcessPendingDestroyClosedRawUDPSockets();

/// Last time that we spewed something that was subject to rate limit 
extern SteamNetworkingMicroseconds g_usecLastRateLimitSpew;

/// Check for rate limiting spew (e.g. when spew could be triggered by malicious sender.)
inline bool BRateLimitSpew( SteamNetworkingMicroseconds usecNow )
{
	if ( usecNow < g_usecLastRateLimitSpew + 100000 )
		return false;
	g_usecLastRateLimitSpew = usecNow;
	return true;
}

extern ESteamNetworkingSocketsDebugOutputType g_eSteamDatagramDebugOutputDetailLevel;
extern void ReallySpewType( ESteamNetworkingSocketsDebugOutputType eType, PRINTF_FORMAT_STRING const char *pMsg, ... ) FMTFUNCTION( 2, 3 );
#define SpewType( eType, ... ) ( ( (eType) <= g_eSteamDatagramDebugOutputDetailLevel ) ? ReallySpewType( ESteamNetworkingSocketsDebugOutputType(eType), __VA_ARGS__ ) : (void)0 )
#define SpewMsg( ... ) SpewType( k_ESteamNetworkingSocketsDebugOutputType_Msg, __VA_ARGS__ )
#define SpewVerbose( ... ) SpewType( k_ESteamNetworkingSocketsDebugOutputType_Verbose, __VA_ARGS__ )
#define SpewDebug( ... ) SpewType( k_ESteamNetworkingSocketsDebugOutputType_Debug, __VA_ARGS__ )
#define SpewImportant( ... ) SpewType( k_ESteamNetworkingSocketsDebugOutputType_Important, __VA_ARGS__ )
#define SpewWarning( ... ) SpewType( k_ESteamNetworkingSocketsDebugOutputType_Warning, __VA_ARGS__ )
#define SpewError( ... ) SpewType( k_ESteamNetworkingSocketsDebugOutputType_Error, __VA_ARGS__ )
#define SpewBug( ... ) SpewType( k_ESteamNetworkingSocketsDebugOutputType_Bug, __VA_ARGS__ )

#define SpewTypeRateLimited( usecNow, eType, ... ) ( ( (eType) <= g_eSteamDatagramDebugOutputDetailLevel && BRateLimitSpew( usecNow ) ) ? ReallySpewType( (eType), __VA_ARGS__ ) : (void)0 )
#define SpewMsgRateLimited( usecNow, ... ) SpewTypeRateLimited( usecNow, k_ESteamNetworkingSocketsDebugOutputType_Msg, __VA_ARGS__ )
#define SpewWarningRateLimited( usecNow, ... ) SpewTypeRateLimited( usecNow, k_ESteamNetworkingSocketsDebugOutputType_Warning, __VA_ARGS__ )
#define SpewErrorRateLimited( usecNow, ... ) SpewTypeRateLimited( usecNow, k_ESteamNetworkingSocketsDebugOutputType_Error, __VA_ARGS__ )
#define SpewBugRateLimited( usecNow, ... ) SpewTypeRateLimited( usecNow, k_ESteamNetworkingSocketsDebugOutputType_Bug, __VA_ARGS__ )

/// Make sure stuff is initialized
extern bool BSteamNetworkingSocketsInitCommon( SteamDatagramErrMsg &errMsg );

/// Nuke common stuff
extern void SteamNetworkingSocketsKillCommon();

/// Scope lock object used to synchronize access to internal data structures.  We use a global lock,
/// even though in some cases it might not be necessary, to simplify the code, since in most cases
/// there will be very little contention and the should be held only for a short amount of time.
struct SteamDatagramTransportLock
{
	inline SteamDatagramTransportLock() { Lock(); }
	inline ~SteamDatagramTransportLock() { Unlock(); }
	static void Lock();
	static void Unlock();
	static void OnLocked();
	static void AssertHeldByCurrentThread();
	static volatile int s_nLocked;
};

} // namespace SteamNetworkingSocketsLib

extern "C" {
/// Fetch current time
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMicroseconds SteamNetworkingSockets_GetLocalTimestamp();
}

#endif // STEAMNETWORKINGSOCKETS_LOWLEVEL_H
