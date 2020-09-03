//========= Copyright Valve LLC, All rights reserved. ========================
#include "crypto.h"

#ifdef STEAMNETWORKINGSOCKETS_CRYPTO_BCRYPT

#include <tier0/vprof.h>
#include <tier1/utlmemory.h>

#include <windows.h>
#include <stdio.h>
#include <bcrypt.h>
#pragma comment(lib, "Bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif

#ifndef NT_ERROR
#define NT_ERROR(Status) ((ULONG)(Status) >> 30 == 3)
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static BCRYPT_ALG_HANDLE hAlgRandom = INVALID_HANDLE_VALUE;
static BCRYPT_ALG_HANDLE hAlgSHA256 = INVALID_HANDLE_VALUE;
static BCRYPT_ALG_HANDLE hAlgHMACSHA256 = INVALID_HANDLE_VALUE;

typedef struct _BCryptContext {
	BCRYPT_ALG_HANDLE hAlgAES;
	HANDLE hKey;
	PUCHAR pbKeyObject;
	ULONG cbKeyObject;

	_BCryptContext() {
		hKey = INVALID_HANDLE_VALUE;
		pbKeyObject = NULL;
		cbKeyObject = 0;
	}

	~_BCryptContext() {
		if (hKey != INVALID_HANDLE_VALUE)
			BCryptDestroyKey(hKey);
		if ( hAlgAES != INVALID_HANDLE_VALUE )
			BCryptCloseAlgorithmProvider( hAlgAES, 0 );
		HeapFree(GetProcessHeap(), 0, pbKeyObject);
	}
} BCryptContext;

void CCrypto::Init()
{
	BCryptOpenAlgorithmProvider(
			&hAlgRandom,
			BCRYPT_RNG_ALGORITHM,
			nullptr,
			0
			);
	AssertFatal( hAlgRandom != INVALID_HANDLE_VALUE );
	BCryptOpenAlgorithmProvider(
			&hAlgSHA256,
			BCRYPT_SHA256_ALGORITHM,
			nullptr,
			0
			);
	AssertFatal( hAlgSHA256 != INVALID_HANDLE_VALUE );
	BCryptOpenAlgorithmProvider(
			&hAlgHMACSHA256,
			BCRYPT_SHA256_ALGORITHM,
			nullptr,
			BCRYPT_ALG_HANDLE_HMAC_FLAG
			);
	AssertFatal( hAlgHMACSHA256 != INVALID_HANDLE_VALUE );
}

SymmetricCryptContextBase::SymmetricCryptContextBase()
{
	m_ctx = NULL;
	m_cbIV = 0;
	m_cbTag = 0;
}

void SymmetricCryptContextBase::Wipe()
{
	delete (BCryptContext *)m_ctx;
	m_ctx = NULL;
	m_cbIV = 0;
	m_cbTag = 0;
}

bool AES_GCM_CipherContext::InitCipher( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag, bool bEncrypt )
{
	DWORD data;
	NTSTATUS ret;

	Wipe();
	m_ctx = new BCryptContext();

	BCryptContext *ctx = (BCryptContext *)(this->m_ctx);

	if ( BCryptOpenAlgorithmProvider( &ctx->hAlgAES, BCRYPT_AES_ALGORITHM, nullptr, 0 ) != 0 )
		return false;
	AssertFatal( ctx->hAlgAES != INVALID_HANDLE_VALUE );

	if ( BCryptGetProperty( ctx->hAlgAES, BCRYPT_OBJECT_LENGTH, ( PBYTE )&ctx->cbKeyObject, sizeof( DWORD ), &data, 0 ) != 0 )
		return false;

	if ( BCryptSetProperty( ctx->hAlgAES, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof( BCRYPT_CHAIN_MODE_GCM ), 0 ) != 0 )
		return false;

	ctx->pbKeyObject = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, ctx->cbKeyObject);
	if (!ctx->pbKeyObject)
		return false;

	if ( (ret = BCryptGenerateSymmetricKey(ctx->hAlgAES, &ctx->hKey, ctx->pbKeyObject, ctx->cbKeyObject, ( PUCHAR )pKey, (ULONG)cbKey, 0 )) != 0 )
		return false;
	AssertFatal( ctx->hKey != INVALID_HANDLE_VALUE );

	m_cbIV = (uint32)cbIV;
	m_cbTag = (uint32)cbTag;

	return true;
}

bool AES_GCM_EncryptContext::Encrypt(
		const void *pPlaintextData, size_t cbPlaintextData,
		const void *pIV,
		void *pEncryptedDataAndTag, uint32 *pcbEncryptedDataAndTag,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData // Optional additional authentication data.  Not encrypted, but will be included in the tag, so it can be authenticated.
		)
{
	BCryptContext *ctx = (BCryptContext *)(this->m_ctx);
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO paddingInfo;
	BCRYPT_INIT_AUTH_MODE_INFO(paddingInfo);
	char buffer[32] = { 0 };
	AssertFatal( m_cbTag <= sizeof( buffer ) );
	paddingInfo.pbTag = m_cbTag ? ( PUCHAR )buffer : NULL;
	paddingInfo.cbTag = m_cbTag;
	paddingInfo.pbNonce = ( PUCHAR )pIV;
	paddingInfo.cbNonce = m_cbIV;
	paddingInfo.cbAuthData = (ULONG)cbAuthenticationData;
	paddingInfo.pbAuthData = cbAuthenticationData ? (PUCHAR)pAdditionalAuthenticationData : NULL;
	ULONG ct_size;
	NTSTATUS status = BCryptEncrypt(
			ctx->hKey,
			( PUCHAR )pPlaintextData, (ULONG)cbPlaintextData,
			&paddingInfo,
			NULL, 0,
			( PUCHAR )pEncryptedDataAndTag, *pcbEncryptedDataAndTag,
			&ct_size,
			0 );
	AssertFatal( ( ct_size + m_cbTag ) < *pcbEncryptedDataAndTag );
	memcpy( ( PUCHAR )( pEncryptedDataAndTag ) + ct_size, buffer, m_cbTag );
	ct_size += m_cbTag;
	*pcbEncryptedDataAndTag = ct_size;
	return NT_SUCCESS(status);
}

bool AES_GCM_DecryptContext::Decrypt(
		const void *pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
		const void *pIV,
		void *pPlaintextData, uint32 *pcbPlaintextData,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData
		)
{
	BCryptContext *ctx = (BCryptContext *)(this->m_ctx);
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO paddingInfo;
	BCRYPT_INIT_AUTH_MODE_INFO(paddingInfo);
	char buffer[32] = { 0 };
	AssertFatal( m_cbTag <= sizeof( buffer ) );
	AssertFatal( m_cbTag <= cbEncryptedDataAndTag );
	memcpy( buffer, ( PUCHAR )pEncryptedDataAndTag + cbEncryptedDataAndTag - m_cbTag, m_cbTag );
	cbEncryptedDataAndTag -= m_cbTag;
	paddingInfo.pbTag = m_cbTag ? ( PUCHAR )buffer : NULL;
	paddingInfo.cbTag = m_cbTag;
	paddingInfo.pbNonce = (PUCHAR)pIV;
	paddingInfo.cbNonce = m_cbIV;
	paddingInfo.cbAuthData = (ULONG)cbAuthenticationData;
	paddingInfo.pbAuthData = cbAuthenticationData ? (PUCHAR)pAdditionalAuthenticationData : NULL;
	ULONG pt_size;
	NTSTATUS status = BCryptDecrypt(
			ctx->hKey,
			( PUCHAR )pEncryptedDataAndTag, (ULONG)cbEncryptedDataAndTag,
			&paddingInfo,
			NULL, 0,
			( PUCHAR )pPlaintextData, *pcbPlaintextData,
			&pt_size,
			0 );
	*pcbPlaintextData = pt_size;
	return NT_SUCCESS(status);
}

//-----------------------------------------------------------------------------
// Purpose: Generate a SHA256 hash
// Input:	pchInput -			Plaintext string of item to hash (null terminated)
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
void CCrypto::GenerateSHA256Digest( const void *pInput, size_t cbInput, SHA256Digest_t *pOutDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSHA256Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	//Assert( pubInput );
	Assert( pOutDigest );

	BCRYPT_HASH_HANDLE hHashSHA256 = INVALID_HANDLE_VALUE;
	PUCHAR pbBuffer = NULL;
	NTSTATUS status;
	static ULONG cbBuffer = 0;
	if (!cbBuffer) {
		ULONG garbage;
		status = BCryptGetProperty(hAlgSHA256, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbBuffer, sizeof(cbBuffer), &garbage, 0 );
		AssertFatal(NT_SUCCESS(status));
	}
	Assert( cbBuffer > 0 && cbBuffer < 16 * 1024 * 1024 );
	pbBuffer = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbBuffer);
	AssertFatal( pbBuffer );
	status = BCryptCreateHash(hAlgSHA256, &hHashSHA256, pbBuffer, cbBuffer, NULL, 0, 0);
	AssertFatal(NT_SUCCESS(status));
	status = BCryptHashData(hHashSHA256, (PUCHAR)pInput, (ULONG)cbInput, 0);
	AssertFatal(NT_SUCCESS(status));
	status = BCryptFinishHash(hHashSHA256, *pOutDigest, sizeof(SHA256Digest_t), 0);
	AssertFatal(NT_SUCCESS(status));
	status = BCryptDestroyHash(hHashSHA256);
	AssertFatal(NT_SUCCESS(status));
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

	BCRYPT_HASH_HANDLE hHashHMACSHA256 = INVALID_HANDLE_VALUE;
	PUCHAR pbBuffer = NULL;
	NTSTATUS status;
	static ULONG cbBuffer = 0;
	if (!cbBuffer) {
		ULONG garbage;
		status = BCryptGetProperty(hAlgHMACSHA256, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbBuffer, sizeof(cbBuffer), &garbage, 0 );
		AssertFatal(NT_SUCCESS(status));
	}
	Assert( cbBuffer > 0 && cbBuffer < 16 * 1024 * 1024 );
	pbBuffer = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbBuffer);
	AssertFatal( pbBuffer );
	status = BCryptCreateHash(hAlgHMACSHA256, &hHashHMACSHA256, pbBuffer, cbBuffer, (PUCHAR)pubKey, cubKey, 0);
	AssertFatal(NT_SUCCESS(status));
	status = BCryptHashData(hHashHMACSHA256, (PUCHAR)pubData, (ULONG)cubData, 0);
	AssertFatal(NT_SUCCESS(status));
	status = BCryptFinishHash(hHashHMACSHA256, *pOutputDigest, sizeof(SHA256Digest_t), 0);
	AssertFatal(NT_SUCCESS(status));
	status = BCryptDestroyHash(hHashHMACSHA256);
	AssertFatal(NT_SUCCESS(status));
}

#endif // GNS_CRYPTO_AES_BCRYPT
