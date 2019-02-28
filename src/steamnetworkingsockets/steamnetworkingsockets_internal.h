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
	#undef SetPort
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

// Windows is the worst
#undef min
#undef max

// Public shared stuff
#include <tier0/basetypes.h>
#include <tier0/t0constants.h>
#include <tier0/platform.h>
#include <tier0/dbgflag.h>
#ifdef STEAMNETWORKINGSOCKETS_STEAM
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

// An identity operator that always returns its operand.
// NOTE: std::hash is an identity operator on many compilers
//       for the basic primitives.  If you really need actual
//       hashing, don't use std::hash!
template <typename T >
struct Identity
{
	 const T &operator()( const T &x ) const { return x; }
};

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

/// Max message size that we can send without fragmenting (except perhaps in some
/// rare degenerate cases.)  Should we promote this to a public header?  It does
/// seems like an important API parameter?  Or maybe not.  If they are doing any
/// of their own fragmentation and assembly, 
const int k_cbSteamNetworkingSocketsMaxMessageNoFragment = 1200;

/// Max size of a reliable segment.  This is designed such that a reliable
/// message of size k_cbSteamNetworkingSocketsMaxMessageNoFragment
/// won't get fragmented, except perhaps in an exceedingly degenerate
/// case.  (Even in this case, the protocol will function properly, it
/// will just potentially fragment the message.)  We shouldn't make any
/// hard promises in this department.
///
/// 1 byte - message header
/// 3 bytes - varint encode msgnum gap between previous reliable message.  (Gap could be greater, but this would be really unusual.)
/// 1 byte - size remainder bytes (assuming message is k_cbSteamNetworkingSocketsMaxMessageNoFragment, we only need a single size overflow byte)
const int k_cbSteamNetworkingSocketsMaxReliableMessageSegment = k_cbSteamNetworkingSocketsMaxMessageNoFragment + 5;

/// Worst case encoding of a single reliable segment frame.
/// Basically this is the SNP frame type header byte, plus a 48-bit
/// message number (worst case scenario).  Nothing for the size field,
/// since we assume that if we write this many bytes, it will be the last
/// frame in the packet and thus no explicit size field will be needed.
const int k_cbSteamNetworkingSocketsMaxReliableMessageSegmentFrame = k_cbSteamNetworkingSocketsMaxReliableMessageSegment + 7;

/// Currently we always use AES Rijndael for symmetric encryption,
/// which has a block size of 128 bits.  This is not configurable.
const int k_cbSteamNetworkingSocketsEncryptionBlockSize = 16;

/// Size of security tag for AES-GCM.
/// It would be nice to use a smaller tag, but BCrypt requires a 16-byte tag,
/// which is what OpenSSL uses by default for TLS.
const int k_cbSteamNetwokingSocketsEncrytionTagSize = 16;

/// Max length of plaintext and encrypted payload we will send.  AES-GCM does
/// not use padding (but it does have the security tag).  So this can be
/// arbitrary, it does not need to account for the block size.
const int k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend = 1248;
const int k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend-k_cbSteamNetwokingSocketsEncrytionTagSize;

/// Use larger limits for what we are willing to receive.
const int k_cbSteamNetworkingSocketsMaxEncryptedPayloadRecv = k_cbSteamNetworkingSocketsMaxUDPMsgLen;
const int k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv = k_cbSteamNetworkingSocketsMaxUDPMsgLen;

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

/// Protocol version of this code.  This is a blunt instrument, which is incremented when we
/// wish to change the wire protocol in a way that doesn't have some other easy
/// mechanism for dealing with compatibility (e.g. using protobuf's robust mechanisms).
const uint32 k_nCurrentProtocolVersion = 8;

/// Minimum required version we will accept from a peer.  We increment this
/// when we introduce wire breaking protocol changes and do not wish to be
/// backward compatible.  This has been fine before the	first major release,
/// but once we make a big public release, we probably won't ever be able to
/// do this again, and we'll need to have more sophisticated mechanisms. 
const uint32 k_nMinRequiredProtocolVersion = 8;

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
extern uint64 CalculatePublicKeyID( const CECSigningPublicKey &pubKey );

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

inline void SteamNetworkingIPAddrToNetAdr( netadr_t &netadr, const SteamNetworkingIPAddr &addr )
{
	uint32 ipv4 = addr.GetIPv4();
	if ( ipv4 )
		netadr.SetIP( ipv4 );
	else
		netadr.SetIPV6( addr.m_ipv6 );
	netadr.SetPort( addr.m_port );
}

inline void NetAdrToSteamNetworkingIPAddr( SteamNetworkingIPAddr &addr, const netadr_t &netadr )
{
	netadr.GetIPV6( addr.m_ipv6 );
	addr.m_port = netadr.GetPort();
}

template <typename T>
inline int64 NearestWithSameLowerBits( T nLowerBits, int64 nReference )
{
	COMPILE_TIME_ASSERT( sizeof(T) < sizeof(int64) ); // Make sure it's smaller than 64 bits, or else why are you doing this?
	COMPILE_TIME_ASSERT( ~T(0) < 0 ); // make sure it's a signed type!
	T nDiff = nLowerBits - T( nReference );
	return nReference + nDiff;
}

/// Calculate hash of identity.
struct SteamNetworkingIdentityHash
{
	uint32 operator()( const SteamNetworkingIdentity &x ) const;
};

struct SteamNetworkingIdentityRender
{
	SteamNetworkingIdentityRender( const SteamNetworkingIdentity &x ) { x.ToString( buf, sizeof(buf) ); }
	inline const char *c_str() const { return buf; }
private:
	char buf[ SteamNetworkingIdentity::k_cchMaxString ];
};

inline bool IsValidSteamIDForIdentity( CSteamID steamID )
{
	return steamID.GetAccountID() != 0 && ( steamID.BIndividualAccount() || steamID.BGameServerAccount() );
}

inline bool IsValidSteamIDForIdentity( uint64 steamid64 ) { return IsValidSteamIDForIdentity( CSteamID( steamid64 ) ); }

extern bool BSteamNetworkingIdentityToProtobufInternal( const SteamNetworkingIdentity &identity, CMsgSteamNetworkingIdentity *msgIdentity, SteamDatagramErrMsg &errMsg );
extern bool BSteamNetworkingIdentityToProtobufInternal( const SteamNetworkingIdentity &identity, std::string *bytesMsgIdentity, SteamDatagramErrMsg &errMsg );
#define BSteamNetworkingIdentityToProtobuf( identity, msg, field_identity, field_legacy_steam_id, errMsg ) ( \
		( (identity).GetSteamID64() ? (void)(msg).set_ ## field_legacy_steam_id( (identity).GetSteamID64() ) : (void)0 ), \
		BSteamNetworkingIdentityToProtobufInternal( identity, (msg).mutable_ ## field_identity(), errMsg ) \
	)
#define SteamNetworkingIdentityToProtobuf( identity, msg, field_identity, field_legacy_steam_id ) \
	{ SteamDatagramErrMsg identityToProtobufErrMsg; \
		if ( !BSteamNetworkingIdentityToProtobuf( identity, msg, field_identity, field_legacy_steam_id, identityToProtobufErrMsg ) ) { \
			AssertMsg2( false, "Failed to serialize identity to %s message.  %s", msg.GetTypeName().c_str(), identityToProtobufErrMsg ); \
		} \
	}

extern bool BSteamNetworkingIdentityFromProtobufBytes( SteamNetworkingIdentity &identity, const std::string &bytesMsgIdentity, uint64 legacy_steam_id, SteamDatagramErrMsg &errMsg );
extern bool BSteamNetworkingIdentityFromProtobufMsg( SteamNetworkingIdentity &identity, const CMsgSteamNetworkingIdentity &msgIdentity, SteamDatagramErrMsg &errMsg );
extern bool BSteamNetworkingIdentityFromLegacySteamID( SteamNetworkingIdentity &identity, uint64 legacy_steam_id, SteamDatagramErrMsg &errMsg );

template <typename TStatsMsg>
inline uint32 StatsMsgImpliedFlags( const TStatsMsg &msg );

template <typename TStatsMsg>
inline void SetStatsMsgFlagsIfNotImplied( TStatsMsg &msg, uint32 nFlags )
{
	if ( ( nFlags & StatsMsgImpliedFlags( msg ) ) != nFlags )
		msg.set_flags( nFlags );
	else
		msg.clear_flags(); // All flags we needed to send are implied by message, no need to send explicitly
}

// Returns:
// <0 Bad data
// 0  No data
// >0 OK
#define SteamNetworkingIdentityFromProtobuf( identity, msg, field_identity, field_legacy_steam_id, errMsg ) \
	( \
		(msg).has_ ##field_identity() ? ( BSteamNetworkingIdentityFromProtobufMsg( identity, (msg).field_identity(), errMsg ) ? +1 : -1 ) \
		: (msg).has_ ##field_legacy_steam_id() ? ( BSteamNetworkingIdentityFromLegacySteamID( identity, (msg).field_legacy_steam_id(), errMsg ) ? +1 : -1 ) \
		: ( V_strcpy_safe( errMsg, "No identity data" ), 0 ) \
	)
inline int SteamNetworkingIdentityFromCert( SteamNetworkingIdentity &result, const CMsgSteamDatagramCertificate &msgCert, SteamDatagramErrMsg &errMsg )
{
	return SteamNetworkingIdentityFromProtobuf( result, msgCert, identity, legacy_steam_id, errMsg );
}

// NOTE: Does NOT check the cert signature!
extern int SteamNetworkingIdentityFromSignedCert( SteamNetworkingIdentity &result, const CMsgSteamDatagramCertificateSigned &msgCertSigned, SteamDatagramErrMsg &errMsg );

struct ConfigValueBase
{

	// Config value we should inherit from, if we are not set
	ConfigValueBase *m_pInherit = nullptr;

	// Is the value set?
	bool m_bValueSet = false;
};

template<typename T>
struct ConfigValue : public ConfigValueBase
{
	inline ConfigValue() : m_data{} {}
	inline explicit ConfigValue( const T &defaultValue ) : m_data(defaultValue) { m_bValueSet = true; }

	T m_data;

	/// Fetch the effective value
	inline const T &Get() const
	{
		const ConfigValueBase *p = this;
		while ( !p->m_bValueSet )
		{
			Assert( p->m_pInherit );
			p = p->m_pInherit;
		}

		const auto *t = static_cast<const ConfigValue<T> *>( p );
		return t->m_data;
	}

	void Set( const T &value )
	{
		m_data = value;
		m_bValueSet = true;
	}
};

template <typename T> struct ConfigDataTypeTraits {};
template <> struct ConfigDataTypeTraits<int32> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_Int32; };
template <> struct ConfigDataTypeTraits<int64> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_Int64; };
template <> struct ConfigDataTypeTraits<float> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_Float; };
template <> struct ConfigDataTypeTraits<std::string> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_String; };
template <> struct ConfigDataTypeTraits<void*> { const static ESteamNetworkingConfigDataType k_eDataType = k_ESteamNetworkingConfig_FunctionPtr; };

struct GlobalConfigValueEntry
{
	GlobalConfigValueEntry( ESteamNetworkingConfigValue eValue, const char *pszName, ESteamNetworkingConfigDataType eDataType, ESteamNetworkingConfigScope eScope, int cbOffsetOf );

	ESteamNetworkingConfigValue const m_eValue;
	const char *const m_pszName;
	ESteamNetworkingConfigDataType const m_eDataType;
	ESteamNetworkingConfigScope const m_eScope;
	int const m_cbOffsetOf;
	GlobalConfigValueEntry *m_pNextEntry;
};

template<typename T>
struct GlobalConfigValueBase : GlobalConfigValueEntry
{
	GlobalConfigValueBase( ESteamNetworkingConfigValue eValue, const char *pszName, const T &defaultValue, ESteamNetworkingConfigScope eScope, int cbOffsetOf )
	: GlobalConfigValueEntry( eValue, pszName, ConfigDataTypeTraits<T>::k_eDataType, eScope, cbOffsetOf )
	, m_value{defaultValue} {}

	inline const T &Get() const
	{
		Assert( !m_value.m_pInherit );
		Assert( m_value.m_bValueSet );
		return m_value.m_data;
	}

	struct Value : public ConfigValue<T>
	{
		inline Value( const T &defaultValue ) : ConfigValue<T>(defaultValue), m_defaultValue(defaultValue) {}
		const T m_defaultValue;
	};
	Value m_value;
};

template<typename T>
struct GlobalConfigValue : GlobalConfigValueBase<T>
{
	GlobalConfigValue( ESteamNetworkingConfigValue eValue, const char *pszName, const T &defaultValue )
	: GlobalConfigValueBase<T>( eValue, pszName, defaultValue, k_ESteamNetworkingConfig_Global, 0 ) {}
};

struct ConnectionConfig
{
	ConfigValue<int32> m_TimeoutInitial;
	ConfigValue<int32> m_TimeoutConnected;
	ConfigValue<int32> m_SendBufferSize;
	ConfigValue<int32> m_SendRateMin;
	ConfigValue<int32> m_SendRateMax;
	ConfigValue<int32> m_NagleTime;
	ConfigValue<int32> m_IP_AllowWithoutAuth;

	ConfigValue<int32> m_LogLevel_AckRTT;
	ConfigValue<int32> m_LogLevel_PacketDecode;
	ConfigValue<int32> m_LogLevel_Message;
	ConfigValue<int32> m_LogLevel_PacketGaps;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		ConfigValue<int32> m_LogLevel_P2PRendezvous;
		ConfigValue<std::string> m_SDRClient_DebugTicketAddress;
	#endif

	void Init( ConnectionConfig *pInherit );
};

template<typename T>
struct ConnectionConfigDefaultValue : GlobalConfigValueBase<T>
{
	ConnectionConfigDefaultValue( ESteamNetworkingConfigValue eValue, const char *pszName, const T &defaultValue, int cbOffsetOf )
	: GlobalConfigValueBase<T>( eValue, pszName, defaultValue, k_ESteamNetworkingConfig_Connection, cbOffsetOf ) {}
};

extern GlobalConfigValue<float> g_Config_FakePacketLoss_Send;
extern GlobalConfigValue<float> g_Config_FakePacketLoss_Recv;
extern GlobalConfigValue<int32> g_Config_FakePacketLag_Send;
extern GlobalConfigValue<int32> g_Config_FakePacketLag_Recv;
extern GlobalConfigValue<float> g_Config_FakePacketReorder_Send;
extern GlobalConfigValue<float> g_Config_FakePacketReorder_Recv;
extern GlobalConfigValue<int32> g_Config_FakePacketReorder_Time;
extern GlobalConfigValue<float> g_Config_FakePacketDup_Send;
extern GlobalConfigValue<float> g_Config_FakePacketDup_Recv;
extern GlobalConfigValue<int32> g_Config_FakePacketDup_TimeMax;

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
extern GlobalConfigValue<int32> g_Config_SDRClient_ConsecutitivePingTimeoutsFailInitial;
extern GlobalConfigValue<int32> g_Config_SDRClient_ConsecutitivePingTimeoutsFail;
extern GlobalConfigValue<int32> g_Config_SDRClient_MinPingsBeforePingAccurate;
extern GlobalConfigValue<int32> g_Config_SDRClient_SingleSocket;
extern GlobalConfigValue<int32> g_Config_LogLevel_SDRRelayPings;
extern GlobalConfigValue<std::string> g_Config_SDRClient_ForceRelayCluster;
extern GlobalConfigValue<std::string> g_Config_SDRClient_ForceProxyAddr;
#endif

// This awkwardness (adding and subtracting sizeof(intptr_t)) silences an UBSan
// runtime error about "member access within null pointer"
#define V_offsetof(class, field) (int)((intptr_t)&((class *)(0+sizeof(intptr_t)))->field - sizeof(intptr_t))

#define DEFINE_GLOBAL_CONFIGVAL( type, name, defaultVal ) \
	GlobalConfigValue<type> g_Config_##name( k_ESteamNetworkingConfig_##name, #name, defaultVal )
#define DEFINE_CONNECTON_DEFAULT_CONFIGVAL( type, name, defaultVal ) \
	ConnectionConfigDefaultValue<type> g_ConfigDefault_##name( k_ESteamNetworkingConfig_##name, #name, defaultVal, V_offsetof(ConnectionConfig, m_##name) )

inline bool RandomBoolWithOdds( float odds )
{
	Assert( odds >= 0.0f && odds <= 100.0f );
	if ( odds <= 0.0f )
		return false;
	return WeakRandomFloat( 0, 100.0 ) < odds;
}

} // namespace SteamNetworkingSocketsLib

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

	template <typename T>
	void copy_construct_elements( T *dest, const T *src, size_t n )
	{
		if ( std::is_trivial<T>::value )
		{
			memcpy( dest, src, n*sizeof(T) );
		}
		else
		{
			T *dest_end = dest+n;
			while ( dest < dest_end )
				new (dest++) T( *(src++) );
		}
	}

	template <typename T>
	void move_construct_elements( T *dest, T *src, size_t n )
	{
		if ( std::is_trivial<T>::value )
		{
			memcpy( dest, src, n*sizeof(T) );
		}
		else
		{
			T *dest_end = dest+n;
			while ( dest < dest_end )
				new (dest++) T( std::move( *(src++) ) );
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
		clear();
		reserve( x.size_ );
		size_ = x.size_;
		vstd::copy_construct_elements( begin(), x.begin(), size_ );
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
	}

	template< typename T, int N >
	void small_vector<T,N>::push_back( const T &value )
	{
		if ( size_ >= capacity_ )
			reserve( size_*2  +  (63+sizeof(T))/sizeof(T) );
		new ( begin() + size_ ) T ( value );
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

		if ( std::is_trivial<T>::value )
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
		if ( std::is_trivial<T>::value && dynamic_ )
		{
			dynamic_ = (T*)realloc( dynamic_, n * sizeof(T) );
		}
		else
		{
			T *new_dynamic = (T *)malloc( n * sizeof(T) );
			T *e = end();
			for ( T *s = begin(), *d = new_dynamic ; s < e ; ++s, ++d )
			{
				new ( d ) T ( std::move( *s ) );
				s->~T();
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
			T *b = begin();
			while ( size_ < n )
			{
				new ( b ) T;
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

	template <typename T,int N>
	struct LikeStdVectorTraits< small_vector<T,N> > { enum { yes = 1 }; typedef T ElemType; };

} // namespace vstd


#include <tier0/memdbgon.h>

#endif // STEAMNETWORKINGSOCKETS_INTERNAL_H
