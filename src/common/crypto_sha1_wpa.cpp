#include "crypto.h"
#include <tier0/dbg.h>
#include <tier0/vprof.h>

#ifdef VALVE_CRYPTO_SHA1_WPA

extern "C" {
// external headers for sha1 and hmac-sha1 support
#include "../external/sha1-wpa/sha1.h"
}

//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using SHA1
// Input:	pubData -			Plaintext data to digest
//			cubData -			length of data
//			pubKey -			key to use in HMAC
//			cubKey -			length of key
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
void CCrypto::GenerateHMAC( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHADigest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMAC", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );

	int status = hmac_sha1(pubKey, cubKey, pubData, cubData, (uint8_t*)pOutputDigest);
	AssertFatal(status == 0);
}

#endif // #ifdef VALVE_CRYPTO_SHA1_WPA

