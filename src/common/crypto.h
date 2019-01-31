//========= Copyright Valve LLC, All rights reserved. =========================

#ifndef CRYPTO_H
#define CRYPTO_H

#include <tier0/dbg.h>		// for Assert & AssertMsg
//#include "tier1/passwordhash.h"
#include "keypair.h"
#include "steam/steamtypes.h" // Salt_t
#include "crypto_constants.h"

const unsigned int k_cubMD5Hash = 16;
typedef	unsigned char MD5Digest_t[k_cubMD5Hash];

const unsigned int k_cubSHA256Hash = 32;
typedef	unsigned char SHA256Digest_t[ k_cubSHA256Hash ];

const unsigned int k_cubSHA1Hash = 20;
typedef	uint8 SHADigest_t[ k_cubSHA1Hash ];

// Base class for symmetric encryption and decryption context.
// A context is used when you want to use the same encryption
// parameters repeatedly:
// - encrypt or decrypt
// - key
// - IV size (but not the IV itself, that should vary per packet!)
// - tag size (if AES-GCM)
class SymmetricCryptContextBase
{
public:
	SymmetricCryptContextBase();
	~SymmetricCryptContextBase() { Wipe(); }
	void Wipe();

protected:
	void *m_ctx;

	uint32 m_cbIV, m_cbTag;
};

// Base class for AES-GCM encryption and ddecryption
class AES_GCM_CipherContext : public SymmetricCryptContextBase
{
protected:

	// Initialize context with the specified private key, IV size, and tag size
	bool InitCipher( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag, bool bEncrypt );
};

class AES_GCM_EncryptContext : public AES_GCM_CipherContext
{
public:

	// Initialize context with the specified private key, IV size, and tag size
	inline bool Init( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag )
	{
		return InitCipher( pKey, cbKey, cbIV, cbTag, true );
	}

	// Encrypt data and append auth tag
	bool Encrypt(
		const void *pPlaintextData, size_t cbPlaintextData,
		const void *pIV,
		void *pEncryptedDataAndTag, uint32 *pcbEncryptedDataAndTag,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData // Optional additional authentication data.  Not encrypted, but will be included in the tag, so it can be authenticated.
	);
};

class AES_GCM_DecryptContext : public AES_GCM_CipherContext
{
public:

	// Initialize context with the specified private key, IV size, and tag size
	inline bool Init( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag )
	{
		return InitCipher( pKey, cbKey, cbIV, cbTag, false );
	}

	// Decrypt data and check auth tag, which is assumed to be at the end
	bool Decrypt(
		const void *pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
		const void *pIV,
		void *pPlaintextData, uint32 *pcbPlaintextData,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData // Optional additional authentication data.  Not encrypted, but will be included in the tag, so it can be authenticated.
	);
};

namespace CCrypto
{
	void Init();
	
	// Symmetric encryption and authentication using AES-GCM.
	bool SymmetricAuthEncryptWithIV(
		const void *pPlaintextData, size_t cbPlaintextData,
		const void *pIV, size_t cbIV,
		void *pEncryptedDataAndTag, uint32 *pcbEncryptedDataAndTag,
		const void *pKey, size_t cbKey,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData, // Optional additional authentication data.  Not encrypted, but will be included in the tag, so it can be authenticated.
		size_t cbTag // Number of tag bytes to append to the end of the buffer
	);

	// Symmetric decryption and check authentication tag using AES-GCM.
	bool SymmetricAuthDecryptWithIV(
		const void *pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
		const void *pIV, size_t cbIV,
		void *pPlaintextData, uint32 *pcbPlaintextData,
		const void *pKey, size_t cbKey,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData, // Optional additional authentication data.  Not encrypted, but will be included in the tag, so it can be authenticated.
		size_t cbTag // Last N bytes in your buffer are assumed to be a tag, and will be checked
	);
	
	//
	// Secure key exchange (curve25519 elliptic-curve Diffie-Hellman key exchange)
	//

	// Generate a curve25519 key pair for Diffie-Hellman secure key exchange
	void GenerateKeyExchangeKeyPair( CECKeyExchangePublicKey *pPublicKey, CECKeyExchangePrivateKey *pPrivateKey );
	bool PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut );

	//
	// Signing and verification (ed25519 elliptic-curve signature scheme)
	//

	// Generate an ed25519 key pair for public-key signature generation
	void GenerateSigningKeyPair( CECSigningPublicKey *pPublicKey, CECSigningPrivateKey *pPrivateKey );

	// Legacy compatibility - use the key methods
	inline void GenerateSignature( const void *pData, size_t cbData, const CECSigningPrivateKey &privateKey, CryptoSignature_t *pSignatureOut ) { privateKey.GenerateSignature( pData, cbData, pSignatureOut ); }
	inline bool VerifySignature( const void *pData, size_t cbData, const CECSigningPublicKey &publicKey, const CryptoSignature_t &signature ) { return publicKey.VerifySignature( pData, cbData, signature ); }

	bool HexEncode( const void *pubData, const uint32 cubData, char *pchEncodedData, uint32 cchEncodedData );
	bool HexDecode( const char *pchData, void *pubDecodedData, uint32 *pcubDecodedData );

	uint32 Base64EncodeMaxOutput( uint32 cubData, const char *pszLineBreakOrNull );
	bool Base64Encode( const void *pubData, uint32 cubData, char *pchEncodedData, uint32 cchEncodedData, bool bInsertLineBreaks = true ); // legacy, deprecated
	bool Base64Encode( const void *pubData, uint32 cubData, char *pchEncodedData, uint32 *pcchEncodedData, const char *pszLineBreak = "\n" );

	inline uint32 Base64DecodeMaxOutput( uint32 cubData ) { return ( (cubData + 3 ) / 4) * 3 + 1; }
	bool Base64Decode( const char *pchEncodedData, void *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true ); // legacy, deprecated
	bool Base64Decode( const char *pchEncodedData, uint32 cchEncodedData, void *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true );

	/// Parse a PEM-like thing, locating the hex-encoded body (but not parsing it.)
	/// On success, will returns a pointer to the first character of the body,
	/// and update *pcch to reflect its size.  You can then pass this
	/// size to Base64DecodeMaxOutput and Base64Decode to get the actual data.
	///
	/// pszExpectedType is string you expect to find in the header and footer,
	/// excluding the "BEGIN" and "END".  For example, you can pass "PRIVATE KEY"
	/// to check that the header contains something like "-----BEGIN PRIVATE KEY-----"
	/// and the header contains "-----END PRIVATE KEY-----".  If you pass NULL,
	/// then we just check that the header and footer contain something vaguely PEM-like.
	const char *LocatePEMBody( const char *pchPEM, uint32 *pcch, const char *pszExpectedType );

	void GenerateRandomBlock( void *pubDest, int cubDest );

	void GenerateSHA256Digest( const uint8 *pubData, const int cubData, SHA256Digest_t *pOutputDigest );

	// GenerateHMAC256 is our implementation of HMAC-SHA256. Relatively future-proof. You should probably use this unless you have a very good reason not to.
	void GenerateHMAC256( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHA256Digest_t *pOutputDigest );
}

#endif // CRYPTO_H
