//====== Copyright Valve Corporation, All rights reserved. ====================

#pragma once

#include "../steamnetworkingsockets_internal.h"

#ifdef DEFINE_CONFIG
#define SDT_EXTERNAL
#define SDT_DEFAULT( x ) = ( x )
#else
#define SDT_EXTERNAL extern 
#define SDT_DEFAULT( x )
#endif

namespace SteamNetworkingSocketsLib 
{

/////////////////////////////////////////////////////////////////////////////
//
// Configuration variables
//
///////////////////////////////////////////////////////////////////////////// 

// If the first N pings to a port all fail, mark that port as unavailable for
// a while, and try a different one.  Some ISPs and routers may drop the first
// packet, so setting this to 1 may greatly disrupt communications.
SDT_EXTERNAL int32 steamdatagram_client_consecutitive_ping_timeouts_fail_initial SDT_DEFAULT( 2 ); 

// If N consecutive pings to a port fail, after having received successful 
// communication, mark that port as unavailable for a while, and try a 
// different one.
SDT_EXTERNAL int32 steamdatagram_client_consecutitive_ping_timeouts_fail SDT_DEFAULT( 4 ); 

/// Minimum number of lifetime pings we need to send, before we think our estimate
/// is solid.  The first ping to each cluster is very often delayed because of NAT,
/// routers not having the best route, etc.  Until we've sent a sufficient number
/// of pings, our estimate is often inaccurate.  Keep pinging until we get this
/// many pings.
SDT_EXTERNAL int32 steamdatagram_client_min_pings_before_ping_accurate SDT_DEFAULT( 10 ); 

// Set all steam datagram traffic to originate from the same local port.  
// By default, we open up a new UDP socket (on a different local port) 
// for each relay.  This is not optimal, but it works around some 
// routers that don't implement NAT properly.  If you have intermittent 
// problems talking to relays that might be NAT related, try toggling 
// this flag
SDT_EXTERNAL int32 steamdatagram_client_single_socket SDT_DEFAULT( 0 );

// Fake message loss.  Should we hook this up on the receiving end, too?
// Might be easiest to do in SNP, I don't think we know at this layer whether
// the received message was reliable or not
SDT_EXTERNAL int32 steamdatagram_fakemessageloss_send SDT_DEFAULT( 0 );
SDT_EXTERNAL int32 steamdatagram_fakemessageloss_recv SDT_DEFAULT( 0 );

SDT_EXTERNAL int32 steamdatagram_fakepacketloss_send SDT_DEFAULT( 0 ); // 0-100 Randomly discard N pct of packets instead of sending
SDT_EXTERNAL int32 steamdatagram_fakepacketloss_recv SDT_DEFAULT( 0 ); // 0-100 Randomly discard N pct of packets received

SDT_EXTERNAL int32 steamdatagram_fakepacketlag_send SDT_DEFAULT( 0 ); // Globally delay all outbound packets by N ms before sending
SDT_EXTERNAL int32 steamdatagram_fakepacketlag_recv SDT_DEFAULT( 0 ); // Globally delay all received packets by N ms before processing

SDT_EXTERNAL int32 steamdatagram_fakepacketreorder_send SDT_DEFAULT( 0 ); // 0-100 Randomly redorder N pct of packets instead of sending
SDT_EXTERNAL int32 steamdatagram_fakepacketreorder_recv SDT_DEFAULT( 0 ); // 0-100 Randomly redorder N pct of packets received
SDT_EXTERNAL int32 steamdatagram_fakepacketreorder_time SDT_DEFAULT( 15 ); // How many ms to delay reordered packets.

SDT_EXTERNAL int32 steamdatagram_snp_send_buffer_size SDT_DEFAULT( 524288 ); // Upper limit of buffered pending bytes to be sent
SDT_EXTERNAL int32 steamdatagram_snp_max_rate SDT_DEFAULT( 1000000 ); // Maximum send rate clamp, 0 is no limit
SDT_EXTERNAL int32 steamdatagram_snp_min_rate SDT_DEFAULT( 128000 ); // Mininum send rate clamp, 0 is no limit

SDT_EXTERNAL int32 steamdatagram_snp_log_ackrtt SDT_DEFAULT( k_ESteamNetworkingSocketsDebugOutputType_Everything );
SDT_EXTERNAL int32 steamdatagram_snp_log_packet SDT_DEFAULT( k_ESteamNetworkingSocketsDebugOutputType_Everything );
SDT_EXTERNAL int32 steamdatagram_snp_log_message SDT_DEFAULT( k_ESteamNetworkingSocketsDebugOutputType_Everything );
SDT_EXTERNAL int32 steamdatagram_snp_log_packetgaps SDT_DEFAULT( k_ESteamNetworkingSocketsDebugOutputType_Debug );
SDT_EXTERNAL int32 steamdatagram_snp_log_p2prendezvous SDT_DEFAULT( k_ESteamNetworkingSocketsDebugOutputType_Verbose );
SDT_EXTERNAL int32 steamdatagram_snp_log_relaypings SDT_DEFAULT( k_ESteamNetworkingSocketsDebugOutputType_Debug );

SDT_EXTERNAL int32 steamdatagram_snp_nagle_time SDT_DEFAULT( 5000 ); // Default Nagle delay

SDT_EXTERNAL int32 steamdatagram_timeout_seconds_connected SDT_DEFAULT( 10 );
SDT_EXTERNAL int32 steamdatagram_timeout_seconds_initial SDT_DEFAULT( 10 );

// Don't automatically fail some IP connections that don't have full security,
// push the decision up to the application level.
SDT_EXTERNAL int32 steamdatagram_ip_allow_connections_without_auth
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	SDT_DEFAULT( 1 );
#else
	SDT_DEFAULT( 0 );
#endif

// Code of relay cluster to use.  If not empty, we will only use relays in that cluster.  E.g. 'iad'
SDT_EXTERNAL std::string steamdatagram_client_force_relay_cluster SDT_DEFAULT( "" );

// For debugging, generate our own (unsigned) ticket, using the specified 
// gameserver address.  Router must be configured to accept unsigned tickets.
SDT_EXTERNAL std::string steamdatagram_client_debugticket_address SDT_DEFAULT( "" );

// For debugging.  Override list of relays from the config with this set
// (maybe just one).  Comma-separated list."
SDT_EXTERNAL std::string steamdatagram_client_forceproxyaddr SDT_DEFAULT( "" );

} // SteamNetworkingSocketsLib
