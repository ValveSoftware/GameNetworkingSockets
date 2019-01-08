//====== Copyright Valve Corporation, All rights reserved. ====================

#include <crypto.h>
#include "steamnetworkingsockets_internal.h"

// Must be the last include
#include <tier0/memdbgon.h>

namespace SteamNetworkingSocketsLib {

extern uint64 CalculatePublicKeyID( const CECSigningPublicKey &pubKey )
{
	if ( !pubKey.IsValid() )
		return 0;

	// SHA over the whole public key.
	SHA256Digest_t digest;
	CCrypto::GenerateSHA256Digest( pubKey.GetData(), pubKey.GetLength(), &digest );

	// First 8 bytes
	return LittleQWord( *(uint64*)&digest );
}

// Returns:
// -1  Bogus data
// 0   Unknown type
// 1   OK
static int SteamNetworkingIdentityFromProtobufMsg( SteamNetworkingIdentity &identity, const CMsgSteamNetworkingIdentity &msgIdentity, SteamDatagramErrMsg &errMsg )
{
	if ( msgIdentity.has_steam_id() )
	{
		if ( !IsValidSteamIDForIdentity( msgIdentity.steam_id() ) )
		{
			V_sprintf_safe( errMsg, "Invalid SteamID %llu", (unsigned long long)msgIdentity.steam_id() );
			return -1;
		}
		identity.SetSteamID64( msgIdentity.steam_id() );
		return 1;
	}
	if ( msgIdentity.has_generic_string() )
	{
		if ( !identity.SetGenericString( msgIdentity.generic_string().c_str() ) )
		{
			V_sprintf_safe( errMsg, "Invalid generic string '%s'", msgIdentity.generic_string().c_str() );
			return -1;
		}
		return 1;
	}
	if ( msgIdentity.has_generic_bytes() )
	{
		if ( !identity.SetGenericBytes( msgIdentity.generic_bytes().c_str(), msgIdentity.generic_bytes().length() ) )
		{
			V_sprintf_safe( errMsg, "Invalid generic bytes (len=%d)", len( msgIdentity.generic_bytes() ) );
			return -1;
		}
		return 1;
	}
	if ( msgIdentity.has_ipv6_and_port() )
	{
		const std::string &ip_and_port = msgIdentity.ipv6_and_port();
		COMPILE_TIME_ASSERT( sizeof( identity.m_ip ) == 18 ); // 16-byte IPv6 + 2-byte port
		if ( ip_and_port.length() != 18 )
		{
			V_sprintf_safe( errMsg, "ip_and_port field has invalid length %d", len( ip_and_port ) );
			return -1;
		}
		const uint8 *b = (const uint8 *)msgIdentity.ipv6_and_port().c_str();
		SteamNetworkingIPAddr tmpAddr;
		tmpAddr.SetIPv6( b, BigWord( *(uint16*)(b+16) ) );
		identity.SetIPAddr( tmpAddr );
		return 1;
	}

	// Unknown type
	return 0;
}

bool BSteamNetworkingIdentityFromProtobufMsg( SteamNetworkingIdentity &identity, const CMsgSteamNetworkingIdentity &msgIdentity, SteamDatagramErrMsg &errMsg )
{
	// Parse it
	int r = SteamNetworkingIdentityFromProtobufMsg( identity, msgIdentity, errMsg );
	if ( r > 0 )
		return true;
	if ( r < 0 )
	{
		identity.Clear();
		return false;
	}

	if ( msgIdentity.unknown_fields().field_count() > 0 )
	{
		V_sprintf_safe( errMsg, "Unrecognized identity format.  (%d unknown field(s), first ID=%d)", msgIdentity.unknown_fields().field_count(), msgIdentity.unknown_fields().field(0).number() );
	}
	else if ( msgIdentity.ByteSize() == 0 )
	{
		V_strcpy_safe( errMsg, "Empty identity msg" );
	}
	else
	{
		AssertMsg( false, "SteamNetworkingIdentityFromProtobufMsg returned 0, but but we don't have any unknown fields?" );
		V_strcpy_safe( errMsg, "Unrecognized identity format" );
	}

	identity.Clear();
	return false;
}

bool BSteamNetworkingIdentityFromLegacySteamID( SteamNetworkingIdentity &identity, uint64 legacy_steam_id, SteamDatagramErrMsg &errMsg )
{
	if ( !IsValidSteamIDForIdentity( legacy_steam_id ) )
	{
		V_sprintf_safe( errMsg, "Invalid SteamID %llu (in legacy field)", legacy_steam_id );
		return false;
	}
	identity.SetSteamID64( legacy_steam_id );
	return true;
}


bool BSteamNetworkingIdentityFromProtobufBytes( SteamNetworkingIdentity &identity, const std::string &bytesMsgIdentity, uint64 legacy_steam_id, SteamDatagramErrMsg &errMsg )
{
	// Assume failure
	identity.Clear();

	// New format blob not present?
	if ( bytesMsgIdentity.empty() )
	{

		// Should have a legacy SteamID
		if ( !legacy_steam_id )
		{
			V_strcpy_safe( errMsg, "No identity data is present" );
			return false;
		}
		if ( !IsValidSteamIDForIdentity( legacy_steam_id ) )
		{
			V_sprintf_safe( errMsg, "Invalid SteamID %llu (in legacy field)", legacy_steam_id );
			return false;
		}
		identity.SetSteamID64( legacy_steam_id );
		return true;
	}

	// Parse it
	CMsgSteamNetworkingIdentity msgIdentity;
	if ( !msgIdentity.ParseFromString( bytesMsgIdentity ) )
	{
		V_strcpy_safe( errMsg, "Protobuf failed to parse" );
		return false;
	}

	// Parse it
	int r = SteamNetworkingIdentityFromProtobufMsg( identity, msgIdentity, errMsg );
	if ( r > 0 )
		return true;
	if ( r < 0 )
	{
		identity.Clear();
		return false;
	}

	// Hm, unknown identity type.  Include the first few bytes for debugging
	const size_t kMaxBytes = 8;
	char szBytes[kMaxBytes*2 + 4];
	size_t l = std::min( bytesMsgIdentity.length(), kMaxBytes );
	for ( size_t i = 0 ; i < l ; ++i )
		sprintf( szBytes + i*2, "%02x", uint8(bytesMsgIdentity[i]) );
	szBytes[l*2] = '\0';
	V_sprintf_safe( errMsg, "Parse failure.  Length=%d, data begins %s", (int)bytesMsgIdentity.length(), szBytes );
	return false;
}

int SteamNetworkingIdentityFromSignedCert( SteamNetworkingIdentity &result, const CMsgSteamDatagramCertificateSigned &msgCertSigned, SteamDatagramErrMsg &errMsg )
{
	// !SPEED! We could optimize this by hand-parsing the protobuf.
	// This would avoid some memory allocations and dealing with
	// fields we don't care about.
	CMsgSteamDatagramCertificate cert;
	if ( !cert.ParseFromString( msgCertSigned.cert() ) )
	{
		V_strcpy_safe( errMsg, "Cert failed protobuf parse" );
		return -1;
	}
	return SteamNetworkingIdentityFromCert( result, cert, errMsg );
}

extern bool BSteamNetworkingIdentityToProtobufInternal( const SteamNetworkingIdentity &identity, CMsgSteamNetworkingIdentity *msgIdentity, SteamDatagramErrMsg &errMsg )
{

	switch ( identity.m_eType )
	{
		case k_ESteamNetworkingIdentityType_Invalid:
			V_strcpy_safe( errMsg, "Identity is blank" );
			return false;

		case k_ESteamNetworkingIdentityType_SteamID:
			Assert( identity.m_cbSize == sizeof(identity.m_steamID64) );
			if ( !IsValidSteamIDForIdentity( identity.m_steamID64 ) )
			{
				V_sprintf_safe( errMsg, "Invalid SteamID %llu", identity.m_steamID64 );
				return false;
			}
			msgIdentity->set_steam_id( identity.m_steamID64 );
			break;

		case k_ESteamNetworkingIdentityType_IPAddress:
		{
			COMPILE_TIME_ASSERT( sizeof( SteamNetworkingIPAddr ) == 18 );
			Assert( identity.m_cbSize == sizeof( SteamNetworkingIPAddr ) );
			SteamNetworkingIPAddr tmpAddr( identity.m_ip );
			tmpAddr.m_port = BigWord( tmpAddr.m_port );
			msgIdentity->set_ipv6_and_port( &tmpAddr, sizeof(tmpAddr) );
			break;
		}

		case k_ESteamNetworkingIdentityType_GenericString:
			Assert( identity.m_cbSize == (int)V_strlen( identity.m_szGenericString ) + 1 );
			Assert( identity.m_cbSize > 1 );
			Assert( identity.m_cbSize <= sizeof( identity.m_szGenericString ) );
			msgIdentity->set_generic_string( identity.m_szGenericString );
			break;

		case k_ESteamNetworkingIdentityType_GenericBytes:
			Assert( identity.m_cbSize > 1 );
			Assert( identity.m_cbSize <= sizeof( identity.m_genericBytes ) );
			msgIdentity->set_generic_bytes( identity.m_genericBytes, identity.m_cbSize );
			break;

		default:
			V_sprintf_safe( errMsg, "Unrecognized identity type %d", identity.m_eType );
			return false;
	}

	return true;
}

bool BSteamNetworkingIdentityToProtobufInternal( const SteamNetworkingIdentity &identity, std::string *bytesMsgIdentity, SteamDatagramErrMsg &errMsg )
{
	CMsgSteamNetworkingIdentity msgIdentity;
	if ( !BSteamNetworkingIdentityToProtobufInternal( identity, &msgIdentity, errMsg ) )
		return false;

	if ( !msgIdentity.SerializeToString( bytesMsgIdentity ) )
	{
		// WAT
		V_strcpy_safe( errMsg, "protobuf serialization failed?" );
		return false;
	}

	return true;
}

}
