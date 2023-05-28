//========= Copyright Valve LLC, All rights reserved. ========================

#include "crypto.h"
#ifdef VALVE_CRYPTO_OPENSSL

#include <tier0/dbg.h>
#include <tier0/vprof.h>

#include "tier0/memdbgoff.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include "tier0/memdbgon.h"

#if OPENSSL_VERSION_NUMBER < 0x10100000
inline void EVP_MD_CTX_free( EVP_MD_CTX *ctx )
{
	EVP_MD_CTX_destroy( ctx );
}
#endif

template < typename CTXType, void(*CleanupFunc)(CTXType)>
class EVPCTXPointer
{
	public:
		CTXType ctx;

		EVPCTXPointer() { this->ctx = NULL; }
		EVPCTXPointer(CTXType ctx) { this->ctx = ctx; }
		~EVPCTXPointer() { CleanupFunc(ctx); }
};

//-----------------------------------------------------------------------------
// Generate a SHA256 hash
//-----------------------------------------------------------------------------
void CCrypto::GenerateSHA256Digest( const void *pInput, size_t cbInput, SHA256Digest_t *pOutDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSHA256Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	//Assert( pubInput );
	Assert( pOutDigest );

	EVPCTXPointer<EVP_MD_CTX *, EVP_MD_CTX_free> ctx(EVP_MD_CTX_create());

	unsigned int digest_len = sizeof(SHA256Digest_t);
	VerifyFatal(ctx.ctx != NULL);
	VerifyFatal(EVP_DigestInit_ex(ctx.ctx, EVP_sha256(), NULL) == 1);
	VerifyFatal(EVP_DigestUpdate(ctx.ctx, pInput, cbInput) == 1);
	VerifyFatal(EVP_DigestFinal(ctx.ctx, *pOutDigest, &digest_len) == 1);
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

	EVPCTXPointer<EVP_MD_CTX *, EVP_MD_CTX_free> mdctx(EVP_MD_CTX_create());
	EVPCTXPointer<EVP_PKEY *, EVP_PKEY_free> pkey(EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, pubKey, cubKey));
	const EVP_MD *digest = EVP_sha256();

	VerifyFatal(mdctx.ctx != NULL && pkey.ctx != NULL);
	VerifyFatal(EVP_DigestInit_ex(mdctx.ctx, digest, NULL) == 1);
	VerifyFatal(EVP_DigestSignInit(mdctx.ctx, NULL, digest, NULL, pkey.ctx) == 1);
	VerifyFatal(EVP_DigestSignUpdate(mdctx.ctx, pubData, cubData) == 1);

	size_t needed = sizeof(SHA256Digest_t);
	VerifyFatal(EVP_DigestSignFinal(mdctx.ctx, *pOutputDigest, &needed) == 1);
}

//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using SHA1
//-----------------------------------------------------------------------------
void CCrypto::GenerateHMAC( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHADigest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMAC", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );
	
	unsigned int needed = (unsigned int)sizeof(SHADigest_t);
	HMAC( EVP_sha1(), pubKey, cubKey, pubData, cubData, (unsigned char*)pOutputDigest, &needed );
}

#endif // VALVE_CRYPTO_OPENSSL
