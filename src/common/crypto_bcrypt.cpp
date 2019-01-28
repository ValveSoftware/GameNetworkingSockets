//========= Copyright Valve LLC, All rights reserved. ========================

#include <tier0/vprof.h>
#include <tier1/utlmemory.h>
#include "crypto.h"

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

SymmetricCryptContextBase::SymmetricCryptContextBase()
{
	m_cbIV = 0;
	m_cbTag = 0;
}

void SymmetricCryptContextBase::Wipe()
{
	m_cbIV = 0;
	m_cbTag = 0;
}

bool AES_GCM_CipherContext::InitCipher( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag, bool bEncrypt )
{
	return false;
}

bool AES_GCM_EncryptContext::Encrypt(
	const void *pPlaintextData, size_t cbPlaintextData,
	const void *pIV,
	void *pEncryptedDataAndTag, uint32 *pcbEncryptedDataAndTag,
	const void *pAdditionalAuthenticationData, size_t cbAuthenticationData // Optional additional authentication data.  Not encrypted, but will be included in the tag, so it can be authenticated.
)
{
	return false;
}

bool AES_GCM_DecryptContext::Decrypt(
	const void *pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
	const void *pIV,
	void *pPlaintextData, uint32 *pcbPlaintextData,
	const void *pAdditionalAuthenticationData, size_t cbAuthenticationData
)
{
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
