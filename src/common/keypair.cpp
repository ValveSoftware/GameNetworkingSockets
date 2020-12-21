//========= Copyright Valve LLC, All rights reserved. ========================

// Note: not using precompiled headers! This file is included directly by
// several different projects and may include Crypto++ headers depending
// on compile-time defines, which in turn pulls in other odd dependencies

#include "keypair.h"
#include "crypto.h"
#include "crypto_25519.h"
#include <tier1/utlbuffer.h>

static const char k_szOpenSSHPrivatKeyPEMHeader[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
static const char k_szOpenSSHPrivatKeyPEMFooter[] = "-----END OPENSSH PRIVATE KEY-----";

static bool BCheckAndEatBytes( CUtlBuffer &buf, const void *data, int sz )
{
	if ( buf.GetBytesRemaining() < sz )
		return false;
	if ( V_memcmp( buf.PeekGet(), data, sz ) != 0 )
		return false;
	buf.SeekGet( CUtlBuffer::SEEK_CURRENT, sz );
	return true;
}

static bool BOpenSSHGetUInt32( CUtlBuffer &buf, uint32 &result )
{
	uint32 temp;
	if ( !buf.Get( &temp, 4 ) )
		return false;
	result = BigDWord( temp );
	return true;
}

static void OpenSSHWriteUInt32( CUtlBuffer &buf, uint32 data )
{
	data = BigDWord( data );
	buf.Put( &data, sizeof(data) );
}

static const uint32 k_nBinarySSHEd25519KeyTypeIDLen = 15;

static bool BOpenSSHBinaryEd25519CheckAndEatKeyType( CUtlBuffer &buf )
{
	return BCheckAndEatBytes( buf, "\x00\x00\x00\x0bssh-ed25519", k_nBinarySSHEd25519KeyTypeIDLen );
}

static void OpenSSHBinaryEd25519WriteKeyType( CUtlBuffer &buf )
{
	buf.Put( "\x00\x00\x00\x0bssh-ed25519", k_nBinarySSHEd25519KeyTypeIDLen );
}

static bool BOpenSSHBinaryReadFixedSizeKey( CUtlBuffer &buf, void *pOut, uint32 cbExpectedSize )
{
	uint32 cbSize;
	if ( !BOpenSSHGetUInt32( buf, cbSize ) )
		return false;
	if ( cbSize != cbExpectedSize || buf.GetBytesRemaining() < (int)cbExpectedSize )
		return false;
	V_memcpy( pOut, buf.PeekGet(), cbExpectedSize );
	buf.SeekGet( CUtlBuffer::SEEK_CURRENT, cbExpectedSize );
	return true;
}

static void OpenSSHBinaryWriteFixedSizeKey( CUtlBuffer &buf, const void *pData, uint32 cbSize )
{
	OpenSSHWriteUInt32( buf, cbSize );
	buf.Put( pData, cbSize );
}

static bool BParseOpenSSHBinaryEd25519Private( CUtlBuffer &buf, uint8 *pPrivateThenPublicKey )
{

	// OpenSSH source sshkey.c, sshkey_private_to_blob2():

	//	if ((r = sshbuf_put(encoded, AUTH_MAGIC, sizeof(AUTH_MAGIC))) != 0 ||
	//	    (r = sshbuf_put_cstring(encoded, ciphername)) != 0 ||
	//	    (r = sshbuf_put_cstring(encoded, kdfname)) != 0 ||
	//	    (r = sshbuf_put_stringb(encoded, kdf)) != 0 ||
	//	    (r = sshbuf_put_u32(encoded, 1)) != 0 ||	/* number of keys */
	//	    (r = sshkey_to_blob(prv, &pubkeyblob, &pubkeylen)) != 0 ||
	//	    (r = sshbuf_put_string(encoded, pubkeyblob, pubkeylen)) != 0)
	//		goto out;

	if ( !BCheckAndEatBytes( buf, "openssh-key-v1", 15 ) )
		return false;

	// Encrypted keys not supported
	if ( !BCheckAndEatBytes( buf, "\x00\x00\x00\x04none\x00\x00\x00\x04none\x00\x00\x00\x00", 20 ) )
	{
		AssertMsg( false, "Tried to use encrypted OpenSSH private key" );
		return false;
	}

	// File should only contain a single key
	if ( !BCheckAndEatBytes( buf, "\x00\x00\x00\x01", 4 ) )
		return false;

	// Public key.  It's actually stored in the file 3 times.
	uint8 arbPubKey1[ 32 ];
	{
		// Size of public key
		uint32 cbEncodedPubKey;
		if ( !BOpenSSHGetUInt32( buf, cbEncodedPubKey ) )
			return false;
		if ( buf.GetBytesRemaining() < (int)cbEncodedPubKey )
			return false;

		// Parse public key
		CUtlBuffer bufPubKey( buf.PeekGet(), cbEncodedPubKey, CUtlBuffer::READ_ONLY );
		bufPubKey.SeekPut( CUtlBuffer::SEEK_HEAD, cbEncodedPubKey );
		buf.SeekGet( CUtlBuffer::SEEK_CURRENT, cbEncodedPubKey );

		if ( !BOpenSSHBinaryEd25519CheckAndEatKeyType( bufPubKey ) )
			return false;
		if ( !BOpenSSHBinaryReadFixedSizeKey( bufPubKey, arbPubKey1, sizeof(arbPubKey1) ) )
			return false;
	}

	// Private key
	{
		uint32 cbEncodedPrivKey;
		if ( !BOpenSSHGetUInt32( buf, cbEncodedPrivKey ) )
			return false;
		if ( buf.GetBytesRemaining() < (int)cbEncodedPrivKey ) // This should actually be the last thing, but if there's extra stuff, I guess we don't care.
			return false;

		// OpenSSH source sshkey.c, to_blob_buf()

		CUtlBuffer bufPrivKey( buf.PeekGet(), cbEncodedPrivKey, CUtlBuffer::READ_ONLY );
		bufPrivKey.SeekPut( CUtlBuffer::SEEK_HEAD, cbEncodedPrivKey );

		// Consume check bytes (used for encrypted keys)

		//	/* Random check bytes */
		//	check = arc4random();
		//	if ((r = sshbuf_put_u32(encrypted, check)) != 0 ||
		//	    (r = sshbuf_put_u32(encrypted, check)) != 0)
		//		goto out;

		uint32 check1, check2;
		if ( !BOpenSSHGetUInt32( bufPrivKey, check1 ) || !BOpenSSHGetUInt32( bufPrivKey, check2 ) || check1 != check2 )
			return false;

		// Key type
		if ( !BOpenSSHBinaryEd25519CheckAndEatKeyType( bufPrivKey ) )
			return false;

		// Public key...again.  One would think that having this large,
		// known know plaintext (TWICE!) is not wise if the key is encrypted
		// with a password....but oh well.
		uint8 arbPubKey2[ 32 ];
		if ( !BOpenSSHBinaryReadFixedSizeKey( bufPrivKey, arbPubKey2, sizeof(arbPubKey2) ) )
			return false;
		if ( V_memcmp( arbPubKey1, arbPubKey2, sizeof(arbPubKey1) ) != 0 )
			return false;

		// And now the entire secret key
		if ( !BOpenSSHBinaryReadFixedSizeKey( bufPrivKey, pPrivateThenPublicKey, 64 ) )
			return false;

		// The "secret" actually consists of the real secret key
		// followed by the public key.  Check that this third
		// copy of the public key matches the other two.
		if ( V_memcmp( arbPubKey1, pPrivateThenPublicKey+32, sizeof(arbPubKey1) ) != 0 )
			return false;

		// Comment and padding comes after this, but we don't care
	}

	return true;
}

static int OpenSSHBinaryBeginSubBlock( CUtlBuffer &buf )
{
	int nSaveTell = buf.TellPut();
	buf.SeekPut( CUtlBuffer::SEEK_CURRENT, sizeof(uint32) );
	return nSaveTell;
}

static void OpenSSHBinaryEndSubBlock( CUtlBuffer &buf, int nSaveTell )
{
	int nBytesWritten = buf.TellPut() - nSaveTell - sizeof(uint32);
	Assert( nBytesWritten >= 0 );
	*(uint32 *)( (uint8 *)buf.Base() + nSaveTell ) = BigDWord( uint32(nBytesWritten) );
}

static void OpenSSHBinaryWriteEd25519Private( CUtlBuffer &buf, const uint8 *pPrivKey, const uint8 *pPubKey )
{
	// Make sure we don't realloc, so that if we wipe afterwards we don't
	// leave key material lying around.
	buf.EnsureCapacity( 2048 );

	buf.Put( "openssh-key-v1", 15 );
	buf.Put( "\x00\x00\x00\x04none\x00\x00\x00\x04none\x00\x00\x00\x00", 20 );
	buf.Put( "\x00\x00\x00\x01", 4 );

	// Public key.  It's actually stored in the file 3 times.
	{
		// Size of public key
		int nSaveTell = OpenSSHBinaryBeginSubBlock( buf );
		OpenSSHBinaryEd25519WriteKeyType( buf );
		OpenSSHBinaryWriteFixedSizeKey( buf, pPubKey, 32 );
		OpenSSHBinaryEndSubBlock( buf, nSaveTell );
	}

	// Private key
	{
		int nSaveTell = OpenSSHBinaryBeginSubBlock( buf );

		// Check bytes.  Since we aren't encrypting, it's not useful for
		// these to be random.
		OpenSSHWriteUInt32( buf, 0x12345678 );
		OpenSSHWriteUInt32( buf, 0x12345678 );

		// Key type
		OpenSSHBinaryEd25519WriteKeyType( buf );

		// Public key...again.
		OpenSSHBinaryWriteFixedSizeKey( buf, pPubKey, 32 );

		// And now the entire "secret" key.  But this is actually
		// the private key followed by the public
		OpenSSHWriteUInt32( buf, 64 );
		buf.Put( pPrivKey, 32 );
		buf.Put( pPubKey, 32 );

		// Comment and padding comes after this.
		// Should we write it anything?

		OpenSSHBinaryEndSubBlock( buf, nSaveTell );
	}
}

static bool BParseOpenSSHBinaryEd25519Public( CUtlBuffer &buf, uint8 *pKey )
{
	if ( !BOpenSSHBinaryEd25519CheckAndEatKeyType( buf ) )
		return false;

	if ( !BOpenSSHBinaryReadFixedSizeKey( buf, pKey, 32 ) )
		return false;

	// If there's extra stuff, we don't care
	return true;
}

static void OpenSSHBinaryEd25519WritePublic( CUtlBuffer &buf, const uint8 *pKey )
{
	buf.EnsureCapacity( 128 );
	OpenSSHBinaryEd25519WriteKeyType( buf );
	OpenSSHBinaryWriteFixedSizeKey( buf, pKey, 32 );
}

static bool GetBinaryDataAsPEM( char *pchPEMData, uint32_t cubPEMData, uint32_t *pcubPEMData, const void *pBinaryData, uint32 cbBinaryData, const char *pchPrefix, const char *pchSuffix )
{
	uint32_t uRequiredBytes = V_strlen( pchPrefix ) + 2 + V_strlen( pchSuffix ) + 2 + CCrypto::Base64EncodeMaxOutput( cbBinaryData, "\r\n" );
	if ( pcubPEMData )
		*pcubPEMData = uRequiredBytes;

	if ( pchPEMData )
	{
		if ( cubPEMData < uRequiredBytes )
			return false;

		V_strncpy( pchPEMData, pchPrefix, cubPEMData );
		V_strncat( pchPEMData, "\r\n", cubPEMData );
		uint32_t uRemainingBytes = cubPEMData - V_strlen( pchPEMData );
	
		if ( !CCrypto::Base64Encode( pBinaryData, cbBinaryData, pchPEMData + V_strlen( pchPEMData ), &uRemainingBytes, "\r\n" ) )
			return false;

		V_strncat( pchPEMData, pchSuffix, cubPEMData );
		V_strncat( pchPEMData, "\r\n", cubPEMData );
		if ( pcubPEMData )
			*pcubPEMData = V_strlen( pchPEMData ) + 1;
	}

	return true;
}

CCryptoKeyBase::~CCryptoKeyBase() {}

bool CCryptoKeyBase::GetRawDataAsStdString( std::string *pString ) const
{
	pString->clear();
	uint32 cbSize = GetRawData(nullptr);
	if ( cbSize == 0 )
		return false;
	void *tmp = alloca( cbSize );
	if ( GetRawData( tmp ) != cbSize )
	{
		Assert( false );
		return false;
	}
	pString->assign( (const char *)tmp, cbSize );
	SecureZeroMemory( tmp, cbSize );
	return true;
}

bool CCryptoKeyBase::SetRawDataAndWipeInput( void *pData, size_t cbData )
{
	bool bResult = SetRawDataWithoutWipingInput( pData, cbData );
	SecureZeroMemory( pData, cbData );
	return bResult;
}

bool CCryptoKeyBase::SetRawDataWithoutWipingInput( const void *pData, size_t cbData )
{
	Wipe();
	return SetRawData( pData, cbData ); // Call type-specific function
}

bool CCryptoKeyBase::SetFromHexEncodedString( const char *pchEncodedKey )
{
	Wipe();

	uint32 cubKey = V_strlen( pchEncodedKey ) / 2 + 1;
	void *buf = alloca( cubKey );

	if ( !CCrypto::HexDecode( pchEncodedKey, buf, &cubKey ) )
	{
		SecureZeroMemory( buf, cubKey );
		return false;
	}

	return SetRawDataAndWipeInput( buf, cubKey );
}


//-----------------------------------------------------------------------------
// Purpose: Initialize a key object from a base-64 encoded string of the raw key bytes
// Input:	pchEncodedKey -		Pointer to the base-64 encoded key string
// Output:  true if successful, false if initialization failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::SetFromBase64EncodedString( const char *pchEncodedKey )
{
	Wipe();
	uint32 cubKey = V_strlen( pchEncodedKey ) * 3 / 4 + 1;

	void *buf = alloca( cubKey );
	if ( !CCrypto::Base64Decode( pchEncodedKey, buf, &cubKey ) )
	{
		SecureZeroMemory( buf, cubKey );
		return false;
	}

	return SetRawDataAndWipeInput( buf, cubKey );
}

//-----------------------------------------------------------------------------
// Purpose: Compare two keys for equality
// Output:  true if the keys are identical
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::operator==( const CCryptoKeyBase &rhs ) const
{
	if ( m_eKeyType != rhs.m_eKeyType ) return false;
	uint32 cbRawData = GetRawData(nullptr);
	if ( cbRawData != rhs.GetRawData(nullptr) ) return false;

	CAutoWipeBuffer bufLHS( cbRawData );
	CAutoWipeBuffer bufRHS( cbRawData );
	DbgVerify( GetRawData( bufLHS.Base() ) == cbRawData );
	DbgVerify( rhs.GetRawData( bufRHS.Base() ) == cbRawData );

	return memcmp( bufLHS.Base(), bufRHS.Base(), cbRawData ) == 0;
}

void CCryptoKeyBase::CopyFrom( const CCryptoKeyBase &x )
{
	Assert( m_eKeyType == x.m_eKeyType );
	Wipe();

	uint32 cbData = x.GetRawData( nullptr );
	if ( cbData == 0 )
		return;
	void *tmp = alloca( cbData );
	VerifyFatal( x.GetRawData( tmp ) == cbData );
	VerifyFatal( SetRawDataAndWipeInput( tmp, cbData ) );
}

bool CCryptoKeyBase::LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes )
{
	AssertMsg1( false, "Key type %d doesn't know how to load from buffer", m_eKeyType );
	Wipe();
	SecureZeroMemory( pBuffer, cBytes );
	return false;
}

//-----------------------------------------------------------------------------
// CCryptoKeyBase_RawBuffer
//-----------------------------------------------------------------------------

CCryptoKeyBase_RawBuffer::~CCryptoKeyBase_RawBuffer()
{
	Wipe();
}

bool CCryptoKeyBase_RawBuffer::IsValid() const
{
	return m_pData != nullptr && m_cbData > 0;
}

uint32 CCryptoKeyBase_RawBuffer::GetRawData( void *pData ) const
{
	if ( pData )
		memcpy( pData, m_pData, m_cbData );
	return m_cbData;
}

bool CCryptoKeyBase_RawBuffer::SetRawData( const void *pData, size_t cbData )
{
	Wipe();
	m_pData = (uint8*)malloc( cbData );
	if ( !m_pData )
		return false;
	memcpy( m_pData, pData, cbData );
	m_cbData = (uint32)cbData;
	return true;
}

void CCryptoKeyBase_RawBuffer::Wipe()
{
	if ( m_pData )
	{
		free( m_pData );
		m_pData = nullptr;
	}
	m_cbData = 0;
}

//-----------------------------------------------------------------------------
// CEC25519PrivateKeyBase
//-----------------------------------------------------------------------------

CEC25519PrivateKeyBase::~CEC25519PrivateKeyBase()
{
	Wipe();
}

void CEC25519PrivateKeyBase::Wipe()
{
	CEC25519KeyBase::Wipe();

	// A public key is not sensitive, by definition, but let's zero it anyway
	SecureZeroMemory( m_publicKey, sizeof(m_publicKey) );
}

bool CEC25519PrivateKeyBase::GetPublicKey( CEC25519PublicKeyBase *pPublicKey ) const
{
	pPublicKey->Wipe();

	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		Assert( pPublicKey->GetKeyType() == k_ECryptoKeyTypeKeyExchangePublic );
		if ( pPublicKey->GetKeyType() != k_ECryptoKeyTypeKeyExchangePublic )
			return false;
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		Assert( pPublicKey->GetKeyType() == k_ECryptoKeyTypeSigningPublic );
		if ( pPublicKey->GetKeyType() != k_ECryptoKeyTypeSigningPublic )
			return false;
	}
	else
	{
		Assert( false ); // impossible, we must be one or the other if valid
		return false;
	}

	return pPublicKey->SetRawDataWithoutWipingInput( m_publicKey, 32 );
}

bool CEC25519PrivateKeyBase::MatchesPublicKey( const CEC25519PublicKeyBase &publicKey ) const
{
	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate && publicKey.GetKeyType() != k_ECryptoKeyTypeKeyExchangePublic )
		return false;
	if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate && publicKey.GetKeyType() != k_ECryptoKeyTypeSigningPublic )
		return false;
	if ( !IsValid() || !publicKey.IsValid() )
		return false;

	uint8 pubKey2[32];
	DbgVerify( publicKey.GetRawData( pubKey2 ) == 32 );

	return memcmp( GetPublicKeyRawData(), pubKey2, 32 ) == 0;
}

bool CEC25519PrivateKeyBase::SetRawData( const void *pData, size_t cbData )
{
	if ( !CEC25519KeyBase::SetRawData( pData, cbData ) )
		return false;
	if ( CachePublicKey() )
		return true;
	Wipe();
	return false;
}

CEC25519PublicKeyBase::~CEC25519PublicKeyBase() {}
CECKeyExchangePrivateKey::~CECKeyExchangePrivateKey() {}
CECKeyExchangePublicKey::~CECKeyExchangePublicKey() {}

//-----------------------------------------------------------------------------
// CECSigningPrivateKey
//-----------------------------------------------------------------------------

bool CECSigningPrivateKey::GetAsPEM( char *pchPEMData, uint32_t cubPEMData, uint32_t *pcubPEMData ) const
{
	if ( !IsValid() )
		return false;
	uint8 privateKey[ 32 ];
	VerifyFatal( GetRawData( privateKey ) == 32 );

	CAutoWipeBuffer bufTemp;
	OpenSSHBinaryWriteEd25519Private( bufTemp, privateKey, GetPublicKeyRawData() );
	SecureZeroMemory( privateKey, sizeof(privateKey) );

	return GetBinaryDataAsPEM( pchPEMData, cubPEMData, pcubPEMData, bufTemp.Base(), bufTemp.TellPut(), k_szOpenSSHPrivatKeyPEMHeader, k_szOpenSSHPrivatKeyPEMFooter ); 
}

bool CECSigningPrivateKey::LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes )
{
	bool bResult = ParsePEM( (const char *)pBuffer, cBytes );
	SecureZeroMemory( pBuffer, cBytes );
	return bResult;
}

bool CECSigningPrivateKey::ParsePEM( const char *pBuffer, size_t cBytes )
{
	Wipe();

	CAutoWipeBuffer buf;
	if ( !CCrypto::DecodePEMBody( pBuffer, (uint32)cBytes, buf, "OPENSSH PRIVATE KEY" ) )
		return false;
	uint8 privateThenPublic[64];
	if ( !BParseOpenSSHBinaryEd25519Private( buf, privateThenPublic ) )
		return false;

	if ( !SetRawDataAndWipeInput( privateThenPublic, 32 ) )
		return false;

	// Check that the public key matches the private one.
	// (And also that all of our code works.)
	if ( V_memcmp( m_publicKey, privateThenPublic+32, 32 ) == 0 )
		return true;

	AssertMsg( false, "Ed25519 key public doesn't match private!" );
	Wipe();
	return false;
}

//-----------------------------------------------------------------------------
// CECSigningPublicKey
//-----------------------------------------------------------------------------

bool CECSigningPublicKey::GetAsOpenSSHAuthorizedKeys( char *pchData, uint32 cubData, uint32 *pcubData, const char *pszComment ) const
{
	if ( !IsValid() )
		return false;

	int cchComment = pszComment ? V_strlen( pszComment ) : 0;

	uint8 publicKey[ 32 ];
	VerifyFatal( GetRawData( publicKey ) == 32 );

	CUtlBuffer bufBinary;
	OpenSSHBinaryEd25519WritePublic( bufBinary, publicKey );

	static const char pchPrefix[] = "ssh-ed25519 ";

	uint32_t uRequiredBytes =
		V_strlen( pchPrefix )
		+ CCrypto::Base64EncodeMaxOutput( bufBinary.TellPut(), "" )
		+ ( cchComment > 0 ? 1 : 0 ) // space
		+ cchComment
		+ 1; // '\0'
	if ( pcubData )
		*pcubData = uRequiredBytes;

	if ( pchData )
	{
		if ( cubData < uRequiredBytes )
			return false;

		V_strncpy( pchData, pchPrefix, cubData );
		uint32_t uRemainingBytes = cubData - V_strlen( pchData );
	
		if ( !CCrypto::Base64Encode( (const uint8 *)bufBinary.Base(), bufBinary.TellPut(), pchData + V_strlen( pchData ), &uRemainingBytes, "" ) )
			return false;

		if ( pszComment )
		{
			V_strncat( pchData, " ", cubData );
			V_strncat( pchData, pszComment, cubData );
		}
		if ( pcubData )
			*pcubData = V_strlen( pchData ) + 1;
	}

	return true;
}

bool CECSigningPublicKey::LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes )
{
	bool bResult = SetFromOpenSSHAuthorizedKeys( (const char *)pBuffer, cBytes );
	SecureZeroMemory( pBuffer, cBytes );
	return bResult;
}

bool CECSigningPublicKey::SetFromOpenSSHAuthorizedKeys( const char *pchData, size_t cbData )
{
	Wipe();

	// Gah, we need to make a copy to '\0'-terminate it, to make parsing below easier
	CAutoWipeBuffer bufText( int( cbData + 8 ) );
	bufText.Put( pchData, int(cbData) );
	bufText.PutChar(0);
	pchData = (const char *)bufText.Base();

	int idxStart = -1, idxEnd = -1;
	sscanf( pchData, "ssh-ed25519 %nAAAA%*s%n", &idxStart, &idxEnd );
	if ( idxStart <= 0 || idxEnd <= idxStart )
		return false;

	CAutoWipeBuffer bufBinary;
	if ( !CCrypto::DecodeBase64ToBuf( pchData + idxStart, idxEnd-idxStart, bufBinary ) )
		return false;

	uint8 pubKey[ 32 ];
	if ( !BParseOpenSSHBinaryEd25519Public( bufBinary, pubKey ) )
		return false;
	return SetRawDataAndWipeInput( pubKey, 32 );
 }

void CCrypto::GenerateKeyExchangeKeyPair( CECKeyExchangePublicKey *pPublicKey, CECKeyExchangePrivateKey *pPrivateKey )
{
	uint8 rgubSecretData[32];
	GenerateRandomBlock( rgubSecretData, 32 );
	VerifyFatal( pPrivateKey->SetRawDataAndWipeInput( rgubSecretData, 32 ) );
	if ( pPublicKey )
	{
		VerifyFatal( pPrivateKey->GetPublicKey( pPublicKey ) );
	}
}

void CCrypto::GenerateSigningKeyPair( CECSigningPublicKey *pPublicKey, CECSigningPrivateKey *pPrivateKey )
{
	uint8 rgubSecretData[32];
	GenerateRandomBlock( rgubSecretData, 32 );
	VerifyFatal( pPrivateKey->SetRawDataAndWipeInput( rgubSecretData, 32 ) );
	if ( pPublicKey )
	{
		VerifyFatal( pPrivateKey->GetPublicKey( pPublicKey ) );
	}
}
