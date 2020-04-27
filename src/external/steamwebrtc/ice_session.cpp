//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================
#include "ice_session.h"
#include "steamwebrtc_internal.h"

#include <mutex>

#include "rtc_base.h"
#include <rtc_base/bind.h>
#include <rtc_base/flags.h>
#ifdef WEBRTC_ANDROID
#include <rtc_base/physical_socket_server.h>
#include <rtc_base/ssl_adapter.h>
#else
#include <rtc_base/physicalsocketserver.h>
#include <rtc_base/ssladapter.h>
#endif

#include <api/jsep.h>
#include <logging/rtc_event_log/rtc_event_log_factory.h>
#include <p2p/base/p2ptransportchannel.h>
#include <p2p/base/basicpacketsocketfactory.h>
#include <p2p/client/basicportallocator.h>

extern "C"
{
STEAMWEBRTC_DECLSPEC IICESession *CreateWebRTCICESession( IICESessionDelegate *pDelegate, int nInterfaceVersion );
}

//-----------------------------------------------------------------------------
// Class to represent an ICE connection
//-----------------------------------------------------------------------------
class CICESession final : public IICESession, public sigslot::has_slots<>, private rtc::MessageHandler
{
public:
	CICESession( IICESessionDelegate *pDelegate );
	virtual ~CICESession();

	bool BInitialize();
	bool BInitializeOnSocketThread();
	void DestroyOnSocketThread();
	bool BShuttingDown() const { return m_bShuttingDown; }

	//
	// IICESession
	//
	virtual void Destroy() override;
	virtual bool BSendData( const void *pData, size_t nSize ) override;
	virtual bool BAddRemoteIceCandidate( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) override;
	virtual bool GetWritableState() override;

	virtual void OnMessage( rtc::Message* msg ) override;

private:
	static std::mutex s_mutex;
	static int s_nInstaneCount;
	static rtc::Thread *s_pSocketThread;
	static rtc::PhysicalSocketServer *s_pSocketServer;

	bool m_bShuttingDown = false;
	IICESessionDelegate *m_pDelegate = nullptr;
    std::unique_ptr<cricket::P2PTransportChannel> ice_transport_;
	std::unique_ptr<webrtc::RtcEventLogFactoryInterface> event_log_factory_;
	std::unique_ptr<webrtc::RtcEventLog> event_log_;
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
		const rtc::PacketTime& packet_time,
		int flags);
	void OnSentPacket(rtc::PacketTransportInternal* transport, const rtc::SentPacket& sent_packet);
	void OnReadyToSend(rtc::PacketTransportInternal* transport);
	void OnReceivingState(rtc::PacketTransportInternal* transport);
	void OnNetworkRouteChanged(absl::optional<rtc::NetworkRoute> network_route);
};


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
IICESession *CreateWebRTCICESession( IICESessionDelegate *pDelegate, int nInterfaceVersion )
{
	if ( nInterfaceVersion != ICESESSION_INTERFACE_VERSION )
	{
		return nullptr;
	}

	CICESession *pSession = new CICESession( pDelegate );
	if ( !pSession->BInitialize() )
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
bool CICESession::BInitialize()
{
	return s_pSocketThread->Invoke<bool>( RTC_FROM_HERE, rtc::Bind( &CICESession::BInitializeOnSocketThread, this ) );
}


//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
bool CICESession::BInitializeOnSocketThread()
{
	event_log_factory_ = webrtc::CreateRtcEventLogFactory();

	// Uhhhhhhh
	auto encoding_type = webrtc::RtcEventLog::EncodingType::Legacy;
	//if (field_trial::IsEnabled("WebRTC-RtcEventLogNewFormat"))
	//  encoding_type = RtcEventLog::EncodingType::NewFormat;
	event_log_ = event_log_factory_
				? event_log_factory_->CreateRtcEventLog(encoding_type)
				: absl::make_unique<webrtc::RtcEventLogNullImpl>();

	default_network_manager_.reset(new rtc::BasicNetworkManager());
	default_socket_factory_.reset(
		new rtc::BasicPacketSocketFactory( s_pSocketThread ));

	webrtc::TurnCustomizer *turn_cusomizer = nullptr;

	port_allocator_ = absl::make_unique<cricket::BasicPortAllocator>(
		default_network_manager_.get(), default_socket_factory_.get(),
		turn_cusomizer );

	cricket::ServerAddresses stun_servers;
	for ( int i = 0 ; i < m_pDelegate->GetNumStunServers() ; ++i )
	{
		const char *pszStun = m_pDelegate->GetStunServer( i );

		// Skip "stun:" prefix, if present
		if ( _strnicmp( pszStun, "stun:", 5 ) == 0 )
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

	// FIXME
	std::vector<cricket::RelayServerConfig> turn_servers;

	// See PeerConnection::InitializePortAllocator_n
	port_allocator_->Initialize();

	// To handle both internal and externally created port allocator, we will
	// enable BUNDLE here.
	uint32_t port_allocator_flags_ = port_allocator_->flags();
	port_allocator_flags_ |= cricket::PORTALLOCATOR_ENABLE_SHARED_SOCKET |
							cricket::PORTALLOCATOR_ENABLE_IPV6 |
							cricket::PORTALLOCATOR_ENABLE_IPV6_ON_WIFI;

	port_allocator_flags_ |= cricket::PORTALLOCATOR_DISABLE_TCP;

	//if (configuration.candidate_network_policy ==
	//	kCandidateNetworkPolicyLowCost) {
	//port_allocator_flags_ |= cricket::PORTALLOCATOR_DISABLE_COSTLY_NETWORKS;

	port_allocator_flags_ |= cricket::PORTALLOCATOR_DISABLE_LINK_LOCAL_NETWORKS;

	port_allocator_->set_flags(port_allocator_flags_);

	// No step delay is used while allocating ports.
	port_allocator_->set_step_delay(cricket::kMinimumStepDelay);

	//CF_NONE = 0x0,
	//CF_HOST = 0x1,
	//CF_REFLEXIVE = 0x2,
	//CF_RELAY = 0x4,
	//CF_ALL = 0x7,
	// FIXME - remove CF_RELAY conditionally?
	port_allocator_->set_candidate_filter( cricket::CF_ALL );

	//port_allocator_->set_max_ipv6_networks(configuration.max_ipv6_networks);

	int ice_candidate_pool_size = 0; // ???
	bool prune_turn_ports = false;
    webrtc::TurnCustomizer* turn_customizer = nullptr;
	absl::optional<int> stun_candidate_keepalive_interval = absl::nullopt;

	if ( !port_allocator_->SetConfiguration(
		stun_servers, turn_servers,
		ice_candidate_pool_size, prune_turn_ports,
		turn_customizer,
		stun_candidate_keepalive_interval
	) ) {
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "PortAllocator::SetConfiguration faiuled\n" );
		return false;
	}

	// ???
	std::string transport_name = "CICESession";
	int component = 0;
	std::unique_ptr<webrtc::AsyncResolverFactory> async_resolver_factory_; // This is apparently allowed to be null

    ice_transport_ = absl::make_unique<cricket::P2PTransportChannel>(
        transport_name, component, port_allocator_.get(), async_resolver_factory_.get(),
        event_log_.get() );

	if ( !ice_transport_ )
	{
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "Failed to create P2PTransportChannel\n" );
		return false;
	}

	static_assert(
		(int)k_EICERole_Unknown == (int)cricket::ICEROLE_UNKNOWN
		&& (int)k_EICERole_Controlling == (int)cricket::ICEROLE_CONTROLLING
		&& (int)k_EICERole_Controlled == (int)cricket::ICEROLE_CONTROLLED, "We assume our ICE role enum matches WebRTC's" );
	ice_transport_->SetIceRole( cricket::IceRole( m_pDelegate->GetRole() ) );

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

	// !TEST! Set ufrag and password.  Do we need to be signaling these?
	//cricket::IceParameters ice_params;
	//webrtc::Random random( rtc::SystemTimeNanos() );
	//char buf[32];
	//sprintf_s( buf, sizeof(buf), "%8x", random.Rand(0,INT_MAX) );
	//ice_params.ufrag = buf;
	//sprintf_s( buf, sizeof(buf), "%8x", random.Rand(0,INT_MAX) );
	//ice_params.pwd = buf;
	//ice_transport_->SetIceParameters( ice_params );

	// !TEST! Hardcode some params just so we can move on.
	// We probably need to signal these
	{
		cricket::IceParameters server_ice_params;
		server_ice_params.ufrag = "s123";
		server_ice_params.pwd = "sxyz";

		cricket::IceParameters client_ice_params;
		client_ice_params.ufrag = "c123";
		client_ice_params.pwd = "cxyz";

		if ( m_pDelegate->GetRole() == k_EICERole_Controlled )
		{
			ice_transport_->SetIceParameters( server_ice_params );
			ice_transport_->SetRemoteIceParameters( client_ice_params );
		}
		else
		{
			ice_transport_->SetIceParameters( client_ice_params );
			ice_transport_->SetRemoteIceParameters( server_ice_params );
		}
	}

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
	ice_transport_.reset();
	port_allocator_.reset();
	default_socket_factory_.reset();
	default_network_manager_.reset();
	event_log_.reset();
	event_log_factory_.reset();
}

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CICESession::Destroy()
{
	m_bShuttingDown = true;
	delete this;
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
bool CICESession::BAddRemoteIceCandidate( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate )
{
	webrtc::SdpParseError error;
	webrtc::IceCandidateInterface* pCandidate = CreateIceCandidate( pszSDPMid, nSDPMLineIndex, pszCandidate, &error );
	if ( !error.line.empty() && !error.description.empty() )
	{
		m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "Error parsing ICE candidates on line %s: %s\n", error.line.c_str(), error.description.c_str() );
		return false;
	}

	s_pSocketThread->Invoke<void>( RTC_FROM_HERE, rtc::Bind( &cricket::P2PTransportChannel::AddRemoteCandidate, ice_transport_.get(), pCandidate->candidate() ) );
	delete pCandidate;
	return true;
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
	if ( !ice_transport_ || !ice_transport_->writable() )
		return false;

	SendPacktetInGoogleThread *pkt = (SendPacktetInGoogleThread*)malloc( sizeof(SendPacktetInGoogleThread) - sizeof(SendPacktetInGoogleThread::data) + nSize );
	new ( pkt ) SendPacktetInGoogleThread();
	pkt->nSize = nSize;
	memcpy( pkt->data, pData, nSize );
	s_pSocketThread->Post( RTC_FROM_HERE, this, SEND_PACKET_IN_GOOGLE_THREAD, pkt, true );

	//// FIXME This is blocking, switching threads back and forth, really bad for perf!
	//rtc::PacketOptions options;
	//int flags = 0;
	//int r = s_pSocketThread->Invoke<int>( RTC_FROM_HERE, rtc::Bind( &cricket::P2PTransportChannel::SendPacket, ice_transport_.get(), (const char *)pData, nSize, options, flags ) );
	//if ( r >= 0 )
	//	return true;
	//m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "P2PTransportChannel::SendPacket returned %d, GetError()=%d\n", r, ice_transport_->GetError() );
	//return false;
	return true;
}

bool CICESession::GetWritableState()
{
	return ice_transport_ && ice_transport_->writable();
}

void CICESession::OnTransportGatheringState_n(cricket::IceTransportInternal* transport)
{
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityError, "P2PTransportChannel::OnTransportGatheringState now %d\n", ice_transport_->gathering_state() );
}

void CICESession::OnTransportCandidateGathered_n(cricket::IceTransportInternal* transport, const cricket::Candidate& candidate)
{
	// !KLUDGE! This is putting SDP stuff on here.  I this the only string format we have?
    const std::string sdp_mid( "" );
    int sdp_mline_index = 0;
	std::unique_ptr<webrtc::IceCandidateInterface> candidate_ptr = webrtc::CreateIceCandidate(
		sdp_mid,
		sdp_mline_index,
		candidate );

	std::string candidate_str;
	candidate_ptr->ToString( &candidate_str );
	m_pDelegate->OnIceCandidateAdded( sdp_mid.c_str(), sdp_mline_index, candidate_str.c_str() );
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
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE state changed to %d\n", ice_transport_->GetState() );
}

void CICESession::OnWritableState(rtc::PacketTransportInternal* transport)
{
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE OnWritableState now %d\n", ice_transport_->writable() );
	m_pDelegate->OnWritableStateChanged();
}

void CICESession::OnReadPacket(
	rtc::PacketTransportInternal* transport,
	const char* data,
	size_t size,
	const rtc::PacketTime& packet_time,
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
	m_pDelegate->Log( IICESessionDelegate::k_ELogPriorityInfo, "ICE OnNetworkRouteChanged %d\n", ice_transport_->receiving() );
}
