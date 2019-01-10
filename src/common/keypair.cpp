//========= Copyright Valve LLC, All rights reserved. ========================

// Note: not using precompiled headers! This file is included directly by
// several different projects and may include Crypto++ headers depending
// on compile-time defines, which in turn pulls in other odd dependencies

#include "keypair.h"
#include "crypto.h"
#include <tier1/utlbuffer.h>

/// CUtlBuffer that will wipe upon destruction
//
/// WARNING: This is only intended for simple use cases where the caller
/// can easily pre-allocate.  For example, it won't wipe if the buffer needs
/// to be relocated as a result of realloc.  Or if you pas it to a function
/// via a CUtlBuffer&, and CUtlBuffer::Purge is invoked directly.  Etc.
class CAutoWipeBuffer : public CUtlBuffer
{
public:
	CAutoWipeBuffer() {}
	explicit CAutoWipeBuffer( int cbInit ) : CUtlBuffer( 0, cbInit, 0 ) {}
	~CAutoWipeBuffer() { Purge(); }

	void Clear()
	{
		SecureZeroMemory( Base(), SizeAllocated() );
		CUtlBuffer::Clear();
	}

	void Purge()
	{
		Clear();
		CUtlBuffer::Purge();
	}
};

static const char k_szOpenSSHPrivatKeyPEMHeader[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
static const char k_szOpenSSHPrivatKeyPEMFooter[] = "-----END OPENSSH PRIVATE KEY-----";

static const uint k_nRSAOAEPOverheadBytes = 42; // fixed-size overhead of OAEP padding scheme

static bool DecodeBase64ToBuf( const char *pszEncoded, uint32 cbEncoded, CAutoWipeBuffer &buf )
{
	uint32 cubDecodeSize = cbEncoded * 3 / 4 + 1;
	buf.EnsureCapacity( cubDecodeSize );
	if ( !CCrypto::Base64Decode( pszEncoded, cbEncoded, (uint8*)buf.Base(), &cubDecodeSize ) )
		return false;
	buf.SeekPut( CUtlBuffer::SEEK_HEAD, cubDecodeSize );
	return true;
}

static bool DecodePEMBody( const char *pszPem, uint32 cch, CAutoWipeBuffer &buf )
{
	const char *pszBody = CCrypto::LocatePEMBody( pszPem, &cch, nullptr );
	if ( !pszBody )
		return false;
	return DecodeBase64ToBuf( pszBody, cch, buf );
}

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

static bool BParseOpenSSHBinaryEd25519Private( CUtlBuffer &buf, uint8 *pKey )
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
		if ( !BOpenSSHBinaryReadFixedSizeKey( bufPrivKey, pKey, 64 ) )
			return false;

		// The "secret" actually consists of the real secret key
		// followed by the public key.  Check that this third
		// copy of the public key matches the other two.
		if ( V_memcmp( arbPubKey1, pKey+32, sizeof(arbPubKey1) ) != 0 )
			return false;

		// OpenSSH stores the key as private first, then public.
		// We want it the other way around
		for ( int i = 0 ; i < 32 ; ++i )
			std::swap( pKey[i], pKey[i+32] );

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

static void OpenSSHBinaryWriteEd25519Private( CUtlBuffer &buf, const uint8 *pKey )
{
	// Make sure we don't realloc, so that if we wipe afterwards we don't
	// leave key material lying around.
	buf.EnsureCapacity( 2048 );

	// We store the key as public first, then private
	const uint8 *pPubKey = pKey;
	const uint8 *pPrivKey = pKey+32;

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

//-----------------------------------------------------------------------------
// Purpose: Initialize an RSA key object from a hex encoded string
// Input:	pchEncodedKey -		Pointer to the hex encoded key string
// Output:  true if successful, false if initialization failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::SetFromHexEncodedString( const char *pchEncodedKey )
{
	uint32 cubKey = V_strlen( pchEncodedKey ) / 2 + 1;
	EnsureCapacity( cubKey );

	bool bSuccess = CCrypto::HexDecode( pchEncodedKey, m_pbKey, &cubKey );
	if ( bSuccess )
	{
		m_cubKey = cubKey;
	}
	else
	{
		Wipe();
	}
	return bSuccess;
}


//-----------------------------------------------------------------------------
// Purpose: Initialize an RSA key object from a base-64 encoded string
// Input:	pchEncodedKey -		Pointer to the base-64 encoded key string
// Output:  true if successful, false if initialization failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::SetFromBase64EncodedString( const char *pchEncodedKey )
{
	uint32 cubKey = V_strlen( pchEncodedKey ) * 3 / 4 + 1;
	EnsureCapacity( cubKey );

	bool bSuccess = CCrypto::Base64Decode( pchEncodedKey, m_pbKey, &cubKey );
	if ( bSuccess )
	{
		m_cubKey = cubKey;
	}
	else
	{
		Wipe();
	}
	return bSuccess;
}


//-----------------------------------------------------------------------------
// Purpose: Initialize an RSA key object from a buffer
// Input:	pData -				Pointer to the buffer
//			cbData -			Length of the buffer
// Output:  true if successful, false if initialization failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::Set( const void* pData, const uint32 cbData )
{
	if ( pData == m_pbKey )
		return true;
	Wipe();
	EnsureCapacity( cbData );
	V_memcpy( m_pbKey, pData, cbData );
	m_cubKey = cbData;
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Serialize an RSA key object to a buffer, in PEM format
// Input:	pchPEMData -		Pointer to string buffer to store output in (or NULL to just calculate required size)
//			cubPEMData -		Size of pchPEMData buffer
//			pcubPEMData -		Pointer to number of bytes written to pchPEMData (including terminating nul), or
//								required size of pchPEMData if it is NULL or not big enough.
// Output:  true if successful, false if it failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::GetAsPEM( char *pchPEMData, uint32_t cubPEMData, uint32_t *pcubPEMData ) const
{
	CAutoWipeBuffer bufTemp;

	if ( !IsValid() )
		return false;

	const char *pchPrefix = "", *pchSuffix = "";
	uint32 cubBinary = m_cubKey;
	const uint8 *pbBinary = m_pbKey;
	if ( m_eKeyType == k_ECryptoKeyTypeRSAPublic )
	{
		pchPrefix = "-----BEGIN PUBLIC KEY-----";
		pchSuffix = "-----END PUBLIC KEY-----";
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeRSAPrivate )
	{
		pchPrefix = "-----BEGIN PRIVATE KEY-----";
		pchSuffix = "-----END PRIVATE KEY-----";
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		pchPrefix = k_szOpenSSHPrivatKeyPEMHeader;
		pchSuffix = k_szOpenSSHPrivatKeyPEMFooter;

		OpenSSHBinaryWriteEd25519Private( bufTemp, m_pbKey );
		cubBinary = bufTemp.TellPut();
		pbBinary = (const uint8 *)bufTemp.Base();
	}
	else
	{
		Assert( false ); // nonsensical to call this on non-RSA keys
		return false;
	}
	
	uint32_t uRequiredBytes = V_strlen( pchPrefix ) + 2 + V_strlen( pchSuffix ) + 2 + CCrypto::Base64EncodeMaxOutput( cubBinary, "\r\n" );
	if ( pcubPEMData )
		*pcubPEMData = uRequiredBytes;

	if ( pchPEMData )
	{
		if ( cubPEMData < uRequiredBytes )
			return false;

		V_strncpy( pchPEMData, pchPrefix, cubPEMData );
		V_strncat( pchPEMData, "\r\n", cubPEMData );
		uint32_t uRemainingBytes = cubPEMData - V_strlen( pchPEMData );
	
		if ( !CCrypto::Base64Encode( pbBinary, cubBinary, pchPEMData + V_strlen( pchPEMData ), &uRemainingBytes, "\r\n" ) )
			return false;

		V_strncat( pchPEMData, pchSuffix, cubPEMData );
		V_strncat( pchPEMData, "\r\n", cubPEMData );
		if ( pcubPEMData )
			*pcubPEMData = V_strlen( pchPEMData ) + 1;
	}

	return true;
}

bool CECSigningPublicKey::GetAsOpenSSHAuthorizedKeys( char *pchData, uint32 cubData, uint32 *pcubData, const char *pszComment ) const
{
	if ( !IsValid() )
		return false;

	int cchComment = pszComment ? V_strlen( pszComment ) : 0;

	CUtlBuffer bufBinary;
	OpenSSHBinaryEd25519WritePublic( bufBinary, m_pbKey );

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


//-----------------------------------------------------------------------------
// Purpose: Compare two keys for equality
// Output:  true if the keys are identical
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::operator==( const CCryptoKeyBase &rhs ) const
{
	return ( m_eKeyType == rhs.m_eKeyType ) && ( m_cubKey == rhs.m_cubKey ) && ( V_memcmp( m_pbKey, rhs.m_pbKey, m_cubKey ) == 0 );
}


//-----------------------------------------------------------------------------
// Purpose: Make sure there's enough space in the allocated key buffer
//			for the designated data size.
//-----------------------------------------------------------------------------
void CCryptoKeyBase::EnsureCapacity( uint32 cbData )
{
	Wipe();
	m_pbKey = new uint8[cbData];

	//
	// Note: Intentionally not setting m_cubKey here - it's the size
	// of the key not the size of the allocation.
	//
}

//-----------------------------------------------------------------------------
// Purpose: Load key from file (best-guess at PKCS#8 PEM, Base64, hex, or binary)
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes )
{
	Wipe();

	Assert( cBytes < 1024*1024*10 ); // sanity check: 10 MB key? no thanks
	if ( cBytes > 0 && cBytes < 1024*1024*10 )
	{

		// Ensure null termination
		char *pchBase = (char*) malloc( cBytes + 1 );
		V_memcpy( pchBase, pBuffer, cBytes );
		pchBase[cBytes] = '\0';

		if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
		{
			// Fixed key size
			EnsureCapacity( 64 );

			CAutoWipeBuffer buf;
			if (
				V_strstr( pchBase, k_szOpenSSHPrivatKeyPEMHeader )
				&& DecodePEMBody( pchBase, (uint32)cBytes, buf )
				&& BParseOpenSSHBinaryEd25519Private( buf, m_pbKey )
			) {
				m_cubKey = 64;

				// Check that the public key matches the private one.
				// (And also that all of our code works.)
				uint8 arCheckPublic[ 32 ];
				V_memcpy( arCheckPublic, m_pbKey, 32 );
				(( CEC25519PrivateKeyBase * )this )->RebuildFromPrivateData( m_pbKey+32 );
				if ( V_memcmp( arCheckPublic, m_pbKey, 32 ) != 0 )
				{
					AssertMsg( false, "Ed25519 key public doesn't match private!" );
					Wipe();
				}
			}
			else
			{
				Wipe();
			}
		}
		else if ( m_eKeyType == k_ECryptoKeyTypeSigningPublic )
		{
			// Fixed key size
			EnsureCapacity( 32 );

			// OpenSSH authorized_keys format?

			CAutoWipeBuffer buf;
			int idxStart = -1, idxEnd = -1;
			sscanf( pchBase, "ssh-ed25519 %nAAAA%*s%n", &idxStart, &idxEnd );
			if (
				idxStart > 0
				&& idxEnd > idxStart
				&& DecodeBase64ToBuf( pchBase + idxStart, idxEnd-idxStart, buf )
				&& BParseOpenSSHBinaryEd25519Public( buf, m_pbKey )
			)
			{
				// OK
				m_cubKey = 32;
			}
			else
			{
				Wipe();
			}
		}
		else if ( m_eKeyType != k_ECryptoKeyTypeRSAPublic && m_eKeyType != k_ECryptoKeyTypeRSAPrivate )
		{
			// TODO?
		}
		else
		{
			// strip PEM header if we find it
			const char *pchPEMPrefix = ( m_eKeyType == k_ECryptoKeyTypeRSAPrivate ) ? "-----BEGIN PRIVATE KEY-----" : "-----BEGIN PUBLIC KEY-----";
			if ( const char *pchData = V_strstr( pchBase, pchPEMPrefix ) )
			{
				SetFromBase64EncodedString( V_strstr( pchData, "KEY-----" ) + 8 );
			}
			else if ( pchBase[0] == 'M' && pchBase[1] == 'I' )
			{
				SetFromBase64EncodedString( pchBase );
			}
			else if ( pchBase[0] == 0x30 && (uint8)pchBase[1] == 0x82 )
			{
				Set( (const uint8*)pchBase, (uint32)cBytes );
			}
			else if ( pchBase[0] == '3' && pchBase[1] == '0' && pchBase[2] == '8' && pchBase[3] == '2' )
			{
				SetFromHexEncodedString( pchBase );
			}
		}

		SecureZeroMemory( pchBase, cBytes );
		free( pchBase );
	}

	// Wipe input buffer
	SecureZeroMemory( pBuffer, cBytes );
	return IsValid();
}

//-----------------------------------------------------------------------------
// Purpose: Retrieve the public half of our internal (public,private) pair
//-----------------------------------------------------------------------------
void CEC25519PrivateKeyBase::GetPublicKey( CCryptoPublicKeyBase *pPublicKey ) const
{
	pPublicKey->Wipe();
	Assert( IsValid() );
	if ( !IsValid() )
		return;
	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		Assert( pPublicKey->GetKeyType() == k_ECryptoKeyTypeKeyExchangePublic );
		if ( pPublicKey->GetKeyType() != k_ECryptoKeyTypeKeyExchangePublic )
			return;
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		Assert( pPublicKey->GetKeyType() == k_ECryptoKeyTypeSigningPublic );
		if ( pPublicKey->GetKeyType() != k_ECryptoKeyTypeSigningPublic )
			return;
	}
	else
	{
		Assert( false ); // impossible, we must be one or the other if valid
		return;
	}
	pPublicKey->Set( GetData(), 32 );
}


//-----------------------------------------------------------------------------
// Purpose: Verify that a set of public and private curve25519 keys are matched
//-----------------------------------------------------------------------------
bool CEC25519PrivateKeyBase::MatchesPublicKey( const CCryptoPublicKeyBase &publicKey ) const
{
	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate && publicKey.GetKeyType() != k_ECryptoKeyTypeKeyExchangePublic )
		return false;
	if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate && publicKey.GetKeyType() != k_ECryptoKeyTypeSigningPublic )
		return false;
	if ( !IsValid() || !publicKey.IsValid() || publicKey.GetLength() != 32 )
		return false;
	return memcmp( GetData(), publicKey.GetData(), 32 ) == 0;
}
