//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Common stuff used by SteamNetworkingSockets code
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS_INTERNAL_H
#define STEAMNETWORKINGSOCKETS_INTERNAL_H
#pragma once

// Public shared stuff
#include <tier0/basetypes.h>
#include <tier0/t0constants.h>
#include <tier0/platform.h>
#include <tier0/dbg.h>
#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
	#include <tier0/validator.h>
#endif
#include <steam/steamnetworkingtypes.h>
#include <tier1/netadr.h>
#include <vstdlib/strtools.h>
#include <vstdlib/random.h>
#include <tier1/utlvector.h>
#include <tier1/utlbuffer.h>
#include "keypair.h"
#include <tier0/memdbgoff.h>
#include <steamnetworkingsockets_messages_certs.pb.h>
#include <steam/isteamnetworkingutils.h> // for the rendering helpers

#include <tier0/memdbgon.h>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE
	#define STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW \
		static void* operator new( size_t s ) noexcept { return malloc( s ); } \
		static void* operator new[]( size_t ) = delete; \
		static void operator delete( void *p ) noexcept { free( p ); } \
		static void operator delete[]( void * ) = delete;
#else
	#define STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
#endif

// Always enable ISteamNetworkingMessages on PC, unless it is specifically
// disabled.  On console, it's opt *in*
#if !defined( STEAMNETWORKINGSOCKETS_DISABLE_STEAMNETWORKINGMESSAGES ) && !defined( STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES ) && !IsConsole()
	#define STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
#endif

// STEAMNETWORKINGSOCKETS_STEAM: we have some capability of talking to the
// running steam client.
#if !defined( STEAMNETWORKINGSOCKETS_STEAM ) && !defined( STEAMNETWORKINGSOCKETS_NOSTEAM )
	#if defined( STEAMNETWORKINGSOCKETS_OPENSOURCE ) || defined( STEAMNETWORKINGSOCKETS_STREAMINGCLIENT ) || IsConsole()
		#define STEAMNETWORKINGSOCKETS_NOSTEAM
	#else
		#define STEAMNETWORKINGSOCKETS_STEAM
	#endif
#endif
#if defined( STEAMNETWORKINGSOCKETS_STEAM ) && defined( STEAMNETWORKINGSOCKETS_NOSTEAM )
	#error "Can't define both STEAMNETWORKINGSOCKETS_STEAM and STEAMNETWORKINGSOCKETS_NOSTEAM"
#endif

// STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP: can we allocate or talk to FakeIP addresses?
#ifndef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP

	// FakeIP system can only be used on Steam
	#ifdef STEAMNETWORKINGSOCKETS_STEAM
		#define STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
	#endif
#endif
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
	#include "sdr/steamdatagram_fakeip.h"
#endif

// STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT: We have some ability to request a cert
#ifdef STEAMNETWORKINGSOCKETS_STEAM
	// STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT means we know how to make a cert request from some sort of certificate authority
	#define STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
#endif

// STEAMNETWORKINGSOCKETS_ENABLE_ICE: Enable NAT-punch for P2P connections
#ifndef STEAMNETWORKINGSOCKETS_ENABLE_ICE

	// Always #define STEAMNETWORKINGSOCKETS_ENABLE_ICE in a few places.
	// You can also define it on the command line
	#if defined( STEAMNETWORKINGSOCKETS_STEAMCLIENT ) || defined( STEAMNETWORKINGSOCKETS_STREAMINGCLIENT )
		#define STEAMNETWORKINGSOCKETS_ENABLE_ICE
	#endif
#endif

// STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC: Enable WebRTC as the ICE client
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
	#if defined( STEAMNETWORKINGSOCKETS_STEAMCLIENT ) || defined( STEAMNETWORKINGSOCKETS_STREAMINGCLIENT )
		#define STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC
	#endif
#endif

// STEAMNETWORKINGSOCKETS_ENABLE_RESOLVEHOSTNAME: Do we need to be able to
// resolve hostnames using DNS?  Note that this is done synchronously, so
// it should be avoided when possible, and only used for testing!
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
	#if !IsConsole()
		#define STEAMNETWORKINGSOCKETS_ENABLE_RESOLVEHOSTNAME
	#endif
#endif

// STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI: The environment has some way to
// display/consume realtime connection diagnostics
#ifndef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI

	// Currently only Steam knows what to do with these
	#ifdef STEAMNETWORKINGSOCKETS_STEAM
		#define STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
	#endif
#endif

/// Max number of lanes we support as sender and receiver.  We make this a define, so that
/// you can override it.  If you don't need support for lanes, then we can make a few optimizations
#ifndef STEAMNETWORKINGSOCKETS_MAX_LANES
	#define STEAMNETWORKINGSOCKETS_MAX_LANES 255
#endif
#if STEAMNETWORKINGSOCKETS_MAX_LANES < 1
	#error "Invalid STEAMNETWORKINGSOCKETS_MAX_LANES"
#endif

// Enable DualSTA support on Windows
#if defined(_WINDOWS) && !defined(STEAMNETWORKINGSOCKETS_OPENSOURCE)
	#define STEAMNETWORKINGSOCKETS_ENABLE_DUALWIFI
#endif

// STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW: Enable default logging and spew based on environment variables
#if !IsConsole()
	#define STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
#endif

enum EDualWifiEnable {
	k_nDualWifiEnable_Disable = 0,
	k_nDualWifiEnable_Enable = 1, // 
	k_nDualWifiEnable_DoNotEnumerate = 2, // Enumerate primary adapters, but don't actually try to enable any Dual Wifi support
	k_nDualWifiEnable_DoNotBind = 3, // Try to turn on Dual Wifi and locate the secondary adapter, but don't actually bind
	k_nDualWifiEnable_ForceSimulate = 4, // Don't really do any DualWifi work, just open up another "regular" socket

	k_nDualWifiEnable_MAX = k_nDualWifiEnable_ForceSimulate // Maximum legal value
};

/// Enumerate different kinds of transport that can be used
enum ESteamNetTransportKind
{
	k_ESteamNetTransport_Unknown = 0,
	k_ESteamNetTransport_LoopbackBuffers = 1, // Internal buffers, not using OS network stack
	k_ESteamNetTransport_LocalHost = 2, // Using OS network stack to talk to localhost address
	k_ESteamNetTransport_UDP = 3, // Ordinary UDP connection.
	k_ESteamNetTransport_UDPProbablyLocal = 4, // Ordinary UDP connection over a route that appears to be "local", meaning we think it is probably fast.  This is just a guess: VPNs and IPv6 make this pretty fuzzy.
	k_ESteamNetTransport_TURN = 5, // Relayed over TURN server
	k_ESteamNetTransport_SDRP2P = 6, // P2P connection relayed over Steam Datagram Relay
	k_ESteamNetTransport_SDRHostedServer = 7, // Connection to a server hosted in a known data center via Steam Datagram Relay

	k_ESteamNetTransport_Force32Bit = 0x7fffffff
};

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

#ifdef _WIN32
// Define iovec with the same field names as the POSIX one, but with the same memory layout
// as Winsock WSABUF thingy
struct iovec
{
	unsigned long iov_len;
	void *iov_base;
};
#else
#include <sys/uio.h>
#endif

// likely() and unlikely().  Branch hints
// This is an idiom from the linux kernel
#ifdef __GNUC__
	#ifndef likely
		#define likely(x) __builtin_expect (!!(x), 1)
	#endif
	#ifndef unlikely
		#define unlikely(x) __builtin_expect (!!(x), 0)
	#endif
#else
	#ifndef likely
		#define likely(x) (x)
	#endif
	#ifndef unlikely
		#define unlikely(x) (x)
	#endif
#endif

// Internal stuff goes in a private namespace
namespace SteamNetworkingSocketsLib {

// Determine serialized size of protobuf msg.
// Always return int, because size_t is dumb,
// and unsigned types are from the devil.
template <typename TMsg>
inline int ProtoMsgByteSize( const TMsg &msg )
{
	#if GOOGLE_PROTOBUF_VERSION < 3004000
		return msg.ByteSize();
	#else
		return static_cast<int>( msg.ByteSizeLong() );
	#endif
}

struct SteamDatagramLinkStats;
struct SteamDatagramLinkLifetimeStats;
struct SteamDatagramLinkInstantaneousStats;
struct SteamNetworkingDetailedConnectionStatus;
struct LinkStatsTrackerBase;

// An identity operator that always returns its operand.
// NOTE: std::hash is an identity operator on many compilers
//       for the basic primitives.  If you really need actual
//       hashing, don't use std::hash!
template <typename T >
struct Identity
{
	 const T &operator()( const T &x ) const { return x; }
};

/// Max size of UDP payload.  Includes API payload and
/// any headers, but does not include IP/UDP headers
/// (IP addresses, ports, checksum, etc.
const int k_cbSteamNetworkingSocketsMaxUDPMsgLen = 1300;

/// Do not allow MTU to be set less than this
const int k_cbSteamNetworkingSocketsMinMTUPacketSize = 200;

/// Overhead that we will reserve for stats, etc when calculating the max
/// message that we won't fragment
const int k_cbSteamNetworkingSocketsNoFragmentHeaderReserve = 100;

/// Size of security tag for AES-GCM.
/// It would be nice to use a smaller tag, but BCrypt requires a 16-byte tag,
/// which is what OpenSSL uses by default for TLS.
const int k_cbAESGCMTagSize = 16;

/// Max length of plaintext and encrypted payload we will send.  AES-GCM does
/// not use padding (but it does have the security tag).  So this can be
/// arbitrary, it does not need to account for the block size.
const int k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend = 1248;
const int k_cbSteamNetworkingSocketsTypicalMaxPlaintextPayloadSend = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend-k_cbAESGCMTagSize;

/// Use larger limits for what we are willing to receive.
const int k_cbSteamNetworkingSocketsMaxEncryptedPayloadRecv = k_cbSteamNetworkingSocketsMaxUDPMsgLen;
const int k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv = k_cbSteamNetworkingSocketsMaxUDPMsgLen;

/// Max value that RecvMaxMessageSize can be set to.
const int k_cbMaxMessageSizeRecv_Limit = k_cbMaxSteamNetworkingSocketsMessageSizeSend*2;
COMPILE_TIME_ASSERT( k_cbMaxMessageSizeRecv_Limit >= k_cbMaxSteamNetworkingSocketsMessageSizeSend*2 );

/// If we have a cert that is going to expire in <N seconds, try to renew it
const int k_nSecCertExpirySeekRenew = 3600*2;

/// Make sure we have enough room for our headers and occasional inline pings and stats and such
/// FIXME - For relayed connections, we send some of the stats outside the encrypted block, so that
/// they can be observed by the relay.  For direct connections, we put it in the encrypted block.
/// So we might need to adjust this to be per connection type instead off constant.
COMPILE_TIME_ASSERT( k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend + 50 < k_cbSteamNetworkingSocketsMaxUDPMsgLen );

/// Min size of raw UDP message.
const int k_nMinSteamDatagramUDPMsgLen = 5;

/// When sending a stats message, what sort of reply is requested by the calling code?
enum EStatsReplyRequest
{
	k_EStatsReplyRequest_NothingToSend, // We don't have anything to send at all
	k_EStatsReplyRequest_NoReply, // We have something to send, but it does not require a reply
	k_EStatsReplyRequest_DelayedOK, // We have something to send, but a delayed reply is OK
	k_EStatsReplyRequest_Immediate, // Immediate reply is requested
};

/// Max time that we we should "Nagle" an ack, hoping to combine them together or
/// piggy back on another outgoing message, before sending a standalone message.
const SteamNetworkingMicroseconds k_usecMaxAckStatsDelay = 250*1000;

/// Max duration that a receiver could pend a data ack, in the hopes of trying
/// to piggyback the ack on another outbound packet.
/// !KLUDGE! This really ought to be application- (or connection-) specific.
const SteamNetworkingMicroseconds k_usecMaxDataAckDelay = 50*1000;

/// Precision of the delay ack delay values we send.  A packed value of 1 represents 2^N microseconds
const unsigned k_usecAckDelayPacketSerializedPrecisionShift = 6;
COMPILE_TIME_ASSERT( ( (k_usecMaxAckStatsDelay*2) >> k_usecAckDelayPacketSerializedPrecisionShift ) < 0x4000 ); // Make sure we varint encode in 2 bytes, even if we overshoot a factor of 2x

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
const SteamNetworkingMicroseconds k_usecSteamDatagramClientPingTimeout = 750000;

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
const SteamNetworkingMicroseconds k_usecSteamDatagramClientSessionRequestTimeout = 750000;

/// Router will continue to pend a client ping request for N microseconds,
/// hoping for an opportunity to send it inline.
const SteamNetworkingMicroseconds k_usecSteamDatagramRouterPendClientPing = 200000;

/// When serializing a "time since I last sent a packet" value into the packet,
/// what precision is used?  (A serialized value of 1 = 2^N microseconds.)
const unsigned k_usecTimeSinceLastPacketSerializedPrecisionShift = 4;

/// "Time since last packet sent" values should be less than this.
/// Any larger value will be discarded, and should not be sent
const SteamNetworkingMicroseconds k_usecTimeSinceLastPacketMaxReasonable = k_nMillion/4;
COMPILE_TIME_ASSERT( ( k_usecTimeSinceLastPacketMaxReasonable >> k_usecTimeSinceLastPacketSerializedPrecisionShift ) < 0x8000 ); // make sure all "reasonable" values can get serialized into 16-bits

///	Don't send spacing values when packets are sent extremely close together.  The spacing
/// should be a bit higher that our serialization precision.
const SteamNetworkingMicroseconds k_usecTimeSinceLastPacketMinReasonable = 2 << k_usecTimeSinceLastPacketSerializedPrecisionShift;

/// A really terrible ping score, but one that we can do some math with without overflowing
constexpr int k_nRouteScoreHuge = INT_MAX/8;

/// Protocol version of this code.  This is a blunt instrument, which is incremented when we
/// wish to change the wire protocol in a way that doesn't have some other easy
/// mechanism for dealing with compatibility (e.g. using protobuf's robust mechanisms).
const uint32 k_nCurrentProtocolVersion = 11;

/// Minimum required version we will accept from a peer.  We increment this
/// when we introduce wire breaking protocol changes and do not wish to be
/// backward compatible.  This has been fine before the	first major release,
/// but once we make a big public release, we probably won't ever be able to
/// do this again, and we'll need to have more sophisticated mechanisms. 
const uint32 k_nMinRequiredProtocolVersion = 8;

/// SteamNetworkingMessages is built on top of SteamNetworkingSockets.  We use a reserved
/// virtual port for this interface
const int k_nVirtualPort_Messages = 0x7fffffff;

/// A portion of the virtual port range is carved out for "fake IP ports".
/// These are the *index* of the fake port, not the actual fake port value itself.
/// This is a much bigger reservation of the space than we ever actually expect to
/// use in practice.  Furthermore, these numbers should only be used locally.
/// Outside of the local process, we would always use the actual fake port value.
const int k_nFakePort_MaxGlobalAllocationAttempt = 255;
const int k_nVirtualPort_GlobalFakePort0 = 0x7fffff00;
const int k_nVirtualPort_GlobalFakePortMax = k_nVirtualPort_GlobalFakePort0 + k_nFakePort_MaxGlobalAllocationAttempt - 1;

const int k_nFakePort_MaxEphemeralPorts = 256;
const int k_nVirtualPort_EphemeralFakePort0 = 0x7ffffe00;
const int k_nVirtualPort_EphemeralFakePortMax = k_nVirtualPort_EphemeralFakePort0 + k_nFakePort_MaxEphemeralPorts - 1;

inline bool IsVirtualPortEphemeralFakePort( int nVirtualPort )
{
	return k_nVirtualPort_EphemeralFakePort0 <= nVirtualPort && nVirtualPort <= k_nVirtualPort_EphemeralFakePortMax;
}
inline bool IsVirtualPortGlobalFakePort( int nVirtualPort )
{
	return k_nVirtualPort_GlobalFakePort0 <= nVirtualPort && nVirtualPort <= k_nVirtualPort_GlobalFakePortMax;
}
inline bool IsVirtualPortFakePort( int nVirtualPort )
{
	return k_nVirtualPort_EphemeralFakePort0 <= nVirtualPort && nVirtualPort <= k_nVirtualPort_GlobalFakePortMax;
}

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
//
// NOTE: We do not detect overflow.
template <typename T>
inline byte *DeserializeVarInt( byte *p, const byte *end, T &x )
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

// Const version
template <typename T>
inline const byte *DeserializeVarInt( const byte *p, const byte *end, T &x )
{
	return DeserializeVarInt( const_cast<byte*>( p ), end, x );
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
		if ( iVal >= 10 )
		{
			if ( iVal >= 100 )
			{
				*(d++) = char( iVal/100 + '0' ); iVal %= 100;
			}
			*(d++) = char( iVal/10 + '0' ); iVal %= 10;
		}
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

/// Indent each line of a string
extern std::string Indent( const char *s );
inline std::string Indent( const std::string &s ) { return Indent( s.c_str() ); }

/// Generic hash
extern uint32 Murmorhash32( const void *data, size_t len );

/// Generate a fingerprint for a public that is reasonably collision resistant,
/// although not really cryptographically secure.  (We are in charge of the
/// set of public keys and we expect it to be reasonably small.)
extern uint64 CalculatePublicKeyID( const CECSigningPublicKey &pubKey );
extern uint64 CalculatePublicKeyID_Ed25519( const void *pPubKey, size_t cbPubKey );

/// Check an arbitrary signature using the specified public key.  (It's assumed that you have
/// already verified that this public key is from somebody you trust.)
extern bool BCheckSignature( const std::string &signed_data, CMsgSteamDatagramCertificate_EKeyType eKeyType, const std::string &public_key, const std::string &signature, SteamDatagramErrMsg &errMsg );

/// Parse PEM-like blob to a cert
extern bool ParseCertFromPEM( const void *pCert, size_t cbCert, CMsgSteamDatagramCertificateSigned &outMsgSignedCert, SteamNetworkingErrMsg &errMsg );
extern bool ParseCertFromBase64( const char *pBase64Data, size_t cbBase64Data, CMsgSteamDatagramCertificateSigned &outMsgSignedCert, SteamNetworkingErrMsg &errMsg );


inline bool IsPrivateIP( uint32 unIP )
{
	// RFC 1918
	if ( ( unIP & 0xff000000 ) == 0x0a000000 ) // 10.0.0.0/8
		return true;
	if ( ( unIP & 0xfff00000 ) == 0xac100000 ) // 172.16.0.0/12
		return true;
	if ( ( unIP & 0xffff0000 ) == 0xc0a80000 ) // 192.168.0.0/16
		return true;
	return false;
}

extern const char *GetAvailabilityString( ESteamNetworkingAvailability a );

inline void SteamNetworkingIPAddrToNetAdr( netadr_t &netadr, const SteamNetworkingIPAddr &addr )
{
	uint32 ipv4 = addr.GetIPv4();
	if ( ipv4 )
		netadr.SetIPv4( ipv4 );
	else
		netadr.SetIPV6( addr.m_ipv6 );
	netadr.SetPort( addr.m_port );
}

inline void NetAdrToSteamNetworkingIPAddr( SteamNetworkingIPAddr &addr, const netadr_t &netadr )
{
	netadr.GetIPV6( addr.m_ipv6 );
	addr.m_port = netadr.GetPort();
}

inline bool AddrEqual( const SteamNetworkingIPAddr &s, const netadr_t &n )
{
	if ( s.m_port != n.GetPort() )
		return false;
	switch ( n.GetType() )
	{
		case k_EIPTypeV4:
			return s.GetIPv4() == n.GetIPv4();

		case k_EIPTypeV6:
			return memcmp( s.m_ipv6, n.GetIPV6Bytes(), 16 ) == 0;
	}

	return false;
}

template <typename T>
inline int64 NearestWithSameLowerBits( T nLowerBits, int64 nReference )
{
	COMPILE_TIME_ASSERT( sizeof(T) < sizeof(int64) ); // Make sure it's smaller than 64 bits, or else why are you doing this?
	COMPILE_TIME_ASSERT( ~T(0) < 0 ); // make sure it's a signed type!
	T nDiff = nLowerBits - T( nReference );
	return nReference + nDiff;
}

/// Calculate hash of SteamNetworkingIdentity
struct SteamNetworkingIdentityHash
{
	uint32 operator()(struct SteamNetworkingIdentity const &x ) const
	{
		// Make sure we don't have any packing or alignment issues
		COMPILE_TIME_ASSERT( offsetof( SteamNetworkingIdentity, m_eType ) == 0 );
		COMPILE_TIME_ASSERT( sizeof( x.m_eType ) == 4 );
		COMPILE_TIME_ASSERT( offsetof( SteamNetworkingIdentity, m_cbSize ) == 4 );
		COMPILE_TIME_ASSERT( sizeof( x.m_cbSize ) == 4 );
		COMPILE_TIME_ASSERT( offsetof( SteamNetworkingIdentity, m_steamID64 ) == 8 );

		return Murmorhash32( &x, sizeof( x.m_eType ) + sizeof( x.m_cbSize ) + x.m_cbSize );
	}
};

/// Calculate hash of SteamNetworkingIPAddr
struct SteamNetworkingIPAddrHash
{
	uint32 operator()( struct SteamNetworkingIPAddr const &x ) const
	{
		// Make sure we don't have any packing or alignment issues
		COMPILE_TIME_ASSERT( sizeof( SteamNetworkingIPAddr ) == 16+2 );
		return Murmorhash32( &x, sizeof( SteamNetworkingIPAddr ) );
	};
};

inline bool IsValidSteamIDForIdentity( CSteamID steamID )
{
	return steamID.GetAccountID() != 0 && ( steamID.BIndividualAccount() || steamID.BGameServerAccount() );
}

inline bool IsValidSteamIDForIdentity( uint64 steamid64 ) { return IsValidSteamIDForIdentity( CSteamID( steamid64 ) ); }

extern bool BSteamNetworkingIdentityToProtobufInternal( const SteamNetworkingIdentity &identity, std::string *strIdentity, CMsgSteamNetworkingIdentityLegacyBinary *msgIdentityLegacyBinary, SteamDatagramErrMsg &errMsg );
extern bool BSteamNetworkingIdentityToProtobufInternal( const SteamNetworkingIdentity &identity, std::string *strIdentity, std::string *bytesMsgIdentityLegacyBinary, SteamDatagramErrMsg &errMsg );
#define BSteamNetworkingIdentityToProtobuf( identity, msg, field_identity_string, field_identity_legacy_binary, field_legacy_steam_id, errMsg ) ( \
		( (identity).GetSteamID64() ? (void)(msg).set_ ## field_legacy_steam_id( (identity).GetSteamID64() ) : (void)0 ), \
		BSteamNetworkingIdentityToProtobufInternal( identity, (msg).mutable_ ## field_identity_string(), (msg).mutable_ ## field_identity_legacy_binary(), errMsg ) \
	)
#define SteamNetworkingIdentityToProtobuf( identity, msg, field_identity_string, field_identity_legacy_binary, field_legacy_steam_id ) \
	{ SteamDatagramErrMsg identityToProtobufErrMsg; \
		if ( !BSteamNetworkingIdentityToProtobuf( identity, msg, field_identity_string, field_identity_legacy_binary, field_legacy_steam_id, identityToProtobufErrMsg ) ) { \
			AssertMsg2( false, "Failed to serialize identity to %s message.  %s", msg.GetTypeName().c_str(), identityToProtobufErrMsg ); \
		} \
	}

extern bool BSteamNetworkingIdentityFromLegacyBinaryProtobuf( SteamNetworkingIdentity &identity, const std::string &bytesMsgIdentity, SteamDatagramErrMsg &errMsg );
extern bool BSteamNetworkingIdentityFromLegacyBinaryProtobuf( SteamNetworkingIdentity &identity, const CMsgSteamNetworkingIdentityLegacyBinary &msgIdentity, SteamDatagramErrMsg &errMsg );
extern bool BSteamNetworkingIdentityFromLegacySteamID( SteamNetworkingIdentity &identity, uint64 legacy_steam_id, SteamDatagramErrMsg &errMsg );

template <typename TStatsMsg>
inline uint32 StatsMsgImpliedFlags( const TStatsMsg &msg );

// Returns:
// <0 Bad data
// 0  No data
// >0 OK
#define SteamNetworkingIdentityFromProtobuf( identity, msg, field_identity_string, field_identity_legacy_binary, field_legacy_steam_id, errMsg ) \
	( \
		(msg).has_ ##field_identity_string() ? ( SteamNetworkingIdentity_ParseString( &(identity), sizeof(identity), (msg).field_identity_string().c_str() ) ? +1 : ( V_strcpy_safe( errMsg, "Failed to parse string" ), -1 ) ) \
		: (msg).has_ ##field_identity_legacy_binary() ? ( BSteamNetworkingIdentityFromLegacyBinaryProtobuf( identity, (msg).field_identity_legacy_binary(), errMsg ) ? +1 : -1 ) \
		: (msg).has_ ##field_legacy_steam_id() ? ( BSteamNetworkingIdentityFromLegacySteamID( identity, (msg).field_legacy_steam_id(), errMsg ) ? +1 : -1 ) \
		: ( V_strcpy_safe( errMsg, "No identity data" ), 0 ) \
	)
inline int SteamNetworkingIdentityFromCert( SteamNetworkingIdentity &result, const CMsgSteamDatagramCertificate &msgCert, SteamDatagramErrMsg &errMsg )
{
	return SteamNetworkingIdentityFromProtobuf( result, msgCert, identity_string, legacy_identity_binary, legacy_steam_id, errMsg );
}

// NOTE: Does NOT check the cert signature!
extern int SteamNetworkingIdentityFromSignedCert( SteamNetworkingIdentity &result, const CMsgSteamDatagramCertificateSigned &msgCertSigned, SteamDatagramErrMsg &errMsg );

struct ConfigValueBase
{

	// Config value we should inherit from, if we are not set
	ConfigValueBase *m_pInherit = nullptr;

	enum EState
	{
		kENotSet,
		kESet,
		kELocked,
	};

	// Is the value set?
	EState m_eState = kENotSet;

	inline bool IsLocked() const { return m_eState == kELocked; }
	inline bool IsSet() const { return m_eState > kENotSet; }

	// Unlock, if we are locked
	inline void Unlock()
	{
		if ( m_eState == kELocked )
			m_eState = kESet;
	}
};

template<typename T>
struct ConfigValue : public ConfigValueBase
{
	inline ConfigValue() : m_data{} {}
	inline explicit ConfigValue( const T &defaultValue ) : m_data(defaultValue) { m_eState = kESet; }

	T m_data;

	/// Fetch the effective value
	inline const T &Get() const
	{
		const ConfigValueBase *p = this;
		while ( !p->IsSet() )
		{
			Assert( p->m_pInherit );
			p = p->m_pInherit;
		}

		const auto *t = static_cast<const ConfigValue<T> *>( p );
		return t->m_data;
	}

	inline void Set( const T &value )
	{
		Assert( !IsLocked() );
		m_data = value;
		m_eState = kESet;
	}

	// Lock in the current value
	inline void Lock()
	{
		if ( !IsSet() )
			m_data = Get();
		m_eState = kELocked;
	}
};

template <typename T> struct ConfigDataTypeTraits {};
template <> struct ConfigDataTypeTraits<int32> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_Int32; };
template <> struct ConfigDataTypeTraits<int64> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_Int64; };
template <> struct ConfigDataTypeTraits<float> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_Float; };
template <> struct ConfigDataTypeTraits<std::string> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_String; };
template <> struct ConfigDataTypeTraits<void*> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_Ptr; };

struct GlobalConfigValueEntry
{
	GlobalConfigValueEntry( ESteamNetworkingConfigValue eValue, const char *pszName, ESteamNetworkingConfigDataType eDataType, ESteamNetworkingConfigScope eScope, int cbOffsetOf );

	ESteamNetworkingConfigValue const m_eValue;
	const char *const m_pszName;
	ESteamNetworkingConfigDataType const m_eDataType;
	ESteamNetworkingConfigScope const m_eScope;
	int const m_cbOffsetOf;
	GlobalConfigValueEntry *m_pNextEntry;

	union
	{
		int32 m_int32min;
		float m_floatmin;
	};
	union
	{
		int32 m_int32max;
		float m_floatmax;
	};

	// Types that do not support limits
	template <typename T> void InitLimits( T _min, T _max ); // Intentionally not defined
	template<typename T> inline void NoLimits() {}
	template<typename T> inline void Clamp( T &val ) {}
};

// Types that do support clamping
template <> inline void GlobalConfigValueEntry::InitLimits<int32>( int32 _min, int32 _max ) { m_int32min = _min; m_int32max = _max; }
template<> void GlobalConfigValueEntry::NoLimits<int32>(); // Intentionally not defined
template<> inline void GlobalConfigValueEntry::Clamp<int32>( int32 &val ) { val = std::max( m_int32min, std::min( m_int32max, val ) ); }

template <> inline void GlobalConfigValueEntry::InitLimits<float>( float _min, float _max ) { m_floatmin = _min; m_floatmax = _max; }
template<> void GlobalConfigValueEntry::NoLimits<float>(); // Intentionally not defined
template<> inline void GlobalConfigValueEntry::Clamp<float>( float &val ) { val = std::max( m_floatmin, std::min( m_floatmax, val ) ); }

template<typename T>
struct GlobalConfigValueBase : GlobalConfigValueEntry
{
	GlobalConfigValueBase( ESteamNetworkingConfigValue eValue, const char *pszName, ESteamNetworkingConfigScope eScope, int cbOffsetOf, const T &defaultValue )
	: GlobalConfigValueEntry( eValue, pszName, ConfigDataTypeTraits<T>::k_eDataType, eScope, cbOffsetOf )
	, m_value{defaultValue}
	{
		GlobalConfigValueEntry::NoLimits<T>();
	}
	GlobalConfigValueBase( ESteamNetworkingConfigValue eValue, const char *pszName, ESteamNetworkingConfigScope eScope, int cbOffsetOf, const T &defaultValue, const T &minVal, const T &maxVal )
	: GlobalConfigValueEntry( eValue, pszName, ConfigDataTypeTraits<T>::k_eDataType, eScope, cbOffsetOf )
	, m_value{defaultValue}
	{
		GlobalConfigValueEntry::InitLimits( minVal, maxVal );
	}

	inline const T &Get() const
	{
		DbgAssert( !m_value.m_pInherit );
		DbgAssert( m_value.IsSet() );
		return m_value.m_data;
	}

	struct Value : public ConfigValue<T>
	{
		inline Value( const T &defaultValue ) : ConfigValue<T>(defaultValue), m_defaultValue(defaultValue) {}
		T m_defaultValue;
	};
	Value m_value;
};

template<typename T>
struct GlobalConfigValue : GlobalConfigValueBase<T>
{
	GlobalConfigValue( ESteamNetworkingConfigValue eValue, const char *pszName, const T &defaultValue )
	: GlobalConfigValueBase<T>( eValue, pszName, k_ESteamNetworkingConfig_Global, 0, defaultValue ) {}
	GlobalConfigValue( ESteamNetworkingConfigValue eValue, const char *pszName, const T &defaultValue, const T &minVal, const T &maxVal )
	: GlobalConfigValueBase<T>( eValue, pszName, k_ESteamNetworkingConfig_Global, 0, defaultValue, minVal, maxVal ) {}
};

struct ConnectionConfig
{
	ConfigValue<int32> TimeoutInitial;
	ConfigValue<int32> TimeoutConnected;
	ConfigValue<int32> SendBufferSize;
	ConfigValue<int32> RecvBufferSize;
	ConfigValue<int32> RecvBufferMessages;
	ConfigValue<int32> RecvMaxMessageSize;
	ConfigValue<int32> RecvMaxSegmentsPerPacket;
	ConfigValue<int32> SendRateMin;
	ConfigValue<int32> SendRateMax;
	ConfigValue<int32> MTU_PacketSize;
	ConfigValue<int32> NagleTime;
	ConfigValue<int32> IP_AllowWithoutAuth;
	ConfigValue<int32> IPLocalHost_AllowWithoutAuth;
	ConfigValue<int32> Unencrypted;
	ConfigValue<int32> SymmetricConnect;
	ConfigValue<int32> LocalVirtualPort;
	ConfigValue<int64> ConnectionUserData;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
	ConfigValue<int32> EnableDiagnosticsUI;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DUALWIFI
	ConfigValue<int32> DualWifi_Enable;
	#endif

	ConfigValue<int32> LogLevel_AckRTT;
	ConfigValue<int32> LogLevel_PacketDecode;
	ConfigValue<int32> LogLevel_Message;
	ConfigValue<int32> LogLevel_PacketGaps;
	ConfigValue<int32> LogLevel_P2PRendezvous;

	ConfigValue<void *> Callback_ConnectionStatusChanged;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		ConfigValue<std::string> P2P_STUN_ServerList;
		ConfigValue<int32> P2P_Transport_ICE_Enable;
		ConfigValue<int32> P2P_Transport_ICE_Penalty;
		ConfigValue<std::string> P2P_TURN_ServerList;
		ConfigValue<std::string> P2P_TURN_UserList;
		ConfigValue<std::string> P2P_TURN_PassList;
		ConfigValue<int32> P2P_Transport_ICE_Implementation;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		ConfigValue<std::string> SDRClient_DevTicket;
		ConfigValue<int32> P2P_Transport_SDR_Penalty;
	#endif

	void Init( ConnectionConfig *pInherit );
};

template<typename T>
struct ConnectionConfigDefaultValue : GlobalConfigValueBase<T>
{
	ConnectionConfigDefaultValue( ESteamNetworkingConfigValue eValue, const char *pszName, int cbOffsetOf, const T &defaultValue )
	: GlobalConfigValueBase<T>( eValue, pszName, k_ESteamNetworkingConfig_Connection, cbOffsetOf, defaultValue ) {}
	ConnectionConfigDefaultValue( ESteamNetworkingConfigValue eValue, const char *pszName, int cbOffsetOf, const T &defaultValue, const T &minVal, const T &maxVal )
	: GlobalConfigValueBase<T>( eValue, pszName, k_ESteamNetworkingConfig_Connection, cbOffsetOf, defaultValue, minVal, maxVal ) {}
};

namespace GlobalConfig
{
	extern GlobalConfigValue<float> FakePacketLoss_Send;
	extern GlobalConfigValue<float> FakePacketLoss_Recv;
	extern GlobalConfigValue<int32> FakePacketLag_Send;
	extern GlobalConfigValue<int32> FakePacketLag_Recv;
	extern GlobalConfigValue<float> FakePacketReorder_Send;
	extern GlobalConfigValue<float> FakePacketReorder_Recv;
	extern GlobalConfigValue<int32> FakePacketReorder_Time;
	extern GlobalConfigValue<float> FakePacketDup_Send;
	extern GlobalConfigValue<float> FakePacketDup_Recv;
	extern GlobalConfigValue<int32> FakePacketDup_TimeMax;
	extern GlobalConfigValue<int32> PacketTraceMaxBytes;
	extern GlobalConfigValue<int32> FakeRateLimit_Send_Rate;
	extern GlobalConfigValue<int32> FakeRateLimit_Send_Burst;
	extern GlobalConfigValue<int32> FakeRateLimit_Recv_Rate;
	extern GlobalConfigValue<int32> FakeRateLimit_Recv_Burst;
	extern GlobalConfigValue<int32> OutOfOrderCorrectionWindowMicroseconds;
	extern GlobalConfigValue<int32> ECN;

	extern GlobalConfigValue<int32> EnumerateDevVars;
	extern GlobalConfigValue<void*> Callback_CreateConnectionSignaling;

	extern ConnectionConfigDefaultValue<int32> LogLevel_PacketGaps;
	extern ConnectionConfigDefaultValue<int32> LogLevel_P2PRendezvous;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
	extern GlobalConfigValue<void*> Callback_MessagesSessionRequest;
	extern GlobalConfigValue<void*> Callback_MessagesSessionFailed;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
	extern GlobalConfigValue<int32> SDRClient_ConsecutitivePingTimeoutsFailInitial;
	extern GlobalConfigValue<int32> SDRClient_ConsecutitivePingTimeoutsFail;
	extern GlobalConfigValue<int32> SDRClient_MinPingsBeforePingAccurate;
	extern GlobalConfigValue<int32> SDRClient_LimitPingProbesToNearestN;
	extern GlobalConfigValue<int32> SDRClient_SingleSocket;
	extern GlobalConfigValue<int32> LogLevel_SDRRelayPings;
	extern GlobalConfigValue<std::string> SDRClient_ForceRelayCluster;
	extern GlobalConfigValue<std::string> SDRClient_ForceProxyAddr;
	extern GlobalConfigValue<std::string> SDRClient_FakeClusterPing;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
	extern ConnectionConfigDefaultValue<int32> EnableDiagnosticsUI;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
	extern ConnectionConfigDefaultValue< std::string > P2P_STUN_ServerList;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
	extern GlobalConfigValue<void*> Callback_FakeIPResult;
	#endif
}

// This awkwardness (adding and subtracting sizeof(intptr_t)) silences an UBSan
// runtime error about "member access within null pointer"
#define V_offsetof(class, field) (int)((intptr_t)&((class *)(0+sizeof(intptr_t)))->field - sizeof(intptr_t))

#define DEFINE_GLOBAL_CONFIGVAL( type, name, ... ) \
	namespace GlobalConfig { GlobalConfigValue<type> name( k_ESteamNetworkingConfig_##name, #name, __VA_ARGS__ ); }
#define DEFINE_CONNECTON_DEFAULT_CONFIGVAL( type, name, ... ) \
	namespace GlobalConfig { ConnectionConfigDefaultValue<type> name( k_ESteamNetworkingConfig_##name, #name, V_offsetof(ConnectionConfig, name), __VA_ARGS__ ); }

inline bool RandomBoolWithOdds( float odds )
{
	Assert( odds >= 0.0f && odds <= 100.0f );
	if ( odds <= 0.0f )
		return false;
	return WeakRandomFloat( 0, 100.0 ) < odds;
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
inline ESteamNetworkingFakeIPType GetIPv4FakeIPType( uint32 nIPv4 )
{
	if ( nIPv4 < k_nSteamNetworkingSockets_FakeIP_MinIP || nIPv4 > k_nSteamNetworkingSockets_FakeIP_MaxIP )
		return k_ESteamNetworkingFakeIPType_NotFake;

	COMPILE_TIME_ASSERT( k_nSteamNetworkingSockets_FakeIP_MaxGlobalIP + 1 == k_nSteamNetworkingSockets_FakeIP_MinLocalIP );
	if ( nIPv4 < k_nSteamNetworkingSockets_FakeIP_MinLocalIP )
		return k_ESteamNetworkingFakeIPType_GlobalIPv4;

	return k_ESteamNetworkingFakeIPType_LocalIPv4;
}
#else
inline ESteamNetworkingFakeIPType GetIPv4FakeIPType( uint32 nIPv4 ) { return k_ESteamNetworkingFakeIPType_NotFake; }
#endif

// Helper class to store a packet that was received when it looks
// like the packet might have been delivered out of order.
class CPossibleOutOfOrderPacket
{
public:

	// Detach from our owner and destroy this object.
	void Destroy();

	// Detach from our owner
	void Detach();

	LinkStatsTrackerBase *GetOwner() const { return m_pOwner; }
	void SetOwner( LinkStatsTrackerBase *pOwner );

protected:

	// Link stats tracker that owns us
	LinkStatsTrackerBase *m_pOwner = nullptr;

	virtual void DoDestroy();

	inline CPossibleOutOfOrderPacket() {}

	// Destructor is protected, you should call Destroy()
	virtual ~CPossibleOutOfOrderPacket();
};

} // namespace SteamNetworkingSocketsLib

#include <tier0/memdbgon.h>

// Set paranoia level, if not already set:
// 0 = disabled
// 1 = sometimes
// 2 = max
#ifndef STEAMNETWORKINGSOCKETS_SNP_PARANOIA
	#if defined( _CERT ) || defined( _RETAIL )
		#define STEAMNETWORKINGSOCKETS_SNP_PARANOIA 0
	#elif defined( _DEBUG )
		#define STEAMNETWORKINGSOCKETS_SNP_PARANOIA 2
	#else
		#define STEAMNETWORKINGSOCKETS_SNP_PARANOIA 1
	#endif
#endif

#if defined(__GNUC__ ) && defined( __linux__ ) && !defined( __ANDROID__ )
	#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA >= 2
		#define STEAMNETWORKINGSOCKETS_USE_GNU_DEBUG_MAP
		#include <debug/map>
	#endif
#endif

// Declare std_vector and std_map in our namespace.  They use debug versions when available,
// a custom allocator
namespace SteamNetworkingSocketsLib
{

	// Custom allocator that use malloc/free (and right now, those are #defines
	// that go to our own functions if we are overriding memory allocation.)
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE
		template <typename T>
		struct Allocator
		{
			using value_type = T;
			Allocator() noexcept = default;
			template<class U> Allocator(const Allocator<U>&) noexcept {}
			template<class U> bool operator==(const Allocator<U>&) const noexcept { return true; }
			template<class U> bool operator!=(const Allocator<U>&) const noexcept { return false; }
			static T* allocate( size_t n ) { return (T*)malloc( n *sizeof(T) ); }
			static void deallocate( T *p, size_t n ) { free( p ); }
		};
	#else
		template <typename T> using Allocator = std::allocator<T>;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_USE_GNU_DEBUG_MAP
		// Use debug version of std::map
		template< typename K, typename V, typename L = std::less<K> >
		using std_map = __gnu_debug::map<K,V,L, Allocator< std::pair<const K, V> > >;
	#else
		template< typename K, typename V, typename L = std::less<K> >
		using std_map = std::map<K,V,L,Allocator< std::pair<const K, V> >>;
	#endif

	template< typename T >
	using std_vector = std::vector<T, Allocator<T> >;
}

//
// Some misc tools for using std::vector that our CUtlVector class had
//

template< typename I = int >
struct IndexRange
{
	struct Iter
	{
		I i;
		int operator*() const { return i; }
		void operator++() { ++i; }
		inline bool operator==( const Iter &x) const { return i == x.i; }
		inline bool operator!=( const Iter &x) const { return i != x.i; }
	};

	I m_nBegin, m_nEnd;
	Iter begin() const { return Iter{m_nBegin}; }
	Iter end() const { return Iter{m_nEnd}; }
};

namespace vstd
{

template <typename V>
struct LikeStdVectorTraits {};

template <typename T, typename A>
struct LikeStdVectorTraits< std::vector<T,A> > { enum { yes=1 }; typedef T ElemType; };

}

template <typename V, typename I = int>
inline IndexRange<I> iter_indices( const V &vec )
{
	(void)vstd::LikeStdVectorTraits<V>::yes;
	return IndexRange<I>{ 0, (I)vec.size() };
}

template <typename V>
inline void erase_at( V &vec, int idx )
{
	(void)vstd::LikeStdVectorTraits<V>::yes;
	vec.erase( vec.begin()+idx );
}

template <typename V>
inline void pop_from_front( V &vec, int n )
{
	(void)vstd::LikeStdVectorTraits<V>::yes;
	auto b = vec.begin();
	vec.erase( b, b+n );
}

template <typename V>
inline int push_back_get_idx( V &vec )
{
	(void)vstd::LikeStdVectorTraits<V>::yes;
	vec.resize( vec.size()+1 ); return int( vec.size()-1 );
}

template <typename V>
inline int push_back_get_idx( V &vec, const typename vstd::LikeStdVectorTraits<V>::ElemType &x )
{
	vec.push_back( x ); return int( vec.size()-1 );
}

template <typename V>
inline typename vstd::LikeStdVectorTraits<V>::ElemType *push_back_get_ptr( V &vec )
{
	vec.resize( vec.size()+1 ); return &vec[ vec.size()-1 ];
}

template <typename V>
inline typename vstd::LikeStdVectorTraits<V>::ElemType *push_back_get_ptr( V &vec, const typename vstd::LikeStdVectorTraits<V>::ElemType &x )
{
	vec.push_back( x ); return &vec[ vec.size()-1 ];
}

// Return size as an *int*, not size_t, which is totally pedantic useless garbage in 99% of code.
template <typename V>
inline int len( const V &vec )
{
	(void)vstd::LikeStdVectorTraits<V>::yes;
	return (int)vec.size();
}

inline int len( const std::string &str )
{
	return (int)str.length();
}

template <typename K, typename V, typename L, typename A>
inline int len( const std::map<K,V,L,A> &map )
{
	return (int)map.size();
}

#ifdef STEAMNETWORKINGSOCKETS_USE_GNU_DEBUG_MAP
	template <typename K, typename V, typename L, typename A>
	inline int len( const __gnu_debug::map<K,V,L,A> &map )
	{
		return (int)map.size();
	}
#endif

template <typename T, typename L, typename A>
inline int len( const std::set<T,L,A> &map )
{
	return (int)map.size();
}

template< typename V>
inline bool has_element( const V &vec, const typename vstd::LikeStdVectorTraits<V>::ElemType &x )
{
	return std::find( vec.begin(), vec.end(), x ) != vec.end();
}

template< typename V>
inline bool find_and_remove_element( V &vec, const typename vstd::LikeStdVectorTraits<V>::ElemType &x )
{
	auto iter = std::find( vec.begin(), vec.end(), x );
	if ( iter == vec.end() )
		return false;
	vec.erase( iter );
	return true;
}

template< typename V>
inline int index_of( const V &vec, const typename vstd::LikeStdVectorTraits<V>::ElemType &x )
{
	int l = len( vec );
	for ( int i = 0 ; i < l ; ++i )
	{
		if ( vec[i] == x )
			return i;
	}
	return -1;
}

namespace vstd
{
	// Polyfill for is_trivially_copyable on older GCC.
	#if defined( __GNUC__ ) && __GNUC__ < 5 && !defined(__clang__)
		template <typename T> struct is_trivially_copyable
		{
			static constexpr bool value = __has_trivial_copy(T);
		};
	#else
		using std::is_trivially_copyable;
	#endif

	// FIXME should I use is_trivially_move_constructible here?
	template <typename T> struct is_relocatable : is_trivially_copyable<T> {};

	// If they tell us it's OK to relocate the type, then ignore GCC warnings
	#ifdef __GNUC__
		#pragma GCC diagnostic push
		#if __GNUC__ >= 8
			#pragma GCC diagnostic ignored "-Wclass-memaccess"
		#endif
	#endif

	template <typename T>
	void copy_construct_elements( T *dest, const T *src, size_t n )
	{
		if ( is_trivially_copyable<T>::value )
		{
			memcpy( dest, src, n*sizeof(T) );
		}
		else
		{
			T *dest_end = dest+n;
			while ( dest < dest_end )
				Construct<T>( dest++, *(src++) );
		}
	}

	template <typename T>
	void move_construct_elements( T *dest, T *src, size_t n )
	{
		if ( is_relocatable<T>::value )
		{
			memcpy( dest, src, n*sizeof(T) );
		}
		else
		{
			T *dest_end = dest+n;
			while ( dest < dest_end )
				Construct( dest++, std::move( *(src++) ) );
		}
	}

	// Almost the exact same interface as std::vector, only it has a small initial capacity of
	// size N in a statically-allocated block of memory.
	//
	// The only difference between this and std::vector (aside from any missing functions that just
	// need to be written) is the guarantee about not constructing elements on swapping.
	template< typename T, int N >
	class small_vector
	{
	public:
		small_vector() {}
		small_vector( const small_vector<T,N> &x );
		small_vector<T,N> &operator=( const small_vector<T,N> &x );
		small_vector( small_vector<T,N> &&x );
		small_vector<T,N> &operator=( small_vector<T,N> &&x );
		~small_vector() { clear(); }

		size_t size() const { return size_; }
		size_t capacity() const { return capacity_; }
		bool empty() const { return size_ == 0; }

		T *begin() { return dynamic_ ? dynamic_ : (T*)fixed_; };
		const T *begin() const { return dynamic_ ? dynamic_ : (T*)fixed_; };

		T *end() { return begin() + size_; }
		const T *end() const { return begin() + size_; }

		T &operator[]( size_t index ) { assert(index < size_); return begin()[index]; }
		const T &operator[]( size_t index ) const { assert(index < size_); return begin()[index]; }

		void push_back( const T &value );
		void pop_back();
		void erase( T *it );

		void resize( size_t n );
		void reserve( size_t n );
		void clear();
		void assign( const T *srcBegin, const T *srcEnd );

	private:
		size_t size_ = 0, capacity_ = N;
		T *dynamic_ = nullptr;
		char fixed_[N][sizeof(T)];
	};

	template<typename T, int N>
	small_vector<T,N>::small_vector( const small_vector<T,N> &x )
	{
		reserve( x.size_ );
		size_ = x.size_;
		vstd::copy_construct_elements<T>( begin(), x.begin(), x.size_ );
	}

	template<typename T, int N>
	small_vector<T,N>::small_vector( small_vector<T,N> &&x )
	{
		size_ = x.size_;
		if ( x.dynamic_ )
		{
			capacity_ = x.capacity_;
			dynamic_ = x.dynamic_;
			x.dynamic_ = nullptr;
			x.size_ = 0;
			x.capacity_ = N;
		}
		else
		{
			vstd::move_construct_elements<T>( (T*)fixed_, (T*)x.fixed_, size_ );
		}
	}

	template<typename T, int N>
	small_vector<T,N> &small_vector<T,N>::operator=( const small_vector<T,N> &x )
	{
		if ( this != &x )
			assign( x.begin(), x.end() );
		return *this;
	}

	template<typename T, int N>
	small_vector<T,N> &small_vector<T,N>::operator=( small_vector<T,N> &&x )
	{
		clear();
		size_ = x.size_;
		if ( x.dynamic_ )
		{
			capacity_ = x.capacity_;
			dynamic_ = x.dynamic_;
			x.dynamic_ = nullptr;
			x.size_ = 0;
			x.capacity_ = N;
		}
		else
		{
			vstd::move_construct_elements<T>( (T*)fixed_, (T*)x.fixed_, size_ );
		}
		return *this;
	}

	template< typename T, int N >
	void small_vector<T,N>::push_back( const T &value )
	{
		if ( size_ >= capacity_ )
			reserve( size_*2  +  (63+sizeof(T))/sizeof(T) );
		Construct<T>( begin() + size_, value );
		++size_;
	}

	template< typename T, int N >
	void small_vector<T,N>::pop_back()
	{
		assert( size_ > 0 );
		--size_;
		( begin() + size_ )->~T();
	}

	template< typename T, int N >
	void small_vector<T,N>::erase( T *it )
	{
		T *e = end();
		assert( begin() <= it );
		assert( it < e );

		if ( is_relocatable<T>::value )
		{
			memmove( it, it+1, (char*)e - (char*)(it+1) );
		}
		else
		{
			--e;
			while ( it < e )
			{
				it[0] = std::move( it[1] );
				++it;
			}
			e->~T();
		}
		--size_;
	}

	template< typename T, int N >
	void small_vector<T,N>::reserve( size_t n )
	{
		if ( n <= capacity_ )
			return;
		assert( capacity_ >= size_ );
		if ( is_relocatable<T>::value && dynamic_ )
		{
			dynamic_ = (T*)realloc( dynamic_, n * sizeof(T) );
		}
		else
		{
			T *new_dynamic = (T *)malloc( n * sizeof(T) );
			T *s = begin();
			T *e = s + size_;
			T *d = new_dynamic;
			while ( s < e )
			{
				Construct<T>( d, std::move( *s ) );
				s->~T();
				++s;
				++d;
			}
			if ( dynamic_ )
				::free( dynamic_ );
			dynamic_ = new_dynamic;
		}
		capacity_ = n;
	}

	template< typename T, int N >
	void small_vector<T,N>::resize( size_t n )
	{
		if ( n > size_ )
		{
			reserve( n );
			T *b = begin() + size_;
			while ( size_ < n )
			{
				Construct<T>( b ); // NOTE: Does not use value initializer, so PODs are *not* initialized
				++b;
				++size_;
			}
		}
		else
		{
			T *e = end();
			while ( size_ > n )
			{
				--size_;
				--e;
				e->~T();
			}
		}
	}

	template< typename T, int N >
	void small_vector<T,N>::clear()
	{
		T *b = begin();
		T *e = b + size_;
		while ( e > b )
		{
			--e;
			e->~T();
		}
		if ( dynamic_ )
		{
			::free( dynamic_ );
			dynamic_ = nullptr;
		}
		size_ = 0;
		capacity_ = N;
	}

	template< typename T, int N >
	void small_vector<T,N>::assign( const T *srcBegin, const T *srcEnd )
	{
		if ( srcEnd <= srcBegin )
		{
			clear();
			return;
		}
		size_t n = srcEnd - srcBegin;
		if ( n > N )
		{
			// We need dynamic memory.  If we're not exactly sized already,
			// just nuke everyhing we have.
			if ( n != capacity_ ) 
			{
				clear();
				reserve( n );
			}
			assert( dynamic_ );
			if ( !std::is_trivial<T>::value )
			{
				while ( size_ > n )
					dynamic_[--size_].~T();
			}
		}
		else if ( dynamic_ )
		{
			// We have dynamic allocation, but don't need it
			clear();
		}
		assert( capacity_ >= n );
		if ( is_trivially_copyable<T>::value )
		{
			// Just blast them over, and don't bother with the leftovers
			memcpy( begin(), srcBegin, n*sizeof(T) );
		}
		else
		{
			assert( size_ <= n );

			// Complex type.  Try to avoid excess constructor/destructor calls
			// First use operator= for items already constructed
			const T *s = srcBegin;
			T *d = begin();
			T *e = d + size_;
			while ( d < e && s < srcEnd )
				*(d++) = *(s++);

			// Use copy constructor for any remaining items
			while ( s < srcEnd )
				Construct<T>( d++, *(s++) );
		}
		size_ = n;
	}

	template <typename T,int N>
	struct LikeStdVectorTraits< small_vector<T,N> > { enum { yes = 1 }; typedef T ElemType; };

	// small_vectors are relocatable,  if T is relocatable
	template <typename T,int N> struct is_relocatable< small_vector<T,N> > : is_relocatable<T> {};

	#ifdef __GNUC__
		#pragma GCC diagnostic pop
	#endif

} // namespace vstd

#endif // STEAMNETWORKINGSOCKETS_INTERNAL_H
