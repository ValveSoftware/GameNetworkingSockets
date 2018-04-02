//====== Copyright Valve Corporation, All rights reserved. ====================
//
// COmmon stuff used by SteamNetworkingSockets code
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS_INTERNAL_H
#define STEAMNETWORKINGSOCKETS_INTERNAL_H
#ifdef _WIN32
#pragma once
#endif

// Socket headers
#ifdef WIN32
	//#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#define MSG_NOSIGNAL 0
#else
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <sys/ioctl.h>
	#include <unistd.h>
	#include <poll.h>
	#include <errno.h>

	#ifndef closesocket
		#define closesocket close
	#endif
	#ifndef ioctlsocket
		#define ioctlsocket ioctl
	#endif
	#define WSAEWOULDBLOCK EWOULDBLOCK
#endif

// Public shared stuff
#include <tier0/basetypes.h>
#include <tier0/t0constants.h>
#include <tier0/platform.h>
#include <steamnetworkingsockets/steamnetworkingtypes.h>
#include <tier1/netadr.h>
#include <vstdlib/strtools.h>
#include <tier1/utlvector.h>
#include <tier1/utlbuffer.h>
#include "keypair.h"

// Messages
#include <tier0/memdbgoff.h>
#include <steamnetworkingsockets_messages_certs.pb.h>
#include <tier0/memdbgon.h>

// Redefine the macros for byte-swapping, to sure the correct
// argument size.  We probably should move this into platform.h,
// but I suspect we'd find a bunch of "bugs" which currently don't
// matter because we don't support any big endian platforms right now
#ifdef PLAT_LITTLE_ENDIAN
	#undef LittleWord
	template <typename T>
	inline T LittleWord( T x )
	{
		COMPILE_TIME_ASSERT(sizeof(T) == 2);
		return x;
	}

	#undef LittleDWord
	template <typename T>
	inline T LittleDWord( T x )
	{
		COMPILE_TIME_ASSERT(sizeof(T) == 4);
		return x;
	}

	#undef LittleQWord
	template <typename T>
	inline T LittleQWord( T x )
	{
		COMPILE_TIME_ASSERT(sizeof(T) == 8);
		return x;
	}
#endif

#ifdef WIN32
// Define iovec with the same field names as the POSIX one, but with the same memory layout
// as Winsock WSABUF thingy
struct iovec
{
	unsigned long iov_len;
	void *iov_base;
};
#else
struct iovec;
#endif

struct SteamDatagramLinkStats;
struct SteamDatagramLinkLifetimeStats;
struct SteamDatagramLinkInstantaneousStats;
struct SteamNetworkingDetailedConnectionStatus;

// Internal stuff goes in a private namespace
namespace SteamNetworkingSocketsLib {

inline int GetLastSocketError()
{
	#ifdef WIN32
		return (int)WSAGetLastError();
	#else
		return errno;
	#endif
}

/// Max size of UDP payload.  Includes API payload and
/// any headers, but does not include IP/UDP headers
/// (IP addresses, ports, checksum, etc.
const int k_cbSteamNetworkingSocketsMaxUDPMsgLen = 1300;

/// Max message size that we promise that we can send without fragmenting
/// Should we promote this to a public header?  It does seems like an
/// important API parameter.
const int k_cbSteamNetworkingSocketsMaxMessageNoFragment = 1200;

/// Max length of encrypted payload we will send.  Must be multiple of
/// the encryption key length.  (Currently 32-byte AES keys.)
const int k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend = 1248;

/// Max length of plaintext payload we could send.  We need at least
/// one byte of pad, so an exact multiple of the key size would
/// basically be guaranteeing a full key's worth of pad and would
/// be bad.  Leaving exactly one byte would be OK.  Maybe that's
/// ideal?  For some reason I'd like to use a more round number.  That might be misguided, but it feels right.
const int k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend-4;

/// Use larger limits for what we are willing to receive.
const int k_cbSteamNetworkingSocketsMaxEncryptedPayloadRecv = k_cbSteamNetworkingSocketsMaxUDPMsgLen;
const int k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv = k_cbSteamNetworkingSocketsMaxUDPMsgLen;

/// Make sure we have enough room for our headers and occasional inline pings and stats and such
COMPILE_TIME_ASSERT( k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend + 50 < k_cbSteamNetworkingSocketsMaxUDPMsgLen );

/// Min size of raw UDP message.
const int k_nMinSteamDatagramUDPMsgLen = 5;

/// When sending a stats message, what sort of reply is requested by the calling code?
enum EStatsReplyRequest
{
	k_EStatsReplyRequest_None,
	k_EStatsReplyRequest_DelayedOK,
	k_EStatsReplyRequest_Immediate,
};

/// Max time that we we should "Nagle" an ack, hoping to combine them together or
/// piggy back on another outgoing message, before sending a standalone message.
const SteamNetworkingMicroseconds k_usecMaxAckDelay = 250*1000;

/// Precision of the delay ack delay values we send.  A packed value of 1 represents 2^N microseconds
const unsigned k_usecAckDelayPacketSerializedPrecisionShift = 6;
COMPILE_TIME_ASSERT( ( (k_usecMaxAckDelay*2) >> k_usecAckDelayPacketSerializedPrecisionShift ) < 0x4000 ); // Make sure we varint encode in 2 bytes, even if we overshoot a factor of 2x

/// After a connection is closed, a session will hang out in a CLOSE_WAIT-like
/// (or perhaps FIN_WAIT?) state to handle last stray packets and help both sides
/// close cleanly.
const SteamNetworkingMicroseconds k_usecSteamDatagramRouterCloseWait = k_nMillion*15;

// Internal reason codes
const int k_ESteamNetConnectionEnd_InternalRelay_SessionIdleTimeout = 9001;
const int k_ESteamNetConnectionEnd_InternalRelay_ClientChangedTarget = 9002;

/// Timeout value for pings.  This basically determines the retry rate for pings.
/// If a ping is longer than this, then really, the server should not probably not be
/// considered available.
const SteamNetworkingMicroseconds k_usecSteamDatagramClientPingTimeout = 0.750f * k_nMillion;

/// Keepalive interval for currently selected router.  We send keepalive pings when
/// we haven't heard anything from the router in a while, to see if we need
/// to re-route.
const SteamNetworkingMicroseconds k_usecSteamDatagramClientPrimaryRouterKeepaliveInterval = 1 * k_nMillion;

/// Keepalive interval for backup routers.  We send keepalive pings to
/// make sure our backup session still exists and we could switch to it
/// if it became necessary
const SteamNetworkingMicroseconds k_usecSteamDatagramClientBackupRouterKeepaliveInterval = 45 * k_nMillion;

/// Keepalive interval for gameserver.  We send keepalive pings when we haven't
/// heard anything from the gameserver in a while, in order to try and deduce
/// where the router or gameserver are available.
const SteamNetworkingMicroseconds k_usecSteamDatagramClientServerKeepaliveInterval = 1 * k_nMillion;

/// Timeout value for session request messages
const SteamNetworkingMicroseconds k_usecSteamDatagramClientSessionRequestTimeout = 0.750f * k_nMillion;

/// Router will continue to pend a client ping request for N microseconds,
/// hoping for an opportunity to send it inline.
const SteamNetworkingMicroseconds k_usecSteamDatagramRouterPendClientPing = 0.200*k_nMillion;

/// When serializing a "time since I last sent a packet" value into the packet,
/// what precision is used?  (A serialized value of 1 = 2^N microseconds.)
const unsigned k_usecTimeSinceLastPacketSerializedPrecisionShift = 4;

/// "Time since last packet sent" values should be less than this.
/// Any larger value will be discarded, and should not be sent
const uint32 k_usecTimeSinceLastPacketMaxReasonable = k_nMillion/4;
COMPILE_TIME_ASSERT( ( k_usecTimeSinceLastPacketMaxReasonable >> k_usecTimeSinceLastPacketSerializedPrecisionShift ) < 0x8000 ); // make sure all "reasonable" values can get serialized into 16-bits

///	Don't send spacing values when packets are sent extremely close together.
const uint32 k_usecTimeSinceLastPacketMinReasonable = k_nMillion/250;
COMPILE_TIME_ASSERT( ( k_usecTimeSinceLastPacketMinReasonable >> k_usecTimeSinceLastPacketSerializedPrecisionShift ) > 64 ); // make sure the minimum reasonable value can be serialized with sufficient precision.

/// What universe are we running in?  Set at init time
extern EUniverse g_eUniverse;

/// Protocol version of this code
const uint32 k_nCurrentProtocolVersion = 4;
const uint32 k_nMinRequiredProtocolVersion = 4;

// Serialize an UNSIGNED quantity.  Returns pointer to the next byte.
// https://developers.google.com/protocol-buffers/docs/encoding
template <typename T>
inline byte *SerializeVarInt( byte *p, T x )
{
	while ( x >= (unsigned)0x80 ) // if you get a warning, it's because you are using a signed type!  Don't use this for signed data!
	{
		// Truncate to 7 bits, and turn on the high bit, and write it.
		*(p++) = byte( x | 0x80 );

		// Move on to the next higher order bits.
		x >>= 7U;
	}
	*p = x;
	return p+1;
}

/// Serialize a bar int, but return null if we want to go past the end
template <typename T>
inline byte *SerializeVarInt( byte *p, T x, const byte *pEnd )
{
	while ( x >= (unsigned)0x80 ) // if you get a warning, it's because you are using a signed type!  Don't use this for signed data!
	{
		if ( p >= pEnd )
			return nullptr;

		// Truncate to 7 bits, and turn on the high bit, and write it.
		*(p++) = byte( x | 0x80 );

		// Move on to the next higher order bits.
		x >>= 7U;
	}
	if ( p >= pEnd )
		return nullptr;
	*p = x;
	return p+1;
}

inline int VarIntSerializedSize( uint32 x )
{
	if ( x < (1U<<7) ) return 1;
	if ( x < (1U<<14) ) return 2;
	if ( x < (1U<<21) ) return 3;
	if ( x < (1U<<28) ) return 4;
	return 5;
}

inline int VarIntSerializedSize( uint64 x )
{
	if ( x < (1LLU<<35) )
	{
		if ( x < (1LLU<<7) ) return 1;
		if ( x < (1LLU<<14) ) return 2;
		if ( x < (1LLU<<21) ) return 3;
		if ( x < (1LLU<<28) ) return 4;
		return 5;
	}
	if ( x < (1LLU<<42) ) return 6;
	if ( x < (1LLU<<49) ) return 7;
	if ( x < (1LLU<<56) ) return 8;
	if ( x < (1LLU<<63) ) return 9;
	return 10;
}

// De-serialize a var-int encoded quantity.  Returns pointer to the next byte,
// or NULL if there was a decoding error (we hit the end of stream.)
// https://developers.google.com/protocol-buffers/docs/encoding
template <typename T>
inline const byte *DeserializeVarInt( const byte *p, const byte *end, T &x )
{
	if ( p >= end )
		return nullptr;
	T nResult = *p & 0x7f; // use local variable for working, to make sure compiler doesn't try to worry about pointer aliasing
	unsigned nShift = 7;
	while ( *(p++) & 0x80 )
	{
		if ( p >= end )
			return nullptr;
		nResult |= ( T( *p & 0x7f ) << nShift );
		nShift += 7;
	}
	x = nResult;
	return p;
}

void LinkStatsPrintInstantaneousToBuf( const char *pszLeader, const SteamDatagramLinkInstantaneousStats &stats, CUtlBuffer &buf );
void LinkStatsPrintLifetimeToBuf( const char *pszLeader, const SteamDatagramLinkLifetimeStats &stats, CUtlBuffer &buf );
void LinkStatsPrintToBuf( const char *pszLeader, const SteamDatagramLinkStats &stats, CUtlBuffer &buf );

class NumberPrettyPrinter
{
public:
	NumberPrettyPrinter( int64 val ) { Print(val); }
	void Print( int64 val )
	{
		char *d = m_buf;
		if ( val < 0 )
		{
			*(d++) = '-';
			val = -val;
		}
		// Largest 64-bit (0x7fffffffffffffff) = 9,223,372,036,854,775,807
		// which is 19 digits.
		COMPILE_TIME_ASSERT( INT64_MAX > (int64)1e18 );
		COMPILE_TIME_ASSERT( INT64_MAX/10 < (int64)1e18 );
		int arnGroupsOfThree[6];
		int nGroupsOfThree = 0;
		while ( val >= 1000 )
		{
			arnGroupsOfThree[nGroupsOfThree++] = val % 1000;
			val /= 1000;
		}
		int iVal = int( val ); // make sure compiler knows it can do 32-bit math
		if ( iVal >= 100 ) { *(d++) = char( iVal/100 + '0' ); iVal %= 100; }
		if ( iVal >= 10 ) { *(d++) = char( iVal/10 + '0' ); iVal %= 10; }
		*(d++) = char( iVal + '0' );
		while ( nGroupsOfThree > 0 )
		{
			int iThreeDigits = arnGroupsOfThree[--nGroupsOfThree];
			*(d++) = ',';
			*(d++) = char( iThreeDigits/100 ) + '0'; iThreeDigits %= 100;
			*(d++) = char( iThreeDigits/10 ) + '0'; iThreeDigits %= 10;
			*(d++) = char( iThreeDigits ) + '0';
		}

		*d = '\0';
	}
	inline const char *String() const { return m_buf; }
private:
	char m_buf[64];
};

/// Used for fast hashes that are reasonably secure
extern uint64_t siphash( const uint8_t *in, uint64_t inlen, const uint8_t *k );

/// Indent each line of a string
extern std::string Indent( const char *s );
inline std::string Indent( const std::string &s ) { return Indent( s.c_str() ); }

/// Generate a fingerprint for a public that is reasonably collision resistant,
/// although not really cryptographically secure.  (We are in charge of the
/// set of public keys and we expect it to be reasonably small.)
#ifdef SDR_SUPPORT_RSA_TICKETS
extern uint64 CalculatePublicKeyID( const CRSAPublicKey &pubKey );
#endif
extern uint64 CalculatePublicKeyID( const CECSigningPublicKey &pubKey );

} // namespace SteamNetworkingSocketsLib

//
// Some misc tools for using std::vector that our CUtlVector class had
//

struct IndexRange
{
	struct Iter
	{
		int i;
		int operator*() const { return i; }
		void operator++() { ++i; }
		inline bool operator==( const Iter &x) const { return i == x.i; }
		inline bool operator!=( const Iter &x) const { return i != x.i; }
	};

	int m_nBegin, m_nEnd;
	Iter begin() const { return Iter{m_nBegin}; }
	Iter end() const { return Iter{m_nEnd}; }
};

template <typename T>
inline IndexRange iter_indices( const std::vector<T> &vec )
{
	return IndexRange{ 0, (int)vec.size() };
}

template <typename T>
inline void erase_at( std::vector<T> &vec, int idx )
{
	vec.erase( vec.begin()+idx );
}

template <typename T>
inline int push_back_get_idx( std::vector<T> &vec )
{
	vec.resize( vec.size()+1 ); return int( vec.size()-1 );
}

template <typename T>
inline int push_back_get_idx( std::vector<T> &vec, const T &x )
{
	vec.push_back( x ); return int( vec.size()-1 );
}

template <typename T>
inline T *push_back_get_ptr( std::vector<T> &vec )
{
	vec.resize( vec.size()+1 ); return &vec[ vec.size()-1 ];
}

template <typename T>
inline T *push_back_get_ptr( std::vector<T> &vec, const T &x )
{
	vec.push_back( x ); return &vec[ vec.size()-1 ];
}

// Return size as an *int*, not size_t, which is totally pedantic useless garbage in 99% of code.
template <typename T>
inline int len( const std::vector<T> &vec )
{
	return (int)vec.size();
}

inline int len( const std::string &str )
{
	return (int)str.length();
}

template< typename T >
inline bool has_element( const std::vector<T> &vec, const T&x )
{
	return std::find( vec.begin(), vec.end(), x ) != vec.end();
}

#endif // STEAMNETWORKINGSOCKETS_INTERNAL_H
