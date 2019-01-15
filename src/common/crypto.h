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

namespace CCrypto
{
	enum ECDSACurve {
		k_ECDSACurve_secp256k1,
		k_ECDSACurve_secp256r1,
	};

	void Init();
	
	// SymmetricEncryptWithIV is NOT compatible with SymmetricDecrypt, because it does not write
	// the IV into the data stream - it is assumed that the IV is communicated or agreed upon by
	// some other out-of-band method. Pair it with SymmetricDecryptWithIV to decrpyt. Output is
	// always 1-16 bytes longer than input due to PKCS#7 block padding.
	bool SymmetricEncryptWithIV( const uint8 * pubPlaintextData, uint32 cubPlaintextData,
										const uint8 * pIV, uint32 cubIV,
										uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
										const uint8 * pubKey, uint32 cubKey );

	bool SymmetricDecryptRecoverIV( const uint8 * pubEncryptedData, uint32 cubEncryptedData,
		uint8 * pubPlaintextData, uint32 * pcubPlaintextData, uint8 *pIV, uint32 cubIV,
		const uint8 * pubKey, uint32 cubKey, bool bVerifyPaddingBytes = true );

	
	// SymmetricDecryptWithIV assumes that the encrypted data does not begin with an IV.
	// If you created the encrypted data with SymmetricEncryptChosenIV, you must discard
	// the first 16 bytes of encrypted output before passing it to SymmetricDecryptWithIV.
	bool SymmetricDecryptWithIV( const uint8 * pubEncryptedData, uint32 cubEncryptedData, 
										const uint8 * pIV, uint32 cubIV,
										uint8 * pubPlaintextData, uint32 * pcubPlaintextData,
										const uint8 * pubKey, uint32 cubKey, bool bVerifyPaddingBytes = true );
	
	//
	// Secure key exchange (curve25519 elliptic-curve Diffie-Hellman key exchange)
	//
	void GenerateKeyExchangeKeyPair( CECKeyExchangePublicKey *pPublicKey, CECKeyExchangePrivateKey *pPrivateKey );
	void PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut );

	//
	// Signing and verification (ed25519 elliptic-curve signature scheme)
	//
	void GenerateSigningKeyPair( CECSigningPublicKey *pPublicKey, CECSigningPrivateKey *pPrivateKey );
	void GenerateSignature( const uint8 *pubData, uint32 cubData, const CECSigningPrivateKey &privateKey, CryptoSignature_t *pSignatureOut );
	bool VerifySignature( const uint8 *pubData, uint32 cubData, const CECSigningPublicKey &publicKey, const CryptoSignature_t &signature );

	bool HexEncode( const uint8 *pubData, const uint32 cubData, char *pchEncodedData, uint32 cchEncodedData );
	bool HexDecode( const char *pchData, uint8 *pubDecodedData, uint32 *pcubDecodedData );

	uint32 Base64EncodeMaxOutput( uint32 cubData, const char *pszLineBreakOrNull );
	bool Base64Encode( const uint8 *pubData, uint32 cubData, char *pchEncodedData, uint32 cchEncodedData, bool bInsertLineBreaks = true ); // legacy, deprecated
	bool Base64Encode( const uint8 *pubData, uint32 cubData, char *pchEncodedData, uint32 *pcchEncodedData, const char *pszLineBreak = "\n" );

	inline uint32 Base64DecodeMaxOutput( uint32 cubData ) { return ( (cubData + 3 ) / 4) * 3 + 1; }
	bool Base64Decode( const char *pchEncodedData, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true ); // legacy, deprecated
	bool Base64Decode( const char *pchEncodedData, uint32 cchEncodedData, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters = true );

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
