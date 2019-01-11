//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Some public types for communicating detailed connection stats
//
//=============================================================================

#ifndef STEAMNETWORKING_STATS_H
#define STEAMNETWORKING_STATS_H
#ifdef _WIN32
#pragma once
#endif

#include <steam/steamnetworkingtypes.h>

#pragma pack(push)
#pragma pack(8)

struct SteamDatagramLinkStats;
struct SteamDatagramLinkLifetimeStats;
struct SteamDatagramLinkInstantaneousStats;
struct SteamNetworkingDetailedConnectionStatus;

/// Instantaneous statistics for a link between two hosts.
struct SteamDatagramLinkInstantaneousStats
{

	/// Data rates
	float m_flOutPacketsPerSec;
	float m_flOutBytesPerSec;
	float m_flInPacketsPerSec;
	float m_flInBytesPerSec;

	/// Smoothed ping.  This will be -1 if we don't have any idea!
	int m_nPingMS;

	/// 0...1, estimated number of packets that were sent to us, but we failed to receive.
	/// <0 if we haven't received any sequenced packets and so we don't have any way to estimate this.
	float m_flPacketsDroppedPct;

	/// Packets received with a sequence number abnormality, other than basic packet loss.  (Duplicated, out of order, lurch.)
	/// <0 if we haven't received any sequenced packets and so we don't have any way to estimate this.
	float m_flPacketsWeirdSequenceNumberPct;

	/// Peak jitter
	int m_usecMaxJitter;

	/// Current sending rate, this can be low at connection start until the slow start
	/// ramps it up.  It's adjusted as packets are lost and congestion is encountered during
	/// the connection
	int m_nSendRate;

	/// How many pending bytes are waiting to be sent.  This is data that is currently waiting 
	/// to be sent and in outgoing buffers.  If this is zero, then the connection is idle
	/// and all pending data has been sent.  Note that in case of packet loss any pending
	/// reliable data might be re-sent.  This does not include data that has been sent and is
	/// waiting for acknowledgment.
	int m_nPendingBytes;

	/// Reset all values to zero / unknown status
	void Clear();
};

/// Stats for the lifetime of a connection.
/// Should match CMsgSteamDatagramLinkLifetimeStats
struct SteamDatagramLinkLifetimeStats
{
	/// Reset all values to zero / unknown status
	void Clear();

	//
	// Lifetime counters.
	// NOTE: Average packet loss, etc can be deduced from this.
	//
	int64 m_nPacketsSent;
	int64 m_nBytesSent;
	int64 m_nPacketsRecv; // total number of packets received, some of which might not have had a sequence number.  Don't use this number to try to estimate lifetime packet loss, use m_nPacketsRecvSequenced
	int64 m_nBytesRecv;
	int64 m_nPktsRecvSequenced; // packets that we received that had a sequence number.
	int64 m_nPktsRecvDropped;
	int64 m_nPktsRecvOutOfOrder;
	int64 m_nPktsRecvDuplicate;
	int64 m_nPktsRecvSequenceNumberLurch;

	// SNP message counters
	int64 m_nMessagesSentReliable;
	int64 m_nMessagesSentUnreliable;
	int64 m_nMessagesRecvReliable;
	int64 m_nMessagesRecvUnreliable;

	//
	// Ping distribution
	//
	int m_nPingHistogram25; // 0..25
	int m_nPingHistogram50; // 26..50
	int m_nPingHistogram75; // 51..75
	int m_nPingHistogram100; // etc
	int m_nPingHistogram125;
	int m_nPingHistogram150;
	int m_nPingHistogram200;
	int m_nPingHistogram300;
	int m_nPingHistogramMax; // >300
	inline int PingHistogramTotalCount() const
	{
		return m_nPingHistogram25
			+ m_nPingHistogram50
			+ m_nPingHistogram75
			+ m_nPingHistogram100
			+ m_nPingHistogram125
			+ m_nPingHistogram150
			+ m_nPingHistogram200
			+ m_nPingHistogram300
			+ m_nPingHistogramMax;
	}

	// Distribution.
	// NOTE: Some of these might be -1 if we didn't have enough data to make a meaningful estimate!
	// It takes fewer samples to make an estimate of the median than the 98th percentile!
	short m_nPingNtile5th; // 5% of ping samples were <= Nms
	short m_nPingNtile50th; // 50% of ping samples were <= Nms
	short m_nPingNtile75th; // 70% of ping samples were <= Nms
	short m_nPingNtile95th; // 95% of ping samples were <= Nms
	short m_nPingNtile98th; // 98% of ping samples were <= Nms
	short m__pad1;


	//
	// Connection quality distribution
	//
	int m_nQualityHistogram100; // This means everything was perfect.  If we delivered over 100 packets in the interval and were less than perfect, but greater than 99.5%, we will use 99% instead.
	int m_nQualityHistogram99; // 99%+
	int m_nQualityHistogram97;
	int m_nQualityHistogram95;
	int m_nQualityHistogram90;
	int m_nQualityHistogram75;
	int m_nQualityHistogram50;
	int m_nQualityHistogram1;
	int m_nQualityHistogramDead; // we received nothing during the interval; it looks like the connection dropped
	inline int QualityHistogramTotalCount() const
	{
		return m_nQualityHistogram100
			+ m_nQualityHistogram99
			+ m_nQualityHistogram97
			+ m_nQualityHistogram95
			+ m_nQualityHistogram90
			+ m_nQualityHistogram75
			+ m_nQualityHistogram50
			+ m_nQualityHistogram1
			+ m_nQualityHistogramDead;
	}

	// Distribution.  Some might be -1, see above for why.
	short m_nQualityNtile2nd; // 2% of measurement intervals had quality <= N%
	short m_nQualityNtile5th; // 5% of measurement intervals had quality <= N%
	short m_nQualityNtile25th; // 25% of measurement intervals had quality <= N%
	short m_nQualityNtile50th; // 50% of measurement intervals had quality <= N%

	// Jitter histogram
	int m_nJitterHistogramNegligible;
	int m_nJitterHistogram1;
	int m_nJitterHistogram2;
	int m_nJitterHistogram5;
	int m_nJitterHistogram10;
	int m_nJitterHistogram20;
	inline int JitterHistogramTotalCount() const
	{
		return m_nJitterHistogramNegligible
			+ m_nJitterHistogram1
			+ m_nJitterHistogram2
			+ m_nJitterHistogram5
			+ m_nJitterHistogram10
			+ m_nJitterHistogram20;
	}

	//
	// Connection transmit speed histogram
	//
	int m_nTXSpeedMax; // Max speed we hit

	int m_nTXSpeedHistogram16; // Speed at kb/s
	int m_nTXSpeedHistogram32; 
	int m_nTXSpeedHistogram64;
	int m_nTXSpeedHistogram128;
	int m_nTXSpeedHistogram256;
	int m_nTXSpeedHistogram512;
	int m_nTXSpeedHistogram1024;
	int m_nTXSpeedHistogramMax;
	inline int TXSpeedHistogramTotalCount() const
	{
		return m_nTXSpeedHistogram16
			+ m_nTXSpeedHistogram32
			+ m_nTXSpeedHistogram64
			+ m_nTXSpeedHistogram128 
			+ m_nTXSpeedHistogram256 
			+ m_nTXSpeedHistogram512 
			+ m_nTXSpeedHistogram1024
			+ m_nTXSpeedHistogramMax;
	}

	// Distribution.  Some might be -1, see above for why.
	int m_nTXSpeedNtile5th; // 5% of transmit samples were <= N kb/s
	int m_nTXSpeedNtile50th; // 50% of transmit samples were <= N kb/s 
	int m_nTXSpeedNtile75th; // 75% of transmit samples were <= N kb/s 
	int m_nTXSpeedNtile95th; // 95% of transmit samples were <= N kb/s 
	int m_nTXSpeedNtile98th; // 98% of transmit samples were <= N kb/s 

	//
	// Connection receive speed histogram
	//
	int m_nRXSpeedMax; // Max speed we hit that formed the histogram

	int m_nRXSpeedHistogram16; // Speed at kb/s
	int m_nRXSpeedHistogram32; 
	int m_nRXSpeedHistogram64;
	int m_nRXSpeedHistogram128;
	int m_nRXSpeedHistogram256;
	int m_nRXSpeedHistogram512;
	int m_nRXSpeedHistogram1024;
	int m_nRXSpeedHistogramMax;
	inline int RXSpeedHistogramTotalCount() const
	{
		return m_nRXSpeedHistogram16
			+ m_nRXSpeedHistogram32
			+ m_nRXSpeedHistogram64
			+ m_nRXSpeedHistogram128
			+ m_nRXSpeedHistogram256
			+ m_nRXSpeedHistogram512
			+ m_nRXSpeedHistogram1024
			+ m_nRXSpeedHistogramMax;
	}

	// Distribution.  Some might be -1, see above for why.
	int m_nRXSpeedNtile5th; // 5% of transmit samples were <= N kb/s
	int m_nRXSpeedNtile50th; // 50% of transmit samples were <= N kb/s 
	int m_nRXSpeedNtile75th; // 75% of transmit samples were <= N kb/s 
	int m_nRXSpeedNtile95th; // 95% of transmit samples were <= N kb/s 
	int m_nRXSpeedNtile98th; // 98% of transmit samples were <= N kb/s 

};

/// Link stats.  Pretty much everything you might possibly want to know about the connection
struct SteamDatagramLinkStats
{

	/// Latest instantaneous stats, calculated locally
	SteamDatagramLinkInstantaneousStats m_latest;

	/// Peak values for each instantaneous stat
	//SteamDatagramLinkInstantaneousStats m_peak;

	/// Lifetime stats, calculated locally
	SteamDatagramLinkLifetimeStats m_lifetime;

	/// Latest instantaneous stats received from remote host.
	/// (E.g. "sent" means they are reporting what they sent.)
	SteamDatagramLinkInstantaneousStats m_latestRemote;

	/// How many seconds ago did we receive m_latestRemote?
	/// This will be <0 if the data is not valid!
	float m_flAgeLatestRemote;

	/// Latest lifetime stats received from remote host.
	SteamDatagramLinkLifetimeStats m_lifetimeRemote;

	/// How many seconds ago did we receive the lifetime stats?
	/// This will be <0 if the data is not valid!
	float m_flAgeLifetimeRemote;

	/// Reset everything to unknown/initial state.
	void Clear();
};

/// Status of a particular network resource
enum ESteamDatagramAvailability
{
	k_ESteamDatagramAvailability_CannotTry = -3,		// A dependent resource is missing, so this service is unavailable.  (E.g. we cannot talk to routers because Internet is down or we don't have the network config.)
	k_ESteamDatagramAvailability_Failed = -2,			// We have tried for enough time that we would expect to have been successful by now.  We have never been successful
	k_ESteamDatagramAvailability_Previously = -1,		// We tried and were successful at one time, but now it looks like we have a problem
	k_ESteamDatagramAvailability_Unknown = 0,			// Unknown, or not applicable in this context
	k_ESteamDatagramAvailability_NeverTried = 1,		// We don't know because we haven't ever checked
	k_ESteamDatagramAvailability_Attempting = 2,		// We're trying now, but are not yet successful.  This is not an error, but it's not success, either.
	k_ESteamDatagramAvailability_Current = 3,			// Resource is online.
};

/// Describe detailed state of current connection
struct SteamNetworkingDetailedConnectionStatus
{
	/// Basic connection info
	SteamNetConnectionInfo_t m_info;

	/// Do we have a valid network configuration?  We cannot do anything without this.
	ESteamDatagramAvailability m_eAvailNetworkConfig;

//		/// Does it look like we have a connection to the Internet at all?
//		EAvailability m_eAvailInternet;

	/// Successful communication with a box on the routing network.
	/// This will be marked as failed if there is a general internet
	/// connection.
	ESteamDatagramAvailability m_eAvailAnyRouterCommunication;

	/// End-to-end communication with the remote host.
	//ESteamDatagramAvailability m_eAvailEndToEnd;

	/// Stats for end-to-end link to the gameserver
	SteamDatagramLinkStats m_statsEndToEnd;

	/// Currently selected front router, if any.
	/// Note that PoP ID can be found in the SteamNetConnectionInfo_t
	char m_szPrimaryRouterName[64];
	uint32 m_unPrimaryRouterIP;
	uint16 m_unPrimaryRouterPort;

	/// Stats for "front" link to current router
	SteamDatagramLinkStats m_statsPrimaryRouter;

	/// Back ping time as reported by primary.
	/// (The front ping is in m_statsPrimaryRouter,
	/// and usually the front ping plus the back ping should
	/// approximately equal the end-to-end ping)
	int m_nPrimaryRouterBackPing;

	/// Currently selected back router, if any
	SteamNetworkingPOPID m_idBackupRouterCluster;
	char m_szBackupRouterName[64];
	uint32 m_unBackupRouterIP;
	uint16 m_unBackupRouterPort;

	/// Ping times to backup router, if any
	int m_nBackupRouterFrontPing, m_nBackupRouterBackPing;

	/// Clear everything to an unknown state
	void Clear();

	/// Print into a buffer.
	/// 0 = OK
	/// >1 = buffer was null or too small (in which case truncation happened).
	/// Pass a buffer of at least N bytes.
	int Print( char *pszBuf, int cbBuf );
};

#pragma pack(pop)

#endif // STEAMNETWORKING_STATS_H
