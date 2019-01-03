//========= Copyright Valve LLC, All rights reserved. ========================

#include "crypto.h"
#include <tier0/vprof.h>

#include <windows.h>
#include <stdio.h>
#include <Bcrypt.h>
#pragma comment(lib, "Bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif

#ifndef NT_ERROR
#define NT_ERROR(Status) ((ULONG)(Status) >> 30 == 3)
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static BCRYPT_ALG_HANDLE hAlgRandom;
static BCRYPT_ALG_HANDLE hAlgAES;

void CCrypto::Init()
{
	BCryptOpenAlgorithmProvider(
		&hAlgRandom,
		BCRYPT_RNG_ALGORITHM,
		nullptr,
		0
	);
	AssertFatal( hAlgRandom );

	BCryptOpenAlgorithmProvider(
		&hAlgAES,
		BCRYPT_AES_ALGORITHM,
		nullptr,
		0
	);
	AssertFatal( hAlgAES );

}

//-----------------------------------------------------------------------------
bool CCrypto::SymmetricEncryptWithIV( const uint8 * pubPlaintextData, uint32 cubPlaintextData,
	const uint8 * pIV, uint32 cubIV, uint8 * pubEncryptedData,
	uint32 * pcubEncryptedData, const uint8 * pubKey, uint32 cubKey )
{

	// IV input into CBC must be exactly one block size
	if ( cubIV != k_nSymmetricBlockSize )
		return false;

	// FIXME
	Assert( false );
	return false;
}



//-----------------------------------------------------------------------------
bool CCrypto::SymmetricDecryptWithIV( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
								const uint8 * pIV, uint32 cubIV,
								uint8 *pubPlaintextData, uint32 *pcubPlaintextData, 
								const uint8 *pubKey, const uint32 cubKey, bool bVerifyPaddingBytes )
{
	Assert( pubEncryptedData );
	Assert( cubEncryptedData);
	Assert( pIV );
	Assert( cubIV );
	Assert( pubPlaintextData );
	Assert( pcubPlaintextData );
	Assert( *pcubPlaintextData );
	Assert( pubKey );
	Assert( k_nSymmetricKeyLen256 == cubKey || k_nSymmetricKeyLen128 == cubKey );

	// IV input into CBC must be exactly one block size
	if ( cubIV != k_nSymmetricBlockSize )
		return false;

	// FIXME
	Assert( false );
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Generate a SHA256 hash
// Input:	pchInput -			Plaintext string of item to hash (null terminated)
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
void CCrypto::GenerateSHA256Digest( const uint8 *pubInput, const int cubInput, SHA256Digest_t *pOutDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSHA256Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	//Assert( pubInput );
	Assert( cubInput >= 0 );
	Assert( pOutDigest );
}

//-----------------------------------------------------------------------------
// Purpose: Generates a cryptographiacally random block of data fit for any use.
// NOTE: Function terminates process on failure rather than returning false!
//-----------------------------------------------------------------------------
void CCrypto::GenerateRandomBlock( void *pvDest, int cubDest )
{
	VPROF_BUDGET( "CCrypto::GenerateRandomBlock", VPROF_BUDGETGROUP_ENCRYPTION );
	AssertFatal( cubDest >= 0 );

	NTSTATUS status = BCryptGenRandom(
		hAlgRandom,
		(PUCHAR)pvDest,
		(ULONG)cubDest,
		0
	);
	AssertFatal( NT_SUCCESS( status) );
}


//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using SHA-256
//-----------------------------------------------------------------------------
void CCrypto::GenerateHMAC256( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHA256Digest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMAC256", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );

}
