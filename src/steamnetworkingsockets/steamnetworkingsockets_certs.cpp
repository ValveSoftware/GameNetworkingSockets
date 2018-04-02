//====== Copyright Valve Corporation, All rights reserved. ====================

#include <crypto.h>
#include "steamnetworkingsockets_internal.h"

// Must be the last include
#include <tier0/memdbgon.h>

namespace SteamNetworkingSocketsLib {

#ifdef SDR_SUPPORT_RSA_TICKETS
uint64 CalculatePublicKeyID( const CRSAPublicKey &pubKey )
{
	if ( !pubKey.IsValid() )
		return 0;

	// Get the public modulus
	uint8 modulus[ 2048 ];
	uint32 cbModulus = pubKey.GetModulusBytes( modulus, sizeof(modulus) );
	Assert( cbModulus >= 32 );
	if ( cbModulus < 32 )
		return 0;

	// SHA
	SHA256Digest_t digest;
	DbgVerify( CCrypto::GenerateSHA256Digest( modulus, cbModulus, &digest ) );

	// First 8 bytes
	return LittleQWord( *(uint64*)&digest );
}
#endif

extern uint64 CalculatePublicKeyID( const CECSigningPublicKey &pubKey )
{
	if ( !pubKey.IsValid() )
		return 0;

	// SHA over the whole public key.
	SHA256Digest_t digest;
	DbgVerify( CCrypto::GenerateSHA256Digest( pubKey.GetData(), pubKey.GetLength(), &digest ) );

	// First 8 bytes
	return LittleQWord( *(uint64*)&digest );
}

}
