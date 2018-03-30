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

const unsigned int k_cubCryptoSignature = 64;
typedef unsigned char CryptoSignature_t[ k_cubCryptoSignature ];

const unsigned int k_cubSHA1Hash = 20;
typedef	uint8 SHADigest_t[ k_cubSHA1Hash ];

class CCrypto
{
public:

	enum ECDSACurve {
		k_ECDSACurve_secp256k1,
		k_ECDSACurve_secp256r1,
	};
	
	static uint32 GetSymmetricEncryptedSize( uint32 cubPlaintextData );

	// Standard AES (128- or 256-bit), PKCS#7 padding, random IV is encrypted then prepended.
	// Output is always 17-32 bytes longer than input, filling up to a multiple of 16.
	static bool SymmetricEncrypt( const uint8 * pubPlaintextData, uint32 cubPlaintextData, 
								  uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
								  const uint8 * pubKey, uint32 cubKey );

	// SymmetricEncryptChosenIV is compatible with SymmetricEncrypt, but uses a specifically
	// chosen initialization vector instead of a randomly generated one. GENERALLY NOT RECOMMENDED.
	static bool SymmetricEncryptChosenIV( const uint8 * pubPlaintextData, uint32 cubPlaintextData,
										  const uint8 * pIV, uint32 cubIV,
										  uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
										  const uint8 * pubKey, uint32 cubKey );

	// SymmetricEncryptWithIV is NOT compatible with SymmetricDecrypt, because it does not write
	// the IV into the data stream - it is assumed that the IV is communicated or agreed upon by
	// some other out-of-band method. Pair it with SymmetricDecryptWithIV to decrpyt. Output is
	// always 1-16 bytes longer than input due to PKCS#7 block padding.
	static bool SymmetricEncryptWithIV( const uint8 * pubPlaintextData, uint32 cubPlaintextData,
										const uint8 * pIV, uint32 cubIV,
										uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
										const uint8 * pubKey, uint32 cubKey );

	static bool SymmetricDecrypt( const uint8 * pubEncryptedData, uint32 cubEncryptedData, 
								  uint8 * pubPlaintextData, uint32 * pcubPlaintextData,
								  const uint8 * pubKey, uint32 cubKey );

	static bool SymmetricDecryptRecoverIV( const uint8 * pubEncryptedData, uint32 cubEncryptedData,
		uint8 * pubPlaintextData, uint32 * pcubPlaintextData, uint8 *pIV, uint32 cubIV,
		const uint8 * pubKey, uint32 cubKey, bool bVerifyPaddingBytes = true );

	
	// SymmetricDecryptWithIV assumes that the encrypted data does not begin with an IV.
	// If you created the encrypted data with SymmetricEncryptChosenIV, you must discard
	// the first 16 bytes of encrypted output before passing it to SymmetricDecryptWithIV.
	static bool SymmetricDecryptWithIV( const uint8 * pubEncryptedData, uint32 cubEncryptedData, 
										const uint8 * pIV, uint32 cubIV,
										uint8 * pubPlaintextData, uint32 * pcubPlaintextData,
										const uint8 * pubKey, uint32 cubKey, bool bVerifyPaddingBytes = true );
	
	//
	// Secure key exchange (curve25519 elliptic-curve Diffie-Hellman key exchange)
	//
	static void GenerateKeyExchangeKeyPair( CECKeyExchangePublicKey *pPublicKey, CECKeyExchangePrivateKey *pPrivateKey );
	static void PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut );

	//
	// Signing and verification (ed25519 elliptic-curve signature scheme)
	//
	static void GenerateSigningKeyPair( CECSigningPublicKey *pPublicKey, CECSigningPrivateKey *pPrivateKey );
	static void GenerateSignature( const uint8 *pubData, uint32 cubData, const CECSigningPrivateKey &privateKey, CryptoSignature_t *pSignatureOut );
	static bool VerifySignature( const uint8 *pubData, uint32 cubData, const CECSigningPublicKey &publicKey, const CryptoSignature_t &signature );


	static bool RSAGenerateKeys( uint32 cKeyBits, CRSAPublicKey &rsaKeyPublic, CRSAPrivateKey &rsaKeyPrivate );

	//
	// RSA encryption of small data blocks - usable for authenticated key exchange
	// (deprecated, prefer key exchange followed by AES-GCM or similar authenticated encryption)
	//
	static uint32 GetRSAMaxPlaintextSize( const CRSAPublicKey &rsaKey );
	static uint32 GetRSAEncryptionBlockSize( const CRSAPublicKey &rsaKey );
	static bool RSAEncrypt( const uint8 *pubPlaintextPlaintextData, const uint32 cubData, uint8 *pubEncryptedData, 
		uint32 *pcubEncryptedData, const CRSAPublicKey &rsaKey );
	static bool RSADecrypt( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
		uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const CRSAPrivateKey &rsaKey );
	// decrypt a payload which was encrypted using PCKS #1 v1.5 padding instead of OAEP 
	static bool RSADecryptPKCSv15( const uint8 *pubEncryptedData, uint32 cubEncryptedData,
		uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const CRSAPrivateKey &rsaKey );
	// decrypt using a public key, and no padding. DO NOT USE. only kept for compatibility with old systems.
	static bool RSAPublicDecrypt_NoPadding_DEPRECATED( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
		uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const CRSAPrivateKey &rsaKey );

	//
	// Signing and verification (RSA signatures - slower, larger, old-school)
	//
	static uint32 GetRSASignatureSize( const CRSAPrivateKey &rsaKey );
	static uint32 GetRSASignatureSize( const CRSAPublicKey &rsaKey );
	static bool RSASign( const uint8 *pubData, const uint32 cubData, 
		uint8 *pubSignature, uint32 *pcubSignature, 
		const CRSAPrivateKey &rsaKey );
	static bool RSAVerifySignature( const uint8 *pubData, const uint32 cubData, 
		const uint8 *pubSignature, const uint32 cubSignature,
		const CRSAPublicKey &rsaKey );
	static bool RSASignSHA256(	const uint8 *pubData, const uint32 cubData, 
		uint8 *pubSignature, uint32 *pcubSignature, 
		const CRSAPrivateKey &rsaKey );
	static bool RSAVerifySignatureSHA256(	const uint8 *pubData, const uint32 cubData, 
		const uint8 *pubSignature, const uint32 cubSignature, 
		const CRSAPublicKey &rsaKey );

#ifdef _SERVER
	//
	// Signing and verification (ECDSA signatures)
	//
	static bool ECDSASign( const uint8 *pubData, const uint32 cubData,
		uint8 *pubSignature, uint32 *pcubSignature,
		const uint8 * pubPrivateKey, const uint32 cubPrivateKey, ECDSACurve eCurve = k_ECDSACurve_secp256k1 );
	static bool ECDSAVerifySignature( const uint8 *pubData, const uint32 cubData,
		const uint8 *pubSignature, const uint32 cubSignature,
		const uint8 *pubPublicKey, const uint32 cubPublicKey, ECDSACurve eCurve = k_ECDSACurve_secp256k1 );
	static bool ECDSASignSHA256( const uint8 *pubData, const uint32 cubData,
		uint8 *pubSignature, uint32 *pcubSignature,
		const uint8 * pubPrivateKey, const uint32 cubPrivateKey, ECDSACurve eCurve = k_ECDSACurve_secp256k1 );
	static bool ECDSAVerifySignatureSHA256( const uint8 *pubData, const uint32 cubData,
		const uint8 *pubSignature, const uint32 cubSignature,
		const uint8 *pubPublicKey, const uint32 cubPublicKey, ECDSACurve eCurve = k_ECDSACurve_secp256k1 );
#endif

	//
	// These are deprecated because they are not secure enough.
	//	1.	the key-derivation function (SHA256 single round) is fast, meaning easy to brute-force
	//		for low-entropy passwords
	//  2.	including the full HMAC actually makes it easier to brute-force, because you just
	//		attack the HMAC and get a definitive "yes" when you use the right key.
	//		A truncated MAC (eg just one or two bytes) would still provide relatively good
	//		typo protection, and would make the list of possible passphrases much larger.
	//		However, then they just have to run the AES decryption once for each of those and
	//		pick the results that look most likely to be the plaintext.
	//	So, don't use these.
	static bool EncryptWithPasswordAndHMAC_DEPRECATED( const uint8 *pubPlaintextData, uint32 cubPlaintextData,
									 uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
									 const char *pchPassword );

	static bool EncryptWithPasswordAndHMACWithIV_DEPRECATED( const uint8 *pubPlaintextData, uint32 cubPlaintextData,
											const uint8 * pIV, uint32 cubIV,
											uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
											const char *pchPassword );


	static bool DecryptWithPasswordAndAuthenticate_DEPRECATED( const uint8 * pubEncryptedData, uint32 cubEncryptedData, 
									 uint8 * pubPlaintextData, uint32 * pcubPlaintextData,
									 const char *pchPassword );

	// EncryptWithPassphrase / DecryptWithPassphrase uses the following format:
	//  <1 byte algorithm ID> <1 byte parameter> <16 bytes random IV> <AES-256 CBC of data> <HMAC-SHA256 of all preceding bytes>
	//
	// The resulting size is always ( 16*n + 2 ), which helps distinguish it from plain AES-256 CBC.
	//
	// Let "intermediate secret" be HashAlgorithm( passphrase, parameter, random IV ):
	//   key for AES-256 CBC is HMAC-SHA256( key = intermediate secret, signed data = 4 bytes "AES\x01" )
	//   key for HMAC-SHA256 is HMAC-SHA256( key = intermediate secret, signed data = 4 bytes "HMAC" )
	//
	// The defined password hashing algorithm are currently
	//   0x01 = PBKDF2( HMAC-SHA256, rounds = 2^(parameter byte) ) 
	//   0x02 = scrypt-jane( HMAC-SHA256, Salsa20/8, params=16/4/0 ) with parameter byte always 0x00
	//
	// Note that EncryptWithPassphrase_Strong is designed to be very slow! Possibly even 2+ seconds.
	// Each decryption attempt to will take exactly as long as encryption, which is the whole point.
	// For things which need to be encrypted and decrypted rapidly, use EncryptWithPassphrase_Fast
	// but be aware that this *dramatically* reduces the expense of cracking the PBKDF2 passphrase.
	// (The Fast variant is still in line with good practices for password hashing as of early 2016.)
	//
	// A stronger passphrase is always the best defense against offline cracking. Any algorithm can
	// only be a fixed work multiplier, compared to the exponential increase of a longer passphrase.
	//
	static bool EncryptWithPassphrase_Strong( const uint8 *pubPlaintextData, uint32 cubPlaintextData, uint8 * pubEncryptedData, uint32 * pcubEncryptedData, const char *pchPassphrase, int nPassphraseLength = -1 );
	static bool EncryptWithPassphrase_Fast( const uint8 *pubPlaintextData, uint32 cubPlaintextData, uint8 * pubEncryptedData, uint32 * pcubEncryptedData, const char *pchPassphrase, int nPassphraseLength = -1 );
	static bool DecryptWithPassphrase( const uint8 *pubEncryptedData, uint32 cubEncryptedData, uint8 * pubPlaintextData, uint32 * pcubPlaintextData, const char *pchPassphrase, int nPassphraseLength = -1, bool bVerifyIntegrity = true );

	// A variation of EncryptWithPassphrase_Strong with no CBC trailing padding and no integrity
	// checks, since the decryption can be validated against the public key. Extremely compact;
	// always 1 + 1 + 16 + 32 bytes = 50 bytes.
	static bool EncryptECPrivateKeyWithPassphrase( const CEC25519PrivateKeyBase &privateKey, uint8 *pubEncryptedData, uint32 *pcubEncryptedData, const char *pchPassphrase, int nPassphraseLength = -1, bool bStrong = true );
	static bool DecryptECPrivateKeyWithPassphrase( const uint8 *pubEncryptedData, uint32 cubEncryptedData, CEC25519PrivateKeyBase *pPrivateKey, const char *pchPassphrase, int nPassphraseLength = -1 );

protected:
	static bool RSAEncrypt( const uint8 *pubPlaintextData, const uint32 cubData, uint8 *pubEncryptedData, 
							uint32 *pcubEncryptedData, const uint8 *pubPublicKey, const uint32 cubPublicKey );
	static bool RSADecrypt( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
							uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const uint8 *pubPrivateKey, const uint32 cubPrivateKey, bool bLegacyPKCSv15 );

	// decrypt using a public key, and no padding
	static bool RSAPublicDecrypt_NoPadding_DEPRECATED( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
							uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const uint8 *pubPublicKey, const uint32 cubPublicKey );

	static bool RSASign( const uint8 *pubData, const uint32 cubData, 
						 uint8 *pubSignature, uint32 *pcubSignature, 
						const uint8 * pubPrivateKey, const uint32 cubPrivateKey );
	static bool RSAVerifySignature( const uint8 *pubData, const uint32 cubData, 
									const uint8 *pubSignature, const uint32 cubSignature, 
									const uint8 *pubPublicKey, const uint32 cubPublicKey );

	static bool RSASignSHA256( const uint8 *pubData, const uint32 cubData, 
						 uint8 *pubSignature, uint32 *pcubSignature, 
						const uint8 * pubPrivateKey, const uint32 cubPrivateKey );
	static bool RSAVerifySignatureSHA256( const uint8 *pubData, const uint32 cubData, 
									const uint8 *pubSignature, const uint32 cubSignature, 
									const uint8 *pubPublicKey, const uint32 cubPublicKey );

	typedef bool (*PassphraseHelperFn_t)( uint8 ( &rgubDigest )[32], const char *pchPassphrase, int nPassphraseLength, const uint8 *pubSalt, uint32 cubSalt, uint8 ubAlgorithmID, uint8 ubParameter );
	static bool PassphraseHelper_PBKDF2SHA256( uint8 ( &rgubDigest )[32], const char *pchPassphrase, int nPassphraseLength, const uint8 *pubSalt, uint32 cubSalt, uint8 ubAlgorithmID, uint8 ubParameter );
	static bool PassphraseHelper_Scrypt( uint8 ( &rgubDigest )[32], const char *pchPassphrase, int nPassphraseLength, const uint8 *pubSalt, uint32 cubSalt, uint8 ubAlgorithmID, uint8 ubParameter );

	static bool EncryptWithPassphraseImpl( const uint8 *pubPlaintextData, uint32 cubPlaintextData, uint8 * pubEncryptedData, uint32 * pcubEncryptedData, const char *pchPassphrase, int nPassphraseLength, bool bStrong );
	static bool DecryptWithPassphraseImpl( const uint8 *pubEncryptedData, uint32 cubEncryptedData, uint8 * pubPlaintextData, uint32 * pcubPlaintextData, const char *pchPassphrase, int nPassphraseLength, bool bVerifyIntegrity );

public:
	static bool HexEncode( const uint8 *pubData, const uint32 cubData, char *pchEncodedData, uint32 cchEncodedData );
	static bool HexDecode( const char *pchData, uint8 *pubDecodedData, uint32 *pcubDecodedData );

	static uint32 Base64EncodeMaxOutput( uint32 cubData, const char *pszLineBreakOrNull );
	static bool Base64Encode( const uint8 *pubData, uint32 cubData, char *pchEncodedData, uint32 cchEncodedData, bool bInsertLineBreaks = true ); // legacy, deprecated
	static bool Base64Encode( const uint8 *pubData, uint32 cubData, char *pchEncodedData, uint32 *pcchEncodedData, const char *pszLineBreak = "\n" );

	static uint32 Base64DecodeMaxOutput( uint32 cubData ) { return ( (cubData + 3 ) / 4) * 3 + 1; }
	static bool Base64Decode( const char *pchEncodedData, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true ); // legacy, deprecated
	static bool Base64Decode( const char *pchEncodedData, uint32 cchEncodedData, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true );

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
	static const char *LocatePEMBody( const char *pchPEM, uint32 *pcch, const char *pszExpectedType );

	static bool GenerateRandomBlock( void *pubDest, int cubDest );

	static bool GenerateSalt( Salt_t *pSalt );
	static bool GenerateSHA1Digest( const uint8 *pubInput, const int cubInput, SHADigest_t *pOutDigest );
	static bool GenerateSHA256Digest( const uint8 *pubData, const int cubData, SHA256Digest_t *pOutputDigest );
	static bool GenerateSaltedSHA1Digest( const char *pchInput, const Salt_t *pSalt, SHADigest_t *pOutDigest );
	static bool GenerateStrongScryptDigest( const char *pchInput, int32 nInputLen, const uint8 *pubSalt, uint32 cubSalt, SHA256Digest_t *pDigestOut );
	static bool GenerateFastScryptDigest( const char *pchInput, int32 nInputLen, const uint8 *pubSalt, uint32 cubSalt, SHA256Digest_t *pDigestOut );

	// GenerateHMAC256 is our implementation of HMAC-SHA256. Relatively future-proof. You should probably use this unless you have a very good reason not to.
	static bool GenerateHMAC256( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHA256Digest_t *pOutputDigest );

	// GenerateHMAC is our implementation of HMAC-SHA1. The current standard, although people are moving to HMAC-SHA256.
	static bool GenerateHMAC( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHADigest_t *pOutputDigest );

	// GenerateHMACMD5 is HMAC-MD5, which is considered "outdated" although it's quite fast and there are no known or theorized attacks.
	static bool GenerateHMACMD5( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, MD5Digest_t *pOutputDigest );

//SDR_PUBLIC 	static bool BGeneratePasswordHash( const char *pchInput, EPasswordHashAlg hashType, const Salt_t &Salt, PasswordHash_t &OutPasswordHash );
//SDR_PUBLIC 	static bool BValidatePasswordHash( const char *pchInput, EPasswordHashAlg hashType, const PasswordHash_t &DigestStored, const Salt_t &Salt, PasswordHash_t *pDigestComputed );
//SDR_PUBLIC 	static bool BGeneratePBKDF2Hash( const char *pchInput, const Salt_t &Salt, unsigned int rounds, PasswordHash_t &OutPasswordHash );
//SDR_PUBLIC 	static bool BGenerateWrappedSHA1PasswordHash( const char *pchInput, const Salt_t &Salt, unsigned int rounds, PasswordHash_t &OutPasswordHash );
//SDR_PUBLIC 	static bool BUpgradeOrWrapPasswordHash( PasswordHash_t &InPasswordHash, EPasswordHashAlg hashTypeIn, const Salt_t &Salt, PasswordHash_t &OutPasswordHash, EPasswordHashAlg &hashTypeOut );


#ifdef DBGFLAG_VALIDATE
	static void ValidateStatics( CValidator &validator, const char *pchName );
#endif
};

//
// Inline a bunch of functions that consume RSA keys.
//
inline bool CCrypto::RSAEncrypt( const uint8 *pubPlaintextPlaintextData, const uint32 cubData, uint8 *pubEncryptedData, 
	uint32 *pcubEncryptedData, const CRSAPublicKey &rsaKey )
{
	return RSAEncrypt( pubPlaintextPlaintextData, cubData, pubEncryptedData, pcubEncryptedData, rsaKey.GetData(), rsaKey.GetLength() );
}

inline bool CCrypto::RSADecrypt( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
	uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const CRSAPrivateKey &rsaKey )
{
	return RSADecrypt( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, rsaKey.GetData(), rsaKey.GetLength(), false );
}

inline bool CCrypto::RSADecryptPKCSv15( const uint8 *pubEncryptedData, uint32 cubEncryptedData,
	uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const CRSAPrivateKey &rsaKey )
{
	return RSADecrypt( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, rsaKey.GetData(), rsaKey.GetLength(), true );
}

inline bool CCrypto::RSAPublicDecrypt_NoPadding_DEPRECATED( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
	uint8 *pubPlaintextData, uint32 *pcubPlaintextData, const CRSAPrivateKey &rsaKey )
{
	return RSAPublicDecrypt_NoPadding_DEPRECATED( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, rsaKey.GetData(), rsaKey.GetLength() );
}

inline bool CCrypto::RSASign( const uint8 *pubData, const uint32 cubData, 
	uint8 *pubSignature, uint32 *pcubSignature, 
	const CRSAPrivateKey &rsaKey )
{
	return RSASign( pubData, cubData, pubSignature, pcubSignature, rsaKey.GetData(), rsaKey.GetLength() );
}

inline bool CCrypto::RSAVerifySignature( const uint8 *pubData, const uint32 cubData, 
	const uint8 *pubSignature, const uint32 cubSignature,
	const CRSAPublicKey &rsaKey )
{
	return RSAVerifySignature( pubData, cubData, pubSignature, cubSignature, rsaKey.GetData(), rsaKey.GetLength() );
}

inline bool CCrypto::RSASignSHA256( const uint8 *pubData, const uint32 cubData, 
	uint8 *pubSignature, uint32 *pcubSignature, 
	const CRSAPrivateKey &rsaKey )
{
	return RSASignSHA256( pubData, cubData, pubSignature, pcubSignature, rsaKey.GetData(), rsaKey.GetLength() );
}

inline bool CCrypto::RSAVerifySignatureSHA256(const uint8 *pubData, const uint32 cubData, 
	const uint8 *pubSignature, const uint32 cubSignature, 
	const CRSAPublicKey &rsaKey )
{
	return RSAVerifySignatureSHA256( pubData, cubData, pubSignature, cubSignature, rsaKey.GetData(), rsaKey.GetLength() );
}



//-----------------------------------------------------------------------------
// Purpose: power-of-two radix conversion with translation table. Can be used
//			for hex, octal, binary, base32, base64, etc. This is a class because
//			the decoding is done via a generated reverse-lookup table.
//-----------------------------------------------------------------------------
class CCustomPow2RadixEncoder
{
public:
	explicit CCustomPow2RadixEncoder( const char *pchEncodingTable );
	bool EncodeBits( const uint8 *pubData, size_t cDataBits, char *pchEncodedData, size_t cchEncodedData ) const;
	bool Encode( const uint8 *pubData, size_t cubData, char *pchEncodedData, size_t cchEncodedData ) const;
	bool Decode( const char *pchData, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidChars = true, bool bDropTrailingPartialByte = true ) const;
	
protected:
	void Init( const uint8 *pubEncodingTable, uint32 cubEncodingTable );
	size_t EncodeImpl( const uint8 *pubData, size_t cubData, uint cIgnoredTrailingBits, uint8 *pchEncodedData, size_t cchEncodedData, bool bAddTrailingNull ) const;
	size_t DecodeImpl( const uint8 *pubData, size_t cubData, uint8 *pchDecodedData, size_t cchDecodedData, bool bIgnoreInvalidChars, bool bDropTrailingPartialByte ) const;

	static size_t TranslateBits( const uint8 *pSrc, size_t cubSrc, uint cIgnoredTrailingBits, uint uLookupBitWidth, const uint8 *pLookupTable, uint uOutputPackBitWidth, uint8 *pDest, size_t cubDest, bool bIgnoreInvalidChars, bool bDropTrailingPartialByte );

	uint m_uRadixBits;
	uint m_chTerminator;
	uint8 m_rgubEncodingTable[256];
	uint8 m_rgubDecodingTable[256];
};

class CCustomHexEncoder : public CCustomPow2RadixEncoder
{
public:
	explicit CCustomHexEncoder( const char *pchEncodeTable );
};

class CCustomBase32Encoder : public CCustomPow2RadixEncoder
{
public:
	explicit CCustomBase32Encoder( const char *pchEncodeTable );
};

#endif // CRYPTO_H
