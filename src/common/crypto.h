//========= Copyright Valve LLC, All rights reserved. =========================

#ifndef CRYPTO_H
#define CRYPTO_H

#include <tier0/platform.h>
#include "crypto_constants.h"
//#include <tier1/passwordhash.h>
#include "keypair.h"

class CUtlBuffer;

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
public:

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

	bool HexEncode( const void *pubData, const uint32 cubData, char *pchEncodedData, uint32 cchEncodedData );
	bool HexDecode( const char *pchData, void *pubDecodedData, uint32 *pcubDecodedData );

	uint32 Base64EncodeMaxOutput( uint32 cubData, const char *pszLineBreakOrNull );
	bool Base64Encode( const void *pubData, uint32 cubData, char *pchEncodedData, uint32 cchEncodedData, bool bInsertLineBreaks = true ); // legacy, deprecated
	bool Base64Encode( const void *pubData, uint32 cubData, char *pchEncodedData, uint32 *pcchEncodedData, const char *pszLineBreak = "\n" );

	inline uint32 Base64DecodeMaxOutput( uint32 cubData ) { return ( (cubData + 3 ) / 4) * 3 + 1; }
	bool Base64Decode( const char *pchEncodedData, void *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true ); // legacy, deprecated
	bool Base64Decode( const char *pchEncodedData, uint32 cchEncodedData, void *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true );

	/// Decode base64 into CUtlBuffer.  This precomputes the expetced size and does
	/// only one allocation, so it's safe to use CAutoWipeBuffer.
	bool DecodeBase64ToBuf( const char *pszEncoded, uint32 cbEncoded, CUtlBuffer &buf );

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

	/// Decode base-64 encoded PEM-like thing into buffer.  it's safe to use CAutoWipeBuffer
	bool DecodePEMBody( const char *pszPem, uint32 cch, CUtlBuffer &buf, const char *pszExpectedType );

	/// Use platform-dependency secure random number source of high entropy
	void GenerateRandomBlock( void *pubDest, int cubDest );

	void GenerateSHA256Digest( const void *pData, size_t cbData, SHA256Digest_t *pOutputDigest );

	// GenerateHMAC256 is our implementation of HMAC-SHA256. Relatively future-proof. You should probably use this unless you have a very good reason not to.
	void GenerateHMAC256( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHA256Digest_t *pOutputDigest );

	/// Used for fast hashes that are reasonably secure
	typedef uint64_t SipHashKey_t[2];
	uint64_t SipHash( const void *data, size_t cbData, const SipHashKey_t &k );
}

#endif // CRYPTO_H
