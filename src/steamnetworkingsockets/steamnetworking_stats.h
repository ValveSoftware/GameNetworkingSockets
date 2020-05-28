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

namespace SteamNetworkingSocketsLib {

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

/// Counts of ping times by bucket
struct PingHistogram
{
	int m_n25, m_n50, m_n75, m_n100, m_n125, m_n150, m_n200, m_n300, m_nMax;

	void Reset() { memset( this, 0, sizeof(*this) ); }

	void AddSample( int nPingMS )
	{

		// Update histogram using hand-rolled sort-of-binary-search, optimized
		// for the expectation that most pings will be reasonable
		if ( nPingMS <= 100 )
		{
			if ( nPingMS <= 50 )
			{
				if ( nPingMS <= 25 )
					++m_n25;
				else
					++m_n50;
			}
			else
			{
				if ( nPingMS <= 75 )
					++m_n75;
				else
					++m_n100;
			}
		}
		else
		{
			if ( nPingMS <= 150 )
			{
				if ( nPingMS <= 125 )
					++m_n125;
				else
					++m_n150;
			}
			else
			{
				if ( nPingMS <= 200 )
					++m_n200;
				else if ( nPingMS <= 300 )
					++m_n300;
				else
					++m_nMax;
			}
		}
	}

	inline int TotalCount() const
	{
		return m_n25 + m_n50 + m_n75 + m_n100 + m_n125 + m_n150 + m_n200 + m_n300 + m_nMax;
	}
};

/// Count of quality measurement intervals by bucket
struct QualityHistogram
{
	int m_n100, m_n99, m_n97, m_n95, m_n90, m_n75, m_n50, m_n1, m_nDead;

	void Reset() { memset( this, 0, sizeof(*this) ); }

	inline int TotalCount() const
	{
		return m_n100 + m_n99 + m_n97 + m_n95 + m_n90 + m_n75 + m_n50 + m_n1 + m_nDead;
	}
};

/// Counts of jitter values by bucket
struct JitterHistogram
{
	void Reset() { memset( this, 0, sizeof(*this) ); }

	int m_nNegligible; // <1ms
	int m_n1; // 1--2ms
	int m_n2; // 2--5ms
	int m_n5; // 5--10ms
	int m_n10; // 10--20ms
	int m_n20; // 20ms or more

	void AddSample( SteamNetworkingMicroseconds usecJitter )
	{

		// Add to histogram
		if ( usecJitter < 1000 )
			++m_nNegligible;
		else if ( usecJitter < 2000 )
			++m_n1;
		else if ( usecJitter < 5000 )
			++m_n2;
		else if ( usecJitter < 10000 )
			++m_n5;
		else if ( usecJitter < 20000 )
			++m_n10;
		else
			++m_n20;
	}

	inline int TotalCount() const
	{
		return m_nNegligible + m_n1 + m_n2 + m_n5 + m_n10 + m_n20;
	}
};

/// Stats for the lifetime of a connection.
/// Should match CMsgSteamDatagramLinkLifetimeStats
struct SteamDatagramLinkLifetimeStats
{
	/// Reset all values to zero / unknown status
	void Clear();

	int m_nConnectedSeconds; // -1 if we don't track it

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

	// Ping distribution
	PingHistogram m_pingHistogram;

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
	QualityHistogram m_qualityHistogram;

	// Distribution.  Some might be -1, see above for why.
	short m_nQualityNtile2nd; // 2% of measurement intervals had quality <= N%
	short m_nQualityNtile5th; // 5% of measurement intervals had quality <= N%
	short m_nQualityNtile25th; // 25% of measurement intervals had quality <= N%
	short m_nQualityNtile50th; // 50% of measurement intervals had quality <= N%

	// Jitter histogram
	JitterHistogram m_jitterHistogram;

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

/// Describe detailed state of current connection
struct SteamNetworkingDetailedConnectionStatus
{
	/// Basic connection info
	SteamNetConnectionInfo_t m_info;

	/// Do we have a valid network configuration?  We cannot do anything without this.
	ESteamNetworkingAvailability m_eAvailNetworkConfig;

//		/// Does it look like we have a connection to the Internet at all?
//		EAvailability m_eAvailInternet;

	/// Successful communication with a box on the routing network.
	/// This will be marked as failed if there is a general internet
	/// connection.
	ESteamNetworkingAvailability m_eAvailAnyRouterCommunication;

	/// End-to-end communication with the remote host.
	//ESteamNetworkingAvailability m_eAvailEndToEnd;

	/// Stats for end-to-end link to the gameserver
	SteamDatagramLinkStats m_statsEndToEnd;

	/// Currently selected front router, if any.
	/// Note that PoP ID can be found in the SteamNetConnectionInfo_t
	char m_szPrimaryRouterName[64];
	SteamNetworkingIPAddr m_addrPrimaryRouter;

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
	SteamNetworkingIPAddr m_addrBackupRouter;

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

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKING_STATS_H
