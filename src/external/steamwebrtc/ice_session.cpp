//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================
#include "ice_session.h"
#include "steamwebrtc_internal.h"

#include <mutex>
#include <string.h>

#include <string>

#include <absl/types/optional.h>
#include <absl/memory/memory.h>
#include <api/async_resolver_factory.h>
#include <api/turn_customizer.h>
#include <rtc_base/network_route.h>
#include <rtc_base/bind.h>
#include <rtc_base/physical_socket_server.h>
#include <rtc_base/ssl_adapter.h>

#include <api/jsep.h>
#include <p2p/base/p2p_transport_channel.h>
#include <p2p/base/basic_packet_socket_factory.h>
#include <p2p/client/basic_port_allocator.h>

#include <pc/webrtc_sdp.h>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#else
	#include <pthread.h>
#endif

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

extern "C"
{
	static void (*g_fnWriteEvent_setsockopt)( int slevel, int sopt, int value ) = nullptr;
	static void (*g_fnWriteEvent_send)( int length ) = nullptr;
	static void (*g_fnWriteEvent_sendto)( void *addr, int length ) = nullptr;
}

extern "C"
{
STEAMWEBRTC_DECLSPEC IICESession *CreateWebRTCICESession( const ICESessionConfig &cfg, IICESessionDelegate *pDelegate, int nInterfaceVersion );
}

//-----------------------------------------------------------------------------
// Class to represent an ICE connection
//-----------------------------------------------------------------------------
class CICESession final : public IICESession, public sigslot::has_slots<>, private rtc::MessageHandler
{
public:
	CICESession( IICESessionDelegate *pDelegate );
	virtual ~CICESession();

	bool BInitialize( const ICESessionConfig &cfg );
	bool BInitializeOnSocketThread( const ICESessionConfig &cfg );
	void DestroyOnSocketThread();
	bool BShuttingDown() const { return m_bShuttingDown; }

	//
	// IICESession
	//
	virtual void Destroy() override;
	virtual bool BSendData( const void *pData, size_t nSize ) override;
	virtual void SetRemoteAuth( const char *pszUserFrag, const char *pszPwdFrag ) override;
	virtual EICECandidateType AddRemoteIceCandidate( const char *pszCandidate ) override;
	virtual bool GetWritableState() override;
	virtual int GetPing() override;
	virtual bool GetRoute( EICECandidateType &eLocalCandidate, EICECandidateType &eRemoteCandidate, CandidateAddressString &szRemoteAddress ) override;
	virtual void SetWriteEvent_setsockopt( void (*fn)( int slevel, int sopt, int value ) ) override { g_fnWriteEvent_setsockopt = fn; }
	virtual void SetWriteEvent_send( void (*fn)( int length ) ) override { g_fnWriteEvent_send = fn; }
	virtual void SetWriteEvent_sendto( void (*fn)( void *addr, int length ) ) override { g_fnWriteEvent_sendto = fn; }

	// rtc::MessageHandler
	virtual void OnMessage( rtc::Message* msg ) override;


private:
	static std::mutex s_mutex;
	static int s_nInstaneCount;
	static rtc::Thread *s_pSocketThread;
	static rtc::PhysicalSocketServer *s_pSocketServer;

	int m_nAllowedCandidateTypes;

	bool m_bShuttingDown = false;
	IICESessionDelegate *m_pDelegate = nullptr;
	bool writable_ = false;
	std::unique_ptr<cricket::P2PTransportChannel> ice_transport_;
	std::unique_ptr<rtc::BasicNetworkManager> default_network_manager_;
	std::unique_ptr<rtc::BasicPacketSocketFactory> default_socket_factory_;
	std::unique_ptr< cricket::BasicPortAllocator > port_allocator_;

	// ICE signals
	void OnTransportGatheringState_n(cricket::IceTransportInternal* transport);
	void OnTransportCandidateGathered_n(cricket::IceTransportInternal* transport, const cricket::Candidate& candidate);
	void OnTransportCandidatesRemoved_n(cricket::IceTransportInternal* transport, const cricket::Candidates& candidates);
	void OnTransportRoleConflict_n(cricket::IceTransportInternal* transport);
	void OnTransportStateChanged_n(cricket::IceTransportInternal* transport);
	void OnWritableState(rtc::PacketTransportInternal* transport);
	void OnReadPacket(
		rtc::PacketTransportInternal* transport,
		const char* data,
		size_t size,
		const int64_t& packet_time,
		int flags);
	void OnSentPacket(rtc::PacketTransportInternal* transport, const rtc::SentPacket& sent_packet);
	void OnReadyToSend(rtc::PacketTransportInternal* transport);
	void OnReceivingState(rtc::PacketTransportInternal* transport);
	void OnNetworkRouteChanged(absl::optional<rtc::NetworkRoute> network_route);
};


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
IICESession *CreateWebRTCICESession( const ICESessionConfig &cfg, IICESessionDelegate *pDelegate, int nInterfaceVersion )
{
	if ( nInterfaceVersion != ICESESSION_INTERFACE_VERSION )
	{
		return nullptr;
	}

	CICESession *pSession = new CICESession( pDelegate );
	if ( !pSession->BInitialize( cfg ) )
	{
		pSession->Destroy();
		return nullptr;
	}
	return pSession;
}

std::mutex CICESession::s_mutex;
int CICESession::s_nInstaneCount = 0;
rtc::Thread *CICESession::s_pSocketThread = nullptr;
rtc::PhysicalSocketServer *CICESession::s_pSocketServer = nullptr;

//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CICESession::CICESession( IICESessionDelegate *pDelegate ) :
	m_pDelegate( pDelegate )
{
	s_mutex.lock();
	if ( ++s_nInstaneCount == 1 )
	{
		rtc::InitializeSSL();
		assert( s_pSocketServer == nullptr );
		s_pSocketServer = new rtc::PhysicalSocketServer;

		assert( s_pSocketThread == nullptr );
		s_pSocketThread = new rtc::Thread( s_pSocketServer );
		s_pSocketThread->Start();
	}
	s_mutex.unlock();
}


//-----------------------------------------------------------------------------
// Destructor
//-----------------------------------------------------------------------------
CICESession::~CICESession()
{
	s_pSocketThread->Invoke<void>( RTC_FROM_HERE, rtc::Bind( &CICESession::DestroyOnSocketThread, this ) );

	s_mutex.lock();
	if ( --s_nInstaneCount == 0 )
	{
		s_pSocketThread->Quit();
		delete s_pSocketThread;
		s_pSocketThread = nullptr;

		delete s_pSocketServer;
		s_pSocketServer = nullptr;

		rtc::CleanupSSL();
	}
	s_mutex.unlock();
}


//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
bool CICESession::BInitialize( const ICESessionConfig &cfg )
{
	return s_pSocketThread->Invoke<bool>( RTC_FROM_HERE, rtc::Bind( &CICESession::BInitializeOnSocketThread, this, cfg ) );
}


//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
bool CICESession::BInitializeOnSocketThread( const ICESessionConfig &cfg )
{
	#ifdef _WIN32
		::SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
	#elif !defined(WEBRTC_MARVELL) // Don't change priority on Steam Link hardware
		struct sched_param sched;
		int policy;
		pthread_t thread = pthread_self();

		if (pthread_getschedparam(thread, &policy, &sched) == 0) {
			sched.sched_priority = sched_get_priority_max(policy);
			pthread_setschedparam(thread, policy, &sched);
		}
	#endif

	m_nAllowedCandidateTypes = cfg.m_nCandidateTypes;

	default_network_manager_.reset(new rtc::BasicNetworkManager());
	default_socket_factory_.reset(
		new rtc::BasicPacketSocketFactory( s_pSocketThread ));

	webrtc::TurnCustomizer *turn_customizer = nullptr;

	port_allocator_ = absl::make_unique<cricket::BasicPortAllocator>(
		default_network_manager_.get(), default_socket_factory_.get(),
		turn_customizer );

	// See PeerConnection::InitializePortAllocator_n
	port_allocator_->Initialize();

	// To handle both internal and externally created port allocator, we will
	// enable BUNDLE here.
	uint32_t port_allocator_flags_ = port_allocator_->flags();
	port_allocator_flags_ |= cricket::PORTALLOCATOR_ENABLE_SHARED_SOCKET;
	port_allocator_flags_ |= cricket::PORTALLOCATOR_DISABLE_TCP;

	uint32_t candidate_filter = cricket::CF_NONE;

	cricket::ServerAddresses stun_servers;
	if ( cfg.m_nCandidateTypes & k_EICECandidate_Any_Reflexive )
	{
		candidate_filter |= cricket::CF_REFLEXIVE;
		for ( int i = 0 ; i < cfg.m_nStunServers ; ++i )
		{
			const char *pszStun = cfg.m_pStunServers[i];

			// Skip "stun:" prefix, if present
			if ( strncasecmp( pszStun, "stun:", 5 ) == 0 )
				pszStun += 5;

			rtc::SocketAddress address;
			if ( !address.FromString( std::string( pszStun ) ) )
			{
				m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "Invalid STUN server address '%s'\n", pszStun );
				return false;
			}
			if ( address.port() == 0 )
				address.SetPort( 3478 ); // default STUN port

			stun_servers.insert( address );
		}
	}
	else
	{
		port_allocator_flags_ |= cricket::PORTALLOCATOR_DISABLE_STUN;
	}

	if ( cfg.m_nCandidateTypes & (k_EICECandidate_Any_HostPrivate|k_EICECandidate_Any_HostPublic) )
		candidate_filter |= cricket::CF_HOST;
	if ( cfg.m_nCandidateTypes & k_EICECandidate_Any_Relay )
		candidate_filter |= cricket::CF_RELAY;
	port_allocator_flags_ |= cricket::PORTALLOCATOR_DISABLE_LINK_LOCAL_NETWORKS;
	if ( cfg.m_nCandidateTypes & k_EICECandidate_Any_IPv6 )
	{
		port_allocator_flags_ |= 
			cricket::PORTALLOCATOR_ENABLE_IPV6 |
			cricket::PORTALLOCATOR_ENABLE_IPV6_ON_WIFI;
	}

	std::vector<cricket::RelayServerConfig> turn_servers;
	if ( cfg.m_nCandidateTypes & (k_EICECandidate_Any_Reflexive|k_EICECandidate_Any_Relay) )
	{
		candidate_filter |= cricket::CF_REFLEXIVE | cricket::CF_RELAY;
		for ( int i = 0 ; i < cfg.m_nTurnServers ; ++i )
		{
			const ICESessionConfig::TurnServer *pTurn = &cfg.m_pTurnServers[i];
			
			if ( !pTurn || !pTurn->m_pszHost || !pTurn->m_pszPwd || !pTurn->m_pszUsername ) {
				continue;
			}

			const char *pszTurn = pTurn->m_pszHost;

			// Skip "turn:" prefix, if present
			if ( strncasecmp( pszTurn, "turn:", 5 ) == 0 )
				pszTurn += 5;

			rtc::SocketAddress address;
			if ( !address.FromString( std::string( pszTurn ) ) )
			{
				m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "Invalid Turn server address '%s'\n", pszTurn );
				return false;
			}
			if ( address.port() == 0 )
				address.SetPort( 3478 ); // default STUN port

			switch( pTurn->m_protocolType ) {
				case k_EProtocolTypeUDP:
				case k_EProtocolTypeTCP:
				case k_EProtocolTypeSSLTCP:
				case k_EProtocolTypeTLS:
				default:
				m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "Invalid Turn server protocol type '%d'\n", (int) pTurn->m_protocolType );
				return false;
			}

			cricket::RelayServerConfig turn(address.hostname(), address.port(), 
				pTurn->m_pszUsername, pTurn->m_pszPwd, (cricket::ProtocolType) pTurn->m_protocolType);
			turn_servers.push_back( turn );
		}
	}

	port_allocator_->set_flags(port_allocator_flags_);

	// No step delay is used while allocating ports.
	port_allocator_->set_step_delay(cricket::kMinimumStepDelay);

	port_allocator_->set_candidate_filter( candidate_filter );

	//port_allocator_->set_max_ipv6_networks(configuration.max_ipv6_networks);

	int ice_candidate_pool_size = 0; // ???
	bool prune_turn_ports = false;
	absl::optional<int> stun_candidate_keepalive_interval = absl::nullopt;

	if ( !port_allocator_->SetConfiguration(
		stun_servers, turn_servers,
		ice_candidate_pool_size, prune_turn_ports,
		turn_customizer,
		stun_candidate_keepalive_interval
	) ) {
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "PortAllocator::SetConfiguration failed\n" );
		return false;
	}

	// ???
	std::string transport_name = "CICESession";
	int component = 0;
	std::unique_ptr<webrtc::AsyncResolverFactory> async_resolver_factory_; // This is apparently allowed to be null

	ice_transport_ = absl::make_unique<cricket::P2PTransportChannel>(
		transport_name, component, port_allocator_.get(), async_resolver_factory_.get(),
		nullptr );

	if ( !ice_transport_ )
	{
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "Failed to create P2PTransportChannel\n" );
		return false;
	}

	const int kBufferSize = 512*1024;
	ice_transport_->SetOption( rtc::Socket::OPT_SNDBUF, kBufferSize );
	ice_transport_->SetOption( rtc::Socket::OPT_RCVBUF, kBufferSize );

	static_assert(
		(int)k_EICERole_Unknown == (int)cricket::ICEROLE_UNKNOWN
		&& (int)k_EICERole_Controlling == (int)cricket::ICEROLE_CONTROLLING
		&& (int)k_EICERole_Controlled == (int)cricket::ICEROLE_CONTROLLED, "We assume our ICE role enum matches WebRTC's" );
	ice_transport_->SetIceRole( cricket::IceRole( cfg.m_eRole ) );

	//m_transport->SetIceTiebreaker(ice_tiebreaker_);
	//cricket::IceConfig ice_config;
	//ice_config.receiving_timeout = RTCConfigurationToIceConfigOptionalInt(
	//	config.ice_connection_receiving_timeout);
	//ice_config.prioritize_most_likely_candidate_pairs =
	//	config.prioritize_most_likely_ice_candidate_pairs;
	//ice_config.backup_connection_ping_interval =
	//	RTCConfigurationToIceConfigOptionalInt(
	//		config.ice_backup_candidate_pair_ping_interval);
	//ice_config.continual_gathering_policy = gathering_policy;
	//ice_config.presume_writable_when_fully_relayed = config.presume_writable_when_fully_relayed;
	//ice_config.ice_check_interval_strong_connectivity = config.ice_check_interval_strong_connectivity;
	//ice_config.ice_check_interval_weak_connectivity = config.ice_check_interval_weak_connectivity;
	//ice_config.ice_check_min_interval = config.ice_check_min_interval;
	//ice_config.stun_keepalive_interval = config.stun_candidate_keepalive_interval;
	//ice_config.regather_all_networks_interval_range = config.ice_regather_interval_range;
	//ice_config.network_preference = config.network_preference;
	//ice_transport_->SetIceConfig(ice_config);

	// Set our local parameters.  We don't know the other guy's params yet
	cricket::IceParameters ice_params;
	ice_params.ufrag = cfg.m_pszLocalUserFrag;
	ice_params.pwd = cfg.m_pszLocalPwd;
	ice_transport_->SetIceParameters( ice_params );

	ice_transport_->SignalGatheringState.connect( this, &CICESession::OnTransportGatheringState_n);
	ice_transport_->SignalCandidateGathered.connect( this, &CICESession::OnTransportCandidateGathered_n);
	ice_transport_->SignalCandidatesRemoved.connect( this, &CICESession::OnTransportCandidatesRemoved_n);
	ice_transport_->SignalRoleConflict.connect( this, &CICESession::OnTransportRoleConflict_n);
	ice_transport_->SignalStateChanged.connect( this, &CICESession::OnTransportStateChanged_n);
	ice_transport_->SignalWritableState.connect( this, &CICESession::OnWritableState );
	ice_transport_->SignalReadPacket.connect( this, &CICESession::OnReadPacket );
	ice_transport_->SignalSentPacket.connect(this, &CICESession::OnSentPacket);
	ice_transport_->SignalReadyToSend.connect(this, &CICESession::OnReadyToSend);
	ice_transport_->SignalReceivingState.connect( this, &CICESession::OnReceivingState);
	ice_transport_->SignalNetworkRouteChanged.connect( this, &CICESession::OnNetworkRouteChanged);

	ice_transport_->MaybeStartGathering();

	return true;
}

void CICESession::DestroyOnSocketThread()
{
	// Kind of defeats the purpose of using std::unique_ptr to manually
	// destroy like this, but we really want to control the teardown order.
	// and we need it to happen in a particular thread, so being "subtle"
	// would be counter productive.
	writable_ = false;
	ice_transport_.reset();
	port_allocator_.reset();
	default_socket_factory_.reset();
	default_network_manager_.reset();
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CICESession::Destroy()
{
	m_bShuttingDown = true;
	delete this;
}

EICECandidateType GetICECandidateType( const cricket::Candidate &candidate )
{
	const rtc::SocketAddress &addr = candidate.address();
	if ( !addr.IsComplete() )
		return k_EICECandidate_Invalid;

	const std::string &typ = candidate.type();
	EICECandidateType eResult;
	if ( strcasecmp( typ.c_str(), cricket::LOCAL_PORT_TYPE ) == 0 )
	{
		// NOTE: This doesn't classify fc00::/7 as private
		if ( addr.IsPrivateIP() )
			eResult = k_EICECandidate_IPv4_HostPrivate;
		else
			eResult = k_EICECandidate_IPv4_HostPublic;
	}
	else if ( strcasecmp( typ.c_str(), cricket::STUN_PORT_TYPE ) == 0 || strcasecmp( typ.c_str(), cricket::PRFLX_PORT_TYPE ) == 0 )
	{
		eResult = k_EICECandidate_IPv4_Reflexive;
	}
	else if ( strcasecmp( typ.c_str(), cricket::RELAY_PORT_TYPE ) == 0 )
	{
		eResult = k_EICECandidate_IPv4_Relay;
	}
	else
	{
		return k_EICECandidate_Invalid;
	}

	switch ( candidate.address().family() )
	{
		case AF_INET:
			return eResult;
		case AF_INET6:
			static_assert(
				k_EICECandidate_IPv4_HostPrivate<<8 == k_EICECandidate_IPv6_HostPrivate_Unsupported
				&& k_EICECandidate_IPv4_HostPublic<<8 == k_EICECandidate_IPv6_HostPublic
				&& k_EICECandidate_IPv4_Reflexive<<8 == k_EICECandidate_IPv6_Reflexive
				&& k_EICECandidate_IPv4_Relay<<8 == k_EICECandidate_IPv6_Relay, "We assume bit layout" );
			return EICECandidateType( eResult << 8 );
	}

	return k_EICECandidate_Invalid;
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
EICECandidateType CICESession::AddRemoteIceCandidate( const char *pszCandidate )
{
	webrtc::SdpParseError error;
	cricket::Candidate candidate;
	if ( !webrtc::SdpDeserializeCandidate(
		"", // transport_name, not really used
		std::string( pszCandidate ),
        &candidate,
        &error
	) ) {
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "Error parsing ICE candidate '%s': %s\n", pszCandidate, error.description.c_str() );
		return k_EICECandidate_Invalid;
	}

	// Should we post instead of invoke here?
	s_pSocketThread->Invoke<void>( RTC_FROM_HERE, rtc::Bind( &cricket::P2PTransportChannel::AddRemoteCandidate, ice_transport_.get(), candidate ) );

	return GetICECandidateType( candidate );
}

const uint32_t SEND_PACKET_IN_GOOGLE_THREAD = 1000;
struct SendPacktetInGoogleThread : rtc::MessageData
{
	size_t nSize;
	char data[4];
};

void CICESession::OnMessage( rtc::Message* msg )
{
	if ( msg->message_id != SEND_PACKET_IN_GOOGLE_THREAD )
	{
		assert( false );
		return;
	}

	if ( !ice_transport_ )
		return;

	auto *pkt = static_cast<SendPacktetInGoogleThread *>( msg->pdata );
	rtc::PacketOptions options;
	int flags = 0;
	ice_transport_->SendPacket( pkt->data, pkt->nSize, options, flags );
}

bool CICESession::BSendData( const void *pData, size_t nSize )
{
	if ( !ice_transport_ || !writable_ )
		return false;

	// Create a message to send it in the other thread.  I hate all this payload
	// copying and context switching.  It's fine on machines with plenty of
	// hardware threads, but on limited hardware, this is a perf bottleneck
	SendPacktetInGoogleThread *pkt = (SendPacktetInGoogleThread*)malloc( sizeof(SendPacktetInGoogleThread) - sizeof(SendPacktetInGoogleThread::data) + nSize );
	new ( pkt ) SendPacktetInGoogleThread();
	pkt->nSize = nSize;
	memcpy( pkt->data, pData, nSize );
	bool time_sensitive = false; // Actually, this is really time sensitive.  But passing "true" triggers an assert for some reason.
	s_pSocketThread->Post( RTC_FROM_HERE, this, SEND_PACKET_IN_GOOGLE_THREAD, pkt, time_sensitive );
	return true;
}

void CICESession::SetRemoteAuth( const char *pszUserFrag, const char *pszPwd )
{
	if ( !ice_transport_ )
		return;
	cricket::IceParameters ice_params;
	ice_params.ufrag = pszUserFrag;
	ice_params.pwd = pszPwd;
	s_pSocketThread->Invoke<void>( RTC_FROM_HERE, rtc::Bind( &cricket::P2PTransportChannel::SetRemoteIceParameters, ice_transport_.get(), ice_params ) );
}

bool CICESession::GetWritableState()
{
	return ice_transport_ && writable_;
}

int CICESession::GetPing()
{
	if ( !ice_transport_ )
		return -1;
	absl::optional<int> rtt = ice_transport_->GetRttEstimate();
	return ( rtt ) ? *rtt : -1;
}

bool CICESession::GetRoute( EICECandidateType &eLocalCandidate, EICECandidateType &eRemoteCandidate, CandidateAddressString &szRemoteAddress )
{
	if ( !ice_transport_ )
		return false;
	const cricket::Connection *conn = ice_transport_->selected_connection();
	if ( !conn )
		return false;

	eLocalCandidate = GetICECandidateType( conn->local_candidate() );
	eRemoteCandidate = GetICECandidateType( conn->remote_candidate() );
	std::string remote_addr = conn->remote_candidate().address().ToString();
	strncpy( szRemoteAddress, remote_addr.c_str(), sizeof(CandidateAddressString) - 1 );
	szRemoteAddress[ sizeof(CandidateAddressString)-1 ] = '\0';

	return eLocalCandidate != k_EICECandidate_Invalid && eRemoteCandidate != k_EICECandidate_Invalid && szRemoteAddress[0] != '\0';
}

void CICESession::OnTransportGatheringState_n(cricket::IceTransportInternal* transport)
{
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "P2PTransportChannel::OnTransportGatheringState now %d\n", ice_transport_->gathering_state() );
}

void CICESession::OnTransportCandidateGathered_n(cricket::IceTransportInternal* transport, const cricket::Candidate& candidate)
{
	std::string sdp = webrtc::SdpSerializeCandidate( candidate );
	EICECandidateType eType = GetICECandidateType( candidate );
	m_pDelegate->OnLocalCandidateGathered( eType, sdp.c_str() );
}

void CICESession::OnTransportCandidatesRemoved_n(cricket::IceTransportInternal* transport, const cricket::Candidates& candidates)
{
	// FIXME delegate doesn't understand this right now
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityWarning, "Ignoring removal of %d ICE candidate\n", (int)candidates.size() );
}

void CICESession::OnTransportRoleConflict_n(cricket::IceTransportInternal* transport)
{
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "ICE role conflict detected!\n" );
}

void CICESession::OnTransportStateChanged_n(cricket::IceTransportInternal* transport)
{
	cricket::IceTransportState state = ice_transport_->GetState();
	if ( state == cricket::IceTransportState::STATE_COMPLETED )
	{
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE completed\n" );
		//m_pDelegate->OnFinished( true );
	}
	else if ( state == cricket::IceTransportState::STATE_FAILED )
	{
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE failed\n" );
		//m_pDelegate->OnFinished( false );
	}
}

void CICESession::OnWritableState(rtc::PacketTransportInternal* transport)
{
	writable_ = ice_transport_->writable();
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE OnWritableState now %d\n", (int)writable_ );
	m_pDelegate->OnWritableStateChanged();
}

void CICESession::OnReadPacket(
	rtc::PacketTransportInternal* transport,
	const char* data,
	size_t size,
	const int64_t& packet_time,
	int flags
) {
	m_pDelegate->OnData( data, size );
}

void CICESession::OnSentPacket(rtc::PacketTransportInternal* transport, const rtc::SentPacket& sent_packet)
{
}

void CICESession::OnReadyToSend(rtc::PacketTransportInternal* transport)
{
}

void CICESession::OnReceivingState(rtc::PacketTransportInternal* transport)
{
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE OnReceivingState now %d\n", ice_transport_->receiving() );
}

void CICESession::OnNetworkRouteChanged(absl::optional<rtc::NetworkRoute> network_route)
{
	m_pDelegate->OnRouteChanged();
	//m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE OnNetworkRouteChanged %d\n", ice_transport_->receiving() );
}
