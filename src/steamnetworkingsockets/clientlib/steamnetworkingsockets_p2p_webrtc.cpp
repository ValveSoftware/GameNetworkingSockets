//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p_webrtc.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC

extern "C" {
CreateICESession_t g_SteamNetworkingSockets_CreateICESessionFunc = nullptr;
}

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportP2PICE_WebRTC
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PICE_WebRTC::CConnectionTransportP2PICE_WebRTC( CSteamNetworkConnectionP2P &connection )
: CConnectionTransportP2PICE( connection )
, m_pICESession( nullptr )
, m_mutexPacketQueue( "ice_packet_queue" )
{
}

CConnectionTransportP2PICE_WebRTC::~CConnectionTransportP2PICE_WebRTC()
{
	Assert( !m_pICESession );
}

void CConnectionTransportP2PICE_WebRTC::TransportFreeResources()
{
	if ( m_pICESession )
	{
		m_pICESession->Destroy();
		m_pICESession = nullptr;
	}

	CConnectionTransport::TransportFreeResources();
}

void CConnectionTransportP2PICE_WebRTC::Init( const ICESessionConfig &cfg )
{
	AssertLocksHeldByCurrentThread( "P2PICE::Init" );

	if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
	{
		Connection().ICEFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "CreateICESession factory not set" );
		return;
	}

	SteamNetworkingGlobalLock::SetLongLockWarningThresholdMS( "CConnectionTransportP2PICE::Init", 50 );

	// Create the session
	m_pICESession = (*g_SteamNetworkingSockets_CreateICESessionFunc)( cfg, this, ICESESSION_INTERFACE_VERSION );
	if ( !m_pICESession )
	{
		Connection().ICEFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "CreateICESession failed" );
		return;
	}
}

bool CConnectionTransportP2PICE_WebRTC::SendPacket( const void *pkt, int cbPkt )
{
	if ( !m_pICESession )
		return false;

	ETW_ICESendPacket( m_connection.m_hConnectionSelf, cbPkt );
	if ( !m_pICESession->BSendData( pkt, cbPkt ) )
		return false;

	// Update stats
	m_connection.m_statsEndToEnd.TrackSentPacket( cbPkt );
	return true;
}

bool CConnectionTransportP2PICE_WebRTC::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
	if ( nChunks == 1 )
	{
		Assert( (int)pChunks->iov_len == cbSendTotal );
		return SendPacket( pChunks->iov_base, pChunks->iov_len );
	}
	if ( cbSendTotal > k_cbSteamNetworkingSocketsMaxUDPMsgLen )
	{
		Assert( false );
		return false;
	}
	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	uint8 *p = pkt;
	while ( nChunks > 0 )
	{
		if ( p + pChunks->iov_len > pkt+cbSendTotal )
		{
			Assert( false );
			return false;
		}
		memcpy( p, pChunks->iov_base, pChunks->iov_len );
		p += pChunks->iov_len;
		--nChunks;
		++pChunks;
	}
	Assert( p == pkt+cbSendTotal );
	return SendPacket( pkt, p-pkt );
}

bool CConnectionTransportP2PICE_WebRTC::BCanSendEndToEndData() const
{
	if ( !m_pICESession )
		return false;
	if ( !m_pICESession->GetWritableState() )
		return false;
	return true;
}

void CConnectionTransportP2PICE_WebRTC::UpdateRoute()
{
	if ( !m_pICESession )
		return;

	AssertLocksHeldByCurrentThread( "P2PICE::UpdateRoute" );

	// Clear ping data, it is no longer accurate
	m_pingEndToEnd.Reset();

	IICESession::CandidateAddressString szRemoteAddress;
	EICECandidateType eLocalCandidate, eRemoteCandidate;
	if ( !m_pICESession->GetRoute( eLocalCandidate, eRemoteCandidate, szRemoteAddress ) )
	{
		SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is unkown\n", ConnectionDescription() );
		m_eCurrentRouteKind = k_ESteamNetTransport_Unknown;
		m_currentRouteRemoteAddress.Clear();
	}
	else
	{
		if ( !m_currentRouteRemoteAddress.ParseString( szRemoteAddress ) )
		{
			AssertMsg1( false, "IICESession::GetRoute returned invalid remote address '%s'!", szRemoteAddress );
			m_currentRouteRemoteAddress.Clear();
		}

		netadr_t netadrRemote;
		SteamNetworkingIPAddrToNetAdr( netadrRemote, m_currentRouteRemoteAddress );

		if ( ( eLocalCandidate | eRemoteCandidate ) & k_EICECandidate_Any_Relay )
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_TURN;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is via TURN to %s\n", ConnectionDescription(), szRemoteAddress );
		}
		else if ( netadrRemote.IsValid() && IsRouteToAddressProbablyLocal( netadrRemote ) )
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_UDPProbablyLocal;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route proably local to %s (based on remote address)\n", ConnectionDescription(), szRemoteAddress );
		}
		else if ( ( eLocalCandidate & k_EICECandidate_Any_HostPrivate ) && ( eRemoteCandidate & k_EICECandidate_Any_HostPrivate ) )
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_UDPProbablyLocal;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is probably local to %s (based on candidate types both being private addresses)\n", ConnectionDescription(), szRemoteAddress );
		}
		else
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_UDP;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is public UDP to %s\n", ConnectionDescription(), szRemoteAddress );
		}
	}

	RouteOrWritableStateChanged();
}

void CConnectionTransportP2PICE_WebRTC::RouteOrWritableStateChanged()
{

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Go ahead and add a ping sample from our RTT estimate if we don't have any other data
	if ( m_pingEndToEnd.m_nSmoothedPing < 0 )
	{
		int nPing = m_pICESession->GetPing();
		if ( nPing >= 0 )
			m_pingEndToEnd.ReceivedPing( nPing, usecNow );
		else
			P2PTransportEndToEndConnectivityNotConfirmed( usecNow );
	}

	Connection().TransportEndToEndConnectivityChanged( this, usecNow );
}

void CConnectionTransportP2PICE_WebRTC::RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	AssertLocksHeldByCurrentThread( "P2PICE::RecvRendezvous" );

	// Safety
	if ( !m_pICESession )
	{
		Connection().ICEFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "No IICESession?" );
		return;
	}

	if ( msg.has_add_candidate() )
	{
		const CMsgICECandidate &c = msg.add_candidate();
		EICECandidateType eType = m_pICESession->AddRemoteIceCandidate( c.candidate().c_str() );
		if ( eType != k_EICECandidate_Invalid )
		{
			SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Processed remote Ice Candidate '%s' (type %d)\n", ConnectionDescription(), c.candidate().c_str(), eType );
			Connection().m_msgICESessionSummary.set_remote_candidate_types( Connection().m_msgICESessionSummary.remote_candidate_types() | eType );
		}
		else
		{
			SpewWarning( "[%s] Ignoring candidate %s\n", ConnectionDescription(), c.ShortDebugString().c_str() );
		}
	}

	if ( msg.has_auth() )
	{
		std::string sUfragRemote = Base64EncodeLower30Bits( ConnectionIDRemote() );
		const char *pszPwdFrag = msg.auth().pwd_frag().c_str();
		SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Set remote auth to %s / %s\n", ConnectionDescription(), sUfragRemote.c_str(), pszPwdFrag );
		m_pICESession->SetRemoteAuth( sUfragRemote.c_str(), pszPwdFrag );
	}
}

void CConnectionTransportP2PICE_WebRTC::P2PTransportThink( SteamNetworkingMicroseconds usecNow )
{
	// Are we dead?
	if ( !m_pICESession || Connection().m_pTransportICEPendingDelete )
	{
		// If we're a zombie, we should be queued for destruction
		Assert( Connection().m_pTransportICE != this );
		Assert( Connection().m_pTransportICEPendingDelete == this );

		// Make sure connection wakes up to do this
		Connection().SetNextThinkTimeASAP();
		return;
	}

	CConnectionTransportP2PICE::P2PTransportThink( usecNow );
}

/////////////////////////////////////////////////////////////////////////////
//
// IICESessionDelegate handlers
//
// NOTE: These can be invoked from any thread,
// and we won't hold the lock
//
/////////////////////////////////////////////////////////////////////////////

/// A glue object used to take a callback from ICE, which might happen in
/// any thread, and execute it with the proper locks.
class IConnectionTransportP2PICERunWithLock : private CQueuedTaskOnTarget<CConnectionTransportP2PICE_WebRTC>
{
public:

	/// Execute the callback.  The global lock and connection locks will be held.
	virtual void RunTransportP2PICE( CConnectionTransportP2PICE_WebRTC *pTransport ) = 0;

	inline void Queue( CConnectionTransportP2PICE_WebRTC *pTransport, const char *pszTag )
	{
		if ( Setup( pTransport ) )
			QueueToRunWithGlobalLock( pszTag );
	}

	inline void RunOrQueue( CConnectionTransportP2PICE_WebRTC *pTransport, const char *pszTag )
	{
		if ( Setup( pTransport ) )
			RunWithGlobalLockOrQueue( pszTag );
	}

private:
	inline bool Setup( CConnectionTransportP2PICE_WebRTC *pTransport )
	{
		CSteamNetworkConnectionP2P &conn = pTransport->Connection();
		if ( conn.m_pTransportICE != pTransport )
		{
			delete this;
			return false;
		}

		SetTarget( pTransport );
		return true;
	}

	virtual void Run()
	{
		CConnectionTransportP2PICE_WebRTC *pTransport = Target();
		CSteamNetworkConnectionP2P &conn = pTransport->Connection();

		ConnectionScopeLock connectionLock( conn );
		if ( conn.m_pTransportICE != pTransport )
			return;

		RunTransportP2PICE( pTransport );
	}
};


void CConnectionTransportP2PICE_WebRTC::Log( IICESessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... )
{
	ESteamNetworkingSocketsDebugOutputType eType;
	switch ( ePriority )
	{
		default:	
			AssertMsg1( false, "Unknown priority %d", ePriority );
			// FALLTHROUGH

		case IICESessionDelegate::k_ELogPriorityDebug: eType = k_ESteamNetworkingSocketsDebugOutputType_Debug; break;
		case IICESessionDelegate::k_ELogPriorityVerbose: eType = k_ESteamNetworkingSocketsDebugOutputType_Verbose; break;
		case IICESessionDelegate::k_ELogPriorityInfo: eType = k_ESteamNetworkingSocketsDebugOutputType_Msg; break;
		case IICESessionDelegate::k_ELogPriorityWarning: eType = k_ESteamNetworkingSocketsDebugOutputType_Warning; break;
		case IICESessionDelegate::k_ELogPriorityError: eType = k_ESteamNetworkingSocketsDebugOutputType_Error; break;
	}

	if ( eType > Connection().LogLevel_P2PRendezvous() )
		return;

	char buf[ 1024 ];
	va_list ap;
	va_start( ap, pszMessageFormat );
	V_vsprintf_safe( buf, pszMessageFormat, ap );
	va_end( ap );

	//ReallySpewType( eType, "[%s] ICE: %s", ConnectionDescription(), buf );
	ReallySpewTypeFmt( eType, "ICE: %s", buf ); // FIXME would like to get the connection description, but that's not threadsafe
}

void CConnectionTransportP2PICE_WebRTC::OnLocalCandidateGathered( EICECandidateType eType, const char *pszCandidate )
{

	struct RunIceCandidateAdded : IConnectionTransportP2PICERunWithLock
	{
		EICECandidateType eType;
		CMsgICECandidate msgCandidate;
		virtual void RunTransportP2PICE( CConnectionTransportP2PICE_WebRTC *pTransport )
		{
			pTransport->LocalCandidateGathered( eType, std::move( msgCandidate ) );
		}
	};

	RunIceCandidateAdded *pRun = new RunIceCandidateAdded;
	pRun->eType = eType;
	pRun->msgCandidate.set_candidate( pszCandidate );

	// Always use the queue here.  Never run immediately, even if we
	// can get the lock.  This is not time sensitive, and deadlocks
	// are a possibility with this type of event.
	pRun->Queue( this, "ICE OnIceCandidateAdded" );
}

void CConnectionTransportP2PICE_WebRTC::DrainPacketQueue( SteamNetworkingMicroseconds usecNow )
{
	// Quickly swap into temp
	CUtlBuffer buf;
	m_mutexPacketQueue.lock();
	buf.Swap( m_bufPacketQueue );
	m_mutexPacketQueue.unlock();

	//SpewMsg( "CConnectionTransportP2PICE_WebRTC::DrainPacketQueue: %d bytes queued\n", buf.TellPut() );

	// Process all the queued packets
	uint8 *p = (uint8*)buf.Base();
	uint8 *end = p + buf.TellPut();
	while ( p < end && Connection().m_pTransportICE == this )
	{
		if ( p+sizeof(int) > end )
		{
			Assert(false);
			break;
		}
		int cbPkt = *(int*)p;
		p += sizeof(int);
		if ( p + cbPkt > end )
		{
			// BUG!
			Assert(false);
			break;
		}
		ProcessPacket( p, cbPkt, usecNow );
		p += cbPkt;
	}
}

void CConnectionTransportP2PICE_WebRTC::OnWritableStateChanged()
{
	struct RunWritableStateChanged : IConnectionTransportP2PICERunWithLock
	{
		virtual void RunTransportP2PICE( CConnectionTransportP2PICE_WebRTC *pTransport )
		{
			// Are we writable right now?
			if ( pTransport->BCanSendEndToEndData() )
			{

				// Just spew
				SpewMsgGroup( pTransport->LogLevel_P2PRendezvous(), "[%s] ICE reports we are writable\n", pTransport->ConnectionDescription() );

				// Re-calculate some stuff if this is news
				if ( pTransport->m_bNeedToConfirmEndToEndConnectivity )
					pTransport->RouteOrWritableStateChanged();
			}
			else
			{

				// We're not writable.  Is this news to us?
				if ( !pTransport->m_bNeedToConfirmEndToEndConnectivity )
				{

					// We thought we were good.  Clear flag, we are in doubt
					SpewMsgGroup( pTransport->LogLevel_P2PRendezvous(), "[%s] ICE reports we are no longer writable\n", pTransport->ConnectionDescription() );
					pTransport->P2PTransportEndToEndConnectivityNotConfirmed( SteamNetworkingSockets_GetLocalTimestamp() );
				}
			}
		}
	};

	// Always use the queue here.  Never run immediately, even if we
	// can get the lock.  This is not time sensitive, and deadlocks
	// are a possibility with this type of event.
	RunWritableStateChanged *pRun = new RunWritableStateChanged;
	pRun->Queue( this, "ICE OnWritableStateChanged" );
}

void CConnectionTransportP2PICE_WebRTC::OnRouteChanged()
{
	struct RunRouteStateChanged : IConnectionTransportP2PICERunWithLock
	{
		virtual void RunTransportP2PICE( CConnectionTransportP2PICE_WebRTC *pTransport )
		{
			pTransport->UpdateRoute();
		}
	};

	// Always use the queue here.  Never run immediately, even if we
	// can get the lock.  This is not time sensitive, and deadlocks
	// are a possibility with this type of event.
	RunRouteStateChanged *pRun = new RunRouteStateChanged;
	pRun->Queue( this, "ICE OnRouteChanged" );
}

void CConnectionTransportP2PICE_WebRTC::OnData( const void *pPkt, size_t nSize )
{
	if ( Connection().m_pTransportICE != this )
		return;

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	const int cbPkt = int(nSize);

	if ( nSize < 1 )
	{
		ReportBadUDPPacketFromConnectionPeer( "packet", "Bad packet size: %d", cbPkt );
		return;
	}

	// See if we can process this packet (and anything queued before us)
	// immediately
	if ( SteamNetworkingGlobalLock::TryLock( "ICE Data", 0 ) )
	{

		// We can process the data now!  Grab the connection lock.
		ConnectionScopeLock connectionLock( m_connection );

		//SpewMsg( "CConnectionTransportP2PICE_WebRTC::OnData %d bytes, process immediate\n", (int)nSize );

		// Check if queue is empty.  Note that no race conditions here.  We hold the lock,
		// which means we aren't messing with it in some other thread.  And we are in WebRTC's
		// callback, and we assume WebRTC will not call us from two threads at the same time.
		if ( m_bufPacketQueue.TellPut() > 0 )
		{
			DrainPacketQueue( usecNow );
			Assert( m_bufPacketQueue.TellPut() == 0 );
		}

		// And now process this packet
		ProcessPacket( (const uint8_t*)pPkt, cbPkt, usecNow );
		SteamNetworkingGlobalLock::Unlock();
		return;
	}

	// We're busy in the other thread.  We'll have to queue the data.
	// Grab the buffer lock
	m_mutexPacketQueue.lock();
	int nSaveTellPut = m_bufPacketQueue.TellPut();
	m_bufPacketQueue.PutInt( cbPkt );
	m_bufPacketQueue.Put( pPkt, cbPkt );
	m_mutexPacketQueue.unlock();

	// If the queue was empty,then we need to add a task to flush it
	// when we acquire the queue.  If it wasn't empty then a task is
	// already in the queue.  Or perhaps it was progress right now
	// in some other thread.  But if that were the case, we know that
	// it had not yet actually swapped the buffer out.  Because we had
	// the buffer lock when we checked if the queue was empty.
	if ( nSaveTellPut == 0 )
	{
		//SpewMsg( "CConnectionTransportP2PICE_WebRTC::OnData %d bytes, queued, added drain queue task\n", (int)nSize );
		struct RunDrainQueue : IConnectionTransportP2PICERunWithLock
		{
			virtual void RunTransportP2PICE( CConnectionTransportP2PICE_WebRTC *pTransport )
			{
				pTransport->DrainPacketQueue( SteamNetworkingSockets_GetLocalTimestamp() );
			}
		};

		RunDrainQueue *pRun = new RunDrainQueue;

		// Queue it.  Don't use RunOrQueue.  We know we need to queue it,
		// since we already tried to grab the lock and failed.
		pRun->Queue( this, "ICE DrainQueue" );
	}
	else
	{
		if ( nSaveTellPut > 30000 )
		{
			SpewMsg( "CConnectionTransportP2PICE_WebRTC::OnData %d bytes, queued, %d previously queued LOCK PROBLEM!\n", (int)nSize, nSaveTellPut );
		}
		else
		{
			//SpewMsg( "CConnectionTransportP2PICE_WebRTC::OnData %d bytes, queued, %d previously queued\n", (int)nSize, nSaveTellPut );
		}
	}
}

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC
