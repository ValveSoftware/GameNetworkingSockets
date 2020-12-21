//========= Copyright Valve Corporation, All rights reserved. =========================

#ifndef KEYPAIR_H
#define KEYPAIR_H
#pragma once

#include <steam/steamuniverse.h>
#include <tier0/platform.h>
#include <tier0/memdbgoff.h>
#include <string>
#include <tier0/memdbgon.h>

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

	ECryptoKeyType GetKeyType() const { return m_eKeyType; }

	// Return true if we're valid
	virtual bool IsValid() const = 0;

	// Free up memory and wipe any sensitive data
	virtual void Wipe() = 0;

	// Get raw data.  Returns number of bytes populated into the buffer.
	// If you pass NULL, the number of bytes required is returned.
	virtual uint32 GetRawData( void *pData ) const = 0;

	// Set raw data.  Returns true on success.  Regardless of the outcome,
	// your buffer will be wiped.
	bool SetRawDataAndWipeInput( void *pData, size_t cbData );

	// Don't wipe the input.  Use this when you know your key is not valuable,
	// or are going to wipe it yourself
	bool SetRawDataWithoutWipingInput( const void *pData, size_t cbData );

	// Initialize a key object from a hex encoded string of the raw key bytes
	bool SetFromHexEncodedString( const char *pchEncodedKey );
	bool SetFromBase64EncodedString( const char *pchEncodedKey );

	// Get raw data as a std::string
	bool GetRawDataAsStdString( std::string *pResult ) const;

	// Set raw data from a std::string.  (Useful for dealing with protobuf)
	// NOTE: DOES NOT WIPE THE INPUT
	bool SetRawDataFromStdString( const std::string &s ) { return SetRawDataWithoutWipingInput( s.c_str(), s.length() ); }

	// Load from some sort of formatted buffer.  (Not the raw binary key data.)
	virtual bool LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes );

	// Compare keys for equality, by comparing their raw data
	bool operator==( const CCryptoKeyBase &rhs ) const;
	bool operator!=( const CCryptoKeyBase &rhs ) const { return !operator==( rhs ); }

	// Make a copy of the key, by using the raw data functions
	void CopyFrom( const CCryptoKeyBase &x );

#ifdef DBGFLAG_VALIDATE
	virtual void Validate( CValidator &validator, const char *pchName ) const = 0;		// Validate our internal structures
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

#ifdef DBGFLAG_VALIDATE
	virtual void Validate( CValidator &validator, const char *pchName ) const;		// Validate our internal structures
#endif // DBGFLAG_VALIDATE

protected:
	virtual bool SetRawData( const void *pData, size_t cbData ) override;
	inline CCryptoKeyBase_RawBuffer( ECryptoKeyType keyType ) : CCryptoKeyBase( keyType ), m_pData( nullptr ), m_cbData( 0 ) {}

	uint8 *m_pData;
	uint32 m_cbData;
};

// Forward declare specific key types.
class CRSAKeyBase;
class CRSAPublicKey;
class CRSAPrivateKey;
class CEC25519KeyBase;
class CEC25519PublicKeyBase;
class CEC25519PrivateKeyBase;
class CECKeyExchangePrivateKey;
class CECKeyExchangePublicKey;
class CECSigningPrivateKey;
class CECSigningPublicKey;

#endif // KEYPAIR_H

