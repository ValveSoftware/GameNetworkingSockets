//========= Copyright Valve LLC, All rights reserved. =========================

#ifndef CRYPTO_25519_H
#define CRYPTO_25519_H

#include "crypto_constants.h"
#include "keypair.h"

class CEC25519KeyBase : public CCryptoKeyBase_RawBuffer
{
public:
	virtual ~CEC25519KeyBase();
	virtual bool IsValid() const override;
	virtual uint32 GetRawData( void *pData ) const override;
	virtual void Wipe() override;

	void *evp_pkey() const { return m_evp_pkey; }
protected:
	virtual bool SetRawData( const void *pData, size_t cbData ) override;
	inline CEC25519KeyBase( ECryptoKeyType keyType ) : CCryptoKeyBase_RawBuffer( keyType ), m_evp_pkey(nullptr) {}

	// Actually EVP_PKEY*, but we don't want to include OpenSSL headers here,
	// especially since we might not actually be using OpenSSL for this at all!
	void *m_evp_pkey;
};

//-----------------------------------------------------------------------------
// Purpose: Common base for x25519 and ed25519 public keys on the 25519 curve
//			The raw data is 32 bytes
//-----------------------------------------------------------------------------
class CEC25519PublicKeyBase : public CEC25519KeyBase
{
public:
	virtual ~CEC25519PublicKeyBase();
protected:
	CEC25519PublicKeyBase( ECryptoKeyType eType ) : CEC25519KeyBase( eType ) { }
};

//-----------------------------------------------------------------------------
// Purpose: Common base for x25519 and ed25519 private keys on the 25519 curve
//			The raw data is 32 bytes.
//          NOTE: An old version also stored the public key in the raw data.
//                We don't do that anymore.)  If you want that, get the public
//                key data specifically
//-----------------------------------------------------------------------------
class CEC25519PrivateKeyBase : public CEC25519KeyBase
{
public:
	virtual ~CEC25519PrivateKeyBase();
	virtual void Wipe() override;
	bool GetPublicKey( CEC25519PublicKeyBase *pPublicKey ) const;
	bool MatchesPublicKey( const CEC25519PublicKeyBase &pPublicKey ) const;

	const uint8 *GetPublicKeyRawData() const { return m_publicKey; }

protected:
	CEC25519PrivateKeyBase( ECryptoKeyType eType ) : CEC25519KeyBase( eType ) { }

	// We keep a copy of the public key cached.
	// It is not considered part of the raw key data,
	// as was previously the case.)
	uint8 m_publicKey[32];

	bool CachePublicKey();
	virtual bool SetRawData( const void *pData, size_t cbData ) override;
};


//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve signature private key (x25519)
//-----------------------------------------------------------------------------
class CECKeyExchangePrivateKey : public CEC25519PrivateKeyBase
{
public:
	CECKeyExchangePrivateKey() : CEC25519PrivateKeyBase( k_ECryptoKeyTypeKeyExchangePrivate ) { }
	virtual ~CECKeyExchangePrivateKey();
};


//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve key-exchange public key (curve25519)
//			Internally, this is stored as a 32-byte binary data blob
//-----------------------------------------------------------------------------
class CECKeyExchangePublicKey : public CEC25519PublicKeyBase
{
public:
	CECKeyExchangePublicKey() : CEC25519PublicKeyBase( k_ECryptoKeyTypeKeyExchangePublic ) { }

	// Allow copying of public keys without a bunch of paranoia.
	CECKeyExchangePublicKey( const CECKeyExchangePublicKey &x ) : CEC25519PublicKeyBase( k_ECryptoKeyTypeKeyExchangePublic ) { SetRawDataWithoutWipingInput( x.GetRawDataPtr(), x.GetRawDataSize() ); }
	CECKeyExchangePublicKey & operator=(const CECKeyExchangePublicKey &x) { if ( this != &x ) SetRawDataWithoutWipingInput( x.GetRawDataPtr(), x.GetRawDataSize() ); return *this; }

	virtual ~CECKeyExchangePublicKey();
};


//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve signature private key (ed25519)
//			Internally, this is stored as a 64-byte (public, private) pair
//-----------------------------------------------------------------------------
class CECSigningPrivateKey : public CEC25519PrivateKeyBase
{
public:
	CECSigningPrivateKey() : CEC25519PrivateKeyBase( k_ECryptoKeyTypeSigningPrivate ) { }

	// Load from PEM
	virtual bool LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes ) override;

	// Purpose: Get key in PEM text format
	// Input:	pchPEMData -		Pointer to string buffer to store output in (or NULL to just calculate required size)
	//			cubPEMData -		Size of pchPEMData buffer
	//			pcubPEMData -		Pointer to number of bytes written to pchPEMData (including terminating nul), or
	//								required size of pchPEMData if it is NULL or not big enough.
	bool GetAsPEM( char *pchPEMData, uint32 cubPEMData, uint32 *pcubPEMData ) const;

	// Parses OpenSSH PEM block.
	// WARNING: DOES NOT WIPE INPUT.
	bool ParsePEM( const char *pBuffer, size_t cBytes );

	// Generate an ed25519 public-key signature
	void GenerateSignature( const void *pData, size_t cbData, CryptoSignature_t *pSignatureOut ) const;
};

//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve signature public key (x25519)
//			Internally, this is stored as a 32-byte binary data blob
//-----------------------------------------------------------------------------
class CECSigningPublicKey : public CEC25519PublicKeyBase
{
public:
	CECSigningPublicKey() : CEC25519PublicKeyBase( k_ECryptoKeyTypeSigningPublic ) { }

	// Allow copying of public keys without a bunch of paranoia.
	CECSigningPublicKey( const CECSigningPublicKey &x ) : CEC25519PublicKeyBase( k_ECryptoKeyTypeSigningPublic ) { SetRawDataWithoutWipingInput( x.GetRawDataPtr(), x.GetRawDataSize() ); }
	CECSigningPublicKey & operator=(const CECSigningPublicKey &x) { if ( this != &x ) SetRawDataWithoutWipingInput( x.GetRawDataPtr(), x.GetRawDataSize() ); return *this; }

	virtual bool LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes ) override;

	bool GetAsOpenSSHAuthorizedKeys( char *pchData, uint32 cubData, uint32 *pcubData, const char *pszComment = "" ) const;
	bool SetFromOpenSSHAuthorizedKeys( const char *pchData, size_t cbData );

	bool VerifySignature( const void *pData, size_t cbData, const CryptoSignature_t &signature ) const;
};

#ifdef VALVE_CRYPTO_ENABLE_25519

namespace CCrypto
{

	//
	// Secure key exchange (curve25519 elliptic-curve Diffie-Hellman key exchange)
	//

	// Generate a X25519 key pair for Diffie-Hellman secure key exchange.
	// pPublicKey can be null.  (Since the private key also has a copy of the public key.)
	void GenerateKeyExchangeKeyPair( CECKeyExchangePublicKey *pPublicKey, CECKeyExchangePrivateKey *pPrivateKey );

	// Do Diffie-Hellman secure key exchange.
	// NOTE: this actually returns the SHA256 of the raw DH result.  I don't know why.
	bool PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut );

	//
	// Signing and verification (ed25519 elliptic-curve signature scheme)
	//

	// Generate an ed25519 key pair for public-key signature generation
	void GenerateSigningKeyPair( CECSigningPublicKey *pPublicKey, CECSigningPrivateKey *pPrivateKey );

	// Legacy compatibility - use the key methods
	inline void GenerateSignature( const void *pData, size_t cbData, const CECSigningPrivateKey &privateKey, CryptoSignature_t *pSignatureOut ) { privateKey.GenerateSignature( pData, cbData, pSignatureOut ); }
	inline bool VerifySignature( const void *pData, size_t cbData, const CECSigningPublicKey &publicKey, const CryptoSignature_t &signature ) { return publicKey.VerifySignature( pData, cbData, signature ); }
};

#endif // #ifdef VALVE_CRYPTO_ENABLE_25519

#endif // CRYPTO_H
