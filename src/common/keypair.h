//========= Copyright Valve Corporation, All rights reserved. =========================

#ifndef KEYPAIR_H
#define KEYPAIR_H
#pragma once

#include "steam/steamuniverse.h"

#include <stdint.h>
#include "minbase/minbase_securezeromemory_impl.h"


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
	~CCryptoKeyBase() { Wipe(); }

	bool operator==( const CCryptoKeyBase &rhs ) const;
	bool operator!=( const CCryptoKeyBase &rhs ) const { return !operator==( rhs ); }

	ECryptoKeyType GetKeyType() const { return m_eKeyType; }
	const uint8 *GetData() const { return m_pbKey; }
	uint32 GetLength() const { return m_cubKey; }
	bool IsValid() const;

	bool Set( const void* pData, const uint32 cbData );

	void Wipe();

	bool SetFromHexEncodedString( const char *pchEncodedKey );
	bool SetFromBase64EncodedString( const char *pchEncodedKey );
	void SetFromExternal( uint8 *pbKey, uint32 cubKey ) { m_pbKey = pbKey; m_cubKey = cubKey; m_bExternal = true; }

	bool LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes );

	bool GetAsPEM( char *pchPEMData, uint32 cubPEMData, uint32 *pcubPEMData ) const;

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName ) const;		// Validate our internal structures
#endif // DBGFLAG_VALIDATE

protected:
	explicit CCryptoKeyBase( ECryptoKeyType keyType ) : m_eKeyType( keyType ) { m_cubKey = 0; m_pbKey = NULL; m_bExternal = false; }
	
	void EnsureCapacity( uint32 cbData );

	ECryptoKeyType m_eKeyType;
	uint8	*m_pbKey;
	uint32	m_cubKey;
	bool m_bExternal;

private:
	CCryptoKeyBase( const CCryptoKeyBase &src ); // no impl
	CCryptoKeyBase & operator=(const CCryptoKeyBase &rhs); // no impl
};


//-----------------------------------------------------------------------------
// Purpose: Check validity of an crypto key object
// Output:  true if successful, false the object is invalid
//-----------------------------------------------------------------------------
inline bool CCryptoKeyBase::IsValid() const
{
	return m_eKeyType != k_ECryptoKeyTypeInvalid && GetLength() > 0 && GetData() != NULL;
}


//-----------------------------------------------------------------------------
// Purpose: Clear the memory for an crypto key object
//-----------------------------------------------------------------------------
inline void CCryptoKeyBase::Wipe()
{
	if ( m_pbKey != NULL )
	{
		SecureZeroMemory( m_pbKey, m_cubKey );
		if ( !m_bExternal )
			delete [] m_pbKey;
		m_pbKey = NULL;
	}
	m_cubKey = 0;
}


//-----------------------------------------------------------------------------
// Purpose: Define operator= and accessible copy constructor for public keys
//-----------------------------------------------------------------------------
class CCryptoPublicKeyBase : public CCryptoKeyBase
{
protected:
	explicit CCryptoPublicKeyBase( ECryptoKeyType eKeyType ) : CCryptoKeyBase( eKeyType ) { }
	CCryptoPublicKeyBase( const CCryptoPublicKeyBase &src ) : CCryptoKeyBase( src.m_eKeyType ) { Set( src.GetData(), src.GetLength() ); }
	CCryptoPublicKeyBase& operator=( const CCryptoPublicKeyBase &rhs ) { Wipe(); Set( rhs.GetData(), rhs.GetLength() ); return *this; }
};


//-----------------------------------------------------------------------------
// Purpose: Common base for x25519 and ed25519 private keys on the 25519 curve
//			Internally, these are stored as 64 bytes (public[32] + private[32])
//-----------------------------------------------------------------------------
class CEC25519PrivateKeyBase : public CCryptoKeyBase
{
public:
	bool IsValid() const { return ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate || m_eKeyType == k_ECryptoKeyTypeSigningPrivate ) && m_cubKey == 64 && m_pbKey; }
	void GetPublicKey( CCryptoPublicKeyBase *pPublicKey ) const;
	bool MatchesPublicKey( const CCryptoPublicKeyBase &pPublicKey ) const;

	void RebuildFromPrivateData( const uint8 privateKeyData[32] );

protected:
	explicit CEC25519PrivateKeyBase( ECryptoKeyType eType ) : CCryptoKeyBase( eType ) { }
};


//-----------------------------------------------------------------------------
// Purpose: Encapsulates an RSA public key
//-----------------------------------------------------------------------------
class CRSAPublicKey: public CCryptoPublicKeyBase
{
public:
	CRSAPublicKey() : CCryptoPublicKeyBase( k_ECryptoKeyTypeRSAPublic ) { }
	uint32 GetModulusBytes( uint8 *pBufferOut, uint32 cbMaxBufferOut ) const;
	uint32 GetExponentBytes( uint8 *pBufferOut, uint32 cbMaxBufferOut ) const;
};

//-----------------------------------------------------------------------------
// Purpose: Encapsulates an RSA private key
//-----------------------------------------------------------------------------
class CRSAPrivateKey: public CCryptoKeyBase
{
public:
	CRSAPrivateKey() : CCryptoKeyBase( k_ECryptoKeyTypeRSAPrivate ) { }

	void GetPublicKey( CRSAPublicKey *pPublicKey ) const;

private:
	CRSAPrivateKey( const CRSAPrivateKey &src ); // no impl
	CRSAPrivateKey & operator=(const CRSAPrivateKey &rhs); // no impl
};

//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve signature private key (x25519)
//			Internally, this is stored as a 64-byte (public, private) pair
//-----------------------------------------------------------------------------
class CECKeyExchangePrivateKey : public CEC25519PrivateKeyBase
{
public:
	CECKeyExchangePrivateKey() : CEC25519PrivateKeyBase( k_ECryptoKeyTypeKeyExchangePrivate ) { }
	bool IsValid() const { return m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate && m_cubKey == 64 && m_pbKey; }

private:
	CECKeyExchangePrivateKey( const CECKeyExchangePrivateKey &src ); // no impl
	CECKeyExchangePrivateKey & operator=(const CECKeyExchangePrivateKey &rhs); // no impl
};


//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve key-exchange public key (curve25519)
//			Internally, this is stored as a 32-byte binary data blob
//-----------------------------------------------------------------------------
class CECKeyExchangePublicKey : public CCryptoPublicKeyBase
{
public:
	CECKeyExchangePublicKey() : CCryptoPublicKeyBase( k_ECryptoKeyTypeKeyExchangePublic ) { }
	bool IsValid() const { return m_eKeyType == k_ECryptoKeyTypeKeyExchangePublic && m_cubKey == 32 && m_pbKey; }
};


//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve signature private key (ed25519)
//			Internally, this is stored as a 64-byte (public, private) pair
//-----------------------------------------------------------------------------
class CECSigningPrivateKey : public CEC25519PrivateKeyBase
{
public:
	CECSigningPrivateKey() : CEC25519PrivateKeyBase( k_ECryptoKeyTypeSigningPrivate ) { }
	bool IsValid() const { return m_eKeyType == k_ECryptoKeyTypeSigningPrivate && m_cubKey == 64 && m_pbKey; }

private:
	CECSigningPrivateKey( const CECSigningPrivateKey &src ); // no impl
	CECSigningPrivateKey & operator=(const CECSigningPrivateKey &rhs); // no impl
};


//-----------------------------------------------------------------------------
// Purpose: Encapsulates an elliptic-curve signature public key (x25519)
//			Internally, this is stored as a 32-byte binary data blob
//-----------------------------------------------------------------------------
class CECSigningPublicKey : public CCryptoPublicKeyBase
{
public:
	CECSigningPublicKey() : CCryptoPublicKeyBase( k_ECryptoKeyTypeSigningPublic ) { }
	bool IsValid() const { return m_eKeyType == k_ECryptoKeyTypeSigningPublic && m_cubKey == 32 && m_pbKey; }

	bool GetAsOpenSSHAuthorizedKeys( char *pchData, uint32 cubData, uint32 *pcubData, const char *pszComment = "" ) const;
};


// callback interface to implement to use encryption or authentication
class INetFilterKeyCallback
{
public:
	virtual const CRSAPublicKey *GetPublicKey( EUniverse eUniverse, const char *pchKeyName ) = 0;
	virtual const CRSAPrivateKey *GetPrivateKey( EUniverse eUniverse, const char *pchKeyName ) = 0;
	virtual EUniverse GetUniverse() = 0;
};

#endif // KEYPAIR_H

