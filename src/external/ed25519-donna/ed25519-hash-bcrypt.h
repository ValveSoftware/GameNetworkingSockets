#include <windows.h>
#include <bcrypt.h>

typedef struct {
	BCRYPT_ALG_HANDLE algorithm;
	BCRYPT_HASH_HANDLE hash;
	PUCHAR pbBlock;
	ULONG cbBlock;
} ed25519_hash_context;

static void
ed25519_hash_init(ed25519_hash_context *ctx)
{
	ULONG junk;
	memset( ctx, 0, sizeof( ed25519_hash_context ) );
	BCryptOpenAlgorithmProvider( &ctx->algorithm, BCRYPT_SHA512_ALGORITHM, NULL, 0 );
	BCryptGetProperty( ctx->algorithm, BCRYPT_OBJECT_LENGTH, ( PUCHAR )&ctx->cbBlock, sizeof( ctx->cbBlock ), &junk, 0 );
	ctx->pbBlock = HeapAlloc( GetProcessHeap(), 0, ctx->cbBlock );
	BCryptCreateHash( ctx->algorithm, &ctx->hash, ctx->pbBlock, ctx->cbBlock, NULL, 0, 0 );
}

static void
ed25519_hash_update(ed25519_hash_context *ctx, const uint8_t *in, size_t inlen)
{
	BCryptHashData( ctx->hash, (PUCHAR)in, (ULONG)inlen, 0 );
}

static void
ed25519_hash_final(ed25519_hash_context *ctx, uint8_t *hash)
{
	BCryptFinishHash( ctx->hash, hash, 64, 0 );
	BCryptCloseAlgorithmProvider( ctx->algorithm, 0 );
	HeapFree( GetProcessHeap(), 0, ctx->pbBlock );
    memset(ctx, 0, sizeof(ed25519_hash_context));
}

static void
ed25519_hash(uint8_t *hash, const uint8_t *in, size_t inlen)
{
	ed25519_hash_context ctx;
	ed25519_hash_init(&ctx);
	ed25519_hash_update(&ctx, in, inlen);
	ed25519_hash_final(&ctx, hash);
}
