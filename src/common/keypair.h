//========= Copyright Valve Corporation, All rights reserved. =========================

#ifndef KEYPAIR_H
#define KEYPAIR_H
#pragma once

#include <steam/steamuniverse.h>
#include <tier0/platform.h>
#include <string>

#include <stdint.h>
#include "minbase/minbase_securezeromemory_impl.h"

const unsigned int k_cubCryptoSignature = 64;
typedef unsigned char CryptoSignature_t[ k_cubCryptoSignature ];

// Ed25519 / Curve25519 (http://ed25519.cr.yp.to/) are strongly preferred over
// RSA and ECDSA due to performance benefits, minimization of side-channel
// attack vectors, smaller signature length, simpler implementation, and more
// transparent cryptographic analysis with fewer unexplainable magic values.
// Furthermore, unlike RSA and ECDSA, the 25519 algortihms are very hard to
// screw up - there is no dependence on a strong entropy source, and there is
// no such thing as a "weak" or "malformed" key that might compromise security.
enum ECryptoKeyType
{
	k_ECryptoKeyTypeInvalid = 0,
	k_ECryptoKeyTypeRSAPublic = 1,				// RSA 1024, 2048, or higher bit
	k_ECryptoKeyTypeRSAPrivate = 2,				// RSA 1024, 2048, or higher bit
	k_ECryptoKeyTypeSigningPublic = 3,			// ed25519, always 256-bit
	k_ECryptoKeyTypeSigningPrivate = 4,			// ed25519, always 256-bit
	k_ECryptoKeyTypeKeyExchangePublic = 5,		// curve25519, always 256-bit
	k_ECryptoKeyTypeKeyExchangePrivate = 6,		// curve25519, always 256-bit
};


//-----------------------------------------------------------------------------
// Purpose: Base class to encapsulate an crypto key (RSA, EC, ECDSA).  This class
//			cannot be instantiated directly, use one of the subclasses
//			to indicate the intent of the key.
//-----------------------------------------------------------------------------
class CCryptoKeyBase 
{
public:
	virtual ~CCryptoKeyBase();

	bool operator==( const CCryptoKeyBase &rhs ) const;
	bool operator!=( const CCryptoKeyBase &rhs ) const { return !operator==( rhs ); }

	ECryptoKeyType GetKeyType() const { return m_eKeyType; }

	// Return true if we're valid
	virtual bool IsValid() const = 0;

	// Free up memory and wipe any sensitive data
	virtual void Wipe() = 0;

	// Get raw data.  Returns number of bytes populated into the buffer.
	// If you pass NULL, the number of bytes required is returned.
	virtual uint32 GetRawData( void *pData ) const = 0;

	// Get raw data as a std::string
	bool GetRawDataAsStdString( std::string *pResult ) const;

	// Set raw data.  Returns true on success.  Regardless of the outcome,
	// your buffer will be wiped.
	bool SetRawDataAndWipeInput( void *pData, size_t cbData );

	// Don't wipe the input.  Use this when you know your key is not valuable,
	// or are going to wipe it yourself
	bool SetRawDataWithoutWipingInput( const void *pData, size_t cbData );

	// Initialize a key object from a hex encoded string of the raw key bytes
	bool SetFromHexEncodedString( const char *pchEncodedKey );
	bool SetFromBase64EncodedString( const char *pchEncodedKey );

	// Load from some sort of formatted buffer.  (Not the raw binary key data.)
	virtual bool LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes );

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName ) const;		// Validate our internal structures
#endif // DBGFLAG_VALIDATE

protected:
	virtual bool SetRawData( const void *pData, size_t cbData ) = 0;
	CCryptoKeyBase( ECryptoKeyType keyType ) : m_eKeyType( keyType ) {}

	const ECryptoKeyType m_eKeyType;

private:
	CCryptoKeyBase( const CCryptoKeyBase &src ) = delete;
	CCryptoKeyBase & operator=(const CCryptoKeyBase &rhs) = delete;
};

// Base class for when we might store the key in a buffer,
// instead of handing it off to the crypto provider and
// using their interfaces.
//
// Since we don't have a configure script, the application
// code doesn't have a mechanism to know how this library
// was compiled, so we need a consistent ABI for these
// classes, no matter how it's implemented.
class CCryptoKeyBase_RawBuffer : public CCryptoKeyBase
{
public:
	virtual ~CCryptoKeyBase_RawBuffer();
	virtual bool IsValid() const override;
	virtual uint32 GetRawData( void *pData ) const override;
	virtual void Wipe() override;

	const uint8 *GetRawDataPtr() const { return m_pData; }
	uint32 GetRawDataSize() const { return m_cbData; }

protected:
	virtual bool SetRawData( const void *pData, size_t cbData ) override;
	inline CCryptoKeyBase_RawBuffer( ECryptoKeyType keyType ) : CCryptoKeyBase( keyType ), m_pData( nullptr ), m_cbData( 0 ) {}

	uint8 *m_pData;
	uint32 m_cbData;
};

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

	virtual bool LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes ) override;

	bool GetAsOpenSSHAuthorizedKeys( char *pchData, uint32 cubData, uint32 *pcubData, const char *pszComment = "" ) const;
	bool SetFromOpenSSHAuthorizedKeys( const char *pchData, size_t cbData );

	bool VerifySignature( const void *pData, size_t cbData, const CryptoSignature_t &signature ) const;
};

#endif // KEYPAIR_H

