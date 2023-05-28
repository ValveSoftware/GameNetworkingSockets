#include <assert.h>
#include <string>

#include <tier1/utlbuffer.h>
#include <crypto.h>
#include <crypto_25519.h>

#ifdef LINUX
#include <unistd.h>
#endif

// I copied these tests from the Steam branch.
// A little compatibility glue so I don't have to make any changes to them.
#define CHECK(x) do { bool _check_result; Assert( (_check_result = (x)) != false ); g_failed |= !_check_result; } while(0)
#define CHECK_EQUAL(a,b) do { bool _check_eq_result; Assert( (_check_eq_result = ((a)==(b))) != false ); g_failed |= !_check_eq_result; } while(0)
#define RETURNIFNOT(x) { if ( !(x) ) { AssertMsg( false, #x ); return; } }
#define RETURNFALSEIFNOT(x) { if ( !(x) ) { AssertMsg( false, #x ); return false; } }
const int k_cSmallBuff = 100;					// smallish buffer
const int k_cMedBuff = 1200;
bool g_failed = false;

static unsigned char Q_nibble( char c )
{
        if ( ( c >= '0' ) &&
                 ( c <= '9' ) )
        {
                 return (unsigned char)(c - '0');
        }

        if ( ( c >= 'A' ) &&
                 ( c <= 'F' ) )
        {
                 return (unsigned char)(c - 'A' + 0x0a);
        }

        if ( ( c >= 'a' ) &&
                 ( c <= 'f' ) )
        {
                 return (unsigned char)(c - 'a' + 0x0a);
        }

        // received an invalid character, and no real way to return an error
        // AssertMsg1( false, "Q_nibble invalid hex character '%c' ", c );
        return 0;
}

#define min(x,y) (((x) > (y)) ? (y) : (x))

//-----------------------------------------------------------------------------
static void V_hextobinary( char const *in, int numchars, byte *out, int maxoutputbytes )
{
        int len = V_strlen( in );
        numchars = min( len, numchars );

        // Make sure it's even
        numchars = ( numchars ) & ~0x1;

        memset( out, 0x00, maxoutputbytes );

        byte *p;
        int i;

        p = out;
        for ( i = 0;
                 ( i < numchars ) && ( ( p - out ) < maxoutputbytes );
                 i+=2, p++ )
        {
                *p = ( Q_nibble( in[i] ) << 4 ) | Q_nibble( in[i+1] );
        }
}

//-----------------------------------------------------------------------------
// Purpose: Tests cryptography hex encoding
//-----------------------------------------------------------------------------
void TestCryptoEncoding()
{
	bool bRet;

	// If you change the source data you'll need to change the expected encoding
	// output strings further down!
	uint8 rgubData[] = { 0x14, 0xfe, 0x26, 0x19, 0x54, 0x78, 0x00, 0x35, 0x19, 0xa9, 0x54, 0x4e, 0x99 };
	uint cubData = V_ARRAYSIZE( rgubData );
	char rgchEncodedData[k_cMedBuff];
	uint cchEncodedData = V_ARRAYSIZE( rgchEncodedData );
	uint8 rgubDecodedData[k_cSmallBuff];
	uint cubDecodedData = V_ARRAYSIZE( rgubDecodedData );

	// HEX
	// encode some data
	bRet = CCrypto::HexEncode( rgubData, cubData, rgchEncodedData, cchEncodedData );
	CHECK( bRet );		// must succeed

	// decode the data
	bRet = CCrypto::HexDecode( rgchEncodedData, rgubDecodedData, &cubDecodedData );
	CHECK( bRet );						// must succeed
	CHECK( cubDecodedData == cubData );	// must be of correct size

	bRet = (0 == V_memcmp( rgubData, rgubDecodedData, cubData ));
	CHECK( bRet );				// decoded data must match original

	// test the documented, if questionable, permissiveness of hexdecode. Note that Crypto++
	// documentation claims that the last partial byte should have been parsed as E0, but this
	// has been disproven by testing with 5.6.1 and 5.6.2. this test verifies that there is no
	// change in the behavior of partial strings, if the algorithm should be updated.
	cubDecodedData = V_ARRAYSIZE( rgubDecodedData );
	bRet = CCrypto::HexDecode( "x,F\nF1\t ,2\t~E ", rgubDecodedData, &cubDecodedData );
	CHECK( bRet );						// must succeed
	CHECK( cubDecodedData == 2 && rgubDecodedData[0] == 0xFF && rgubDecodedData[1] == 0x12 );

	// this hilarious string is offered up for laughs to verify that we remain as broken as ever.
	// Crypto++'s documentation claims that it will "correctly" parse this string as FF 12 E0.
	// In reality, it does the dumb/obvious thing and discards the 'x' and ' ' characters and
	// parses the '0's, resulting in 4 bytes. There is no evidence that any version of Crypto++
	// has ever matched the documentation and actually done "smart" prefix skipping.
	cubDecodedData = V_ARRAYSIZE( rgubDecodedData );
	bRet = CCrypto::HexDecode( "0xFF 0x12 0xE", rgubDecodedData, &cubDecodedData );
	CHECK( bRet );
	CHECK( cubDecodedData == 4 && rgubDecodedData[0] == 0x0F && rgubDecodedData[1] == 0xF0 && rgubDecodedData[2] == 0x12 && rgubDecodedData[3] == 0x0E );

	// BASE64
	cchEncodedData = V_ARRAYSIZE( rgchEncodedData );
	bRet = CCrypto::Base64Encode( rgubData, cubData, rgchEncodedData, &cchEncodedData );
	CHECK( bRet );		// must succeed

	// decode the data
	cubDecodedData = V_ARRAYSIZE( rgubDecodedData );
	bRet = CCrypto::Base64Decode( rgchEncodedData, cchEncodedData, rgubDecodedData, &cubDecodedData );
	CHECK( bRet );						// must succeed
	CHECK( cubDecodedData == cubData );	// must be of correct size

	bRet = ( 0 == V_memcmp( rgubData, rgubDecodedData, cubData ) );
	CHECK( bRet );				// decoded data must match original

	// Test empty string handling
	{
		char buf[4] = { 1, 1, 1, 1 };
		uint32 bufSize = sizeof(buf);
		bRet = CCrypto::Base64Encode( NULL, 0, buf, &bufSize, NULL );
		CHECK( bRet );
		CHECK_EQUAL( bufSize, 0 ); // zero characters written
		CHECK_EQUAL( buf[0], 0 ); // terminating null written to buffer
	}
	{
		uint8 buf[4] = { 1, 1, 1, 1 };
		uint32 bufSize = sizeof(buf);
		bRet = CCrypto::Base64Decode( NULL, 0, buf, &bufSize, true );
		CHECK( bRet );
		CHECK_EQUAL( bufSize, 0 );
		CHECK_EQUAL( buf[0], 1 ); // shouldn't have written to buf at all

		bufSize = sizeof(buf);
		bRet = CCrypto::Base64Decode( "", 0, buf, &bufSize, true );
		CHECK( bRet );
		CHECK_EQUAL( bufSize, 0 );
		CHECK_EQUAL( buf[0], 1 ); // shouldn't have written to buf at all
	}

	// Test decoding error
	{
		uint8 buf[4] = { 1, 1, 1, 1 };
		uint32 bufSize = sizeof(buf);
		const char pchEncodedBogus[] = "AAAA!@#$%^&*()_+|<>?:;'[]{}\\/,.";
		bRet = CCrypto::Base64Decode( pchEncodedBogus, V_strlen(pchEncodedBogus), buf, &bufSize, false );
		CHECK( bRet == false );
		// Should have decoded 3 null bytes and then failed
		CHECK_EQUAL( bufSize, 3 );
		CHECK_EQUAL( buf[0], 0 );
		CHECK_EQUAL( buf[1], 0 );
		CHECK_EQUAL( buf[2], 0 );
		CHECK_EQUAL( buf[3], 1 );
	}
}

// https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/CAVP-TESTING-BLOCK-CIPHER-MODES
// https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents/mac/gcmtestvectors.zip
class NISTTestVectorFile
{
	FILE *f = nullptr;
public:
	NISTTestVectorFile( const char *pszFilename )
	{
		f = fopen( pszFilename, "rt" );
		CHECK(f);
	}
	~NISTTestVectorFile() { if ( f ) fclose( f ); }

	bool FindNextTest()
	{
		if ( !f )
			return false;
		char line[2048];
		while ( fgets( line, sizeof(line), f ) )
		{
			if ( V_strnicmp( line, "count", 5 ) == 0 )
				return true;
		}
		return false;
	}

	bool GetBinaryBlob( const char *pszTag, std::string &blob )
	{
		char line[2048];
		RETURNFALSEIFNOT( fgets( line, sizeof(line), f ) );
		int lTag = V_strlen(pszTag);
		RETURNFALSEIFNOT( V_strnicmp( line, pszTag, lTag ) == 0 );
		const char *p = line + lTag;
		while ( isspace(*p) )
			++p;
		CHECK_EQUAL( *p, '=' );
		++p;
		while ( isspace(*p) )
			++p;

		uint8 decodedData[ 1024 ];
		uint32 cbDecodedData = sizeof(decodedData);
		RETURNFALSEIFNOT( CCrypto::HexDecode( p, decodedData, &cbDecodedData ) );
		blob.assign( (char *)decodedData, cbDecodedData );
		return true;
	}
};

void TestSymmetricAuthCrypto_EncryptTestVectorFile( const char *pszFilename )
{
	bool bRet;
	AES_GCM_EncryptContext ctxEnc;
	AES_GCM_DecryptContext ctxDec;
	NISTTestVectorFile file( pszFilename );

	while ( file.FindNextTest() )
	{
		std::string key;
		RETURNIFNOT( file.GetBinaryBlob( "key", key ) );
		std::string iv;
		RETURNIFNOT( file.GetBinaryBlob( "iv", iv ) );
		std::string pt;
		RETURNIFNOT( file.GetBinaryBlob( "pt", pt ) );
		std::string aad;
		RETURNIFNOT( file.GetBinaryBlob( "aad", aad ) );
		std::string ct;
		RETURNIFNOT( file.GetBinaryBlob( "ct", ct ) );
		std::string tag;
		RETURNIFNOT( file.GetBinaryBlob( "tag", tag ) );

		// With the Windows CNG API (BCryptEncrypt/BCryptDecrypt) there are no
		// other tag length or IV length options permitted.
		if ( tag.length() != 16 || iv.length() != 12 )
			continue;

		ctxEnc.Init(
			key.c_str(), key.length(),
			iv.length(),
			tag.length() );

		ctxDec.Init(
			key.c_str(), key.length(),
			iv.length(),
			tag.length() );

		// Encrypt it
		uint8 encrypted[ 2048 ];
		uint32 cbEncrypted = sizeof(encrypted);
		RETURNIFNOT( ct.length() <= sizeof(encrypted) );
		bRet = ctxEnc.Encrypt(
			pt.c_str(), pt.length(),
			iv.c_str(),
			encrypted, &cbEncrypted,
			aad.c_str(), aad.length() );
		CHECK( bRet );

		// Confirm it matches the test vector
		CHECK( cbEncrypted == ct.length() + tag.length() );
		CHECK( memcmp( ct.c_str(), encrypted, ct.length() ) == 0 );
		CHECK( memcmp( tag.c_str(), encrypted+ct.length(), tag.length() ) == 0 );

		// Make sure we can decrypt it successfully
		uint8 decrypted[ 2048 ];
		uint32 cbDecrypted = sizeof(decrypted);
		bRet = ctxDec.Decrypt(
			encrypted, cbEncrypted,
			iv.c_str(),
			decrypted, &cbDecrypted,
			aad.c_str(), aad.length() );
		CHECK( bRet );

		CHECK( cbDecrypted == pt.length() );
		CHECK( memcmp( pt.c_str(), decrypted, cbDecrypted ) == 0 );

		// Flip a random bit in the ciphertext+tag blob
		encrypted[ rand() % cbEncrypted ] ^= ( 1 << (rand() & 7 ) );

		// It should fail to decrypt
		cbDecrypted = sizeof(decrypted);
		bRet = ctxDec.Decrypt(
			encrypted, cbEncrypted,
			iv.c_str(),
			decrypted, &cbDecrypted,
			aad.c_str(), aad.length() );
		CHECK( !bRet );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Test AES-GCM crypto against known vectors
//-----------------------------------------------------------------------------
void TestSymmetricAuthCryptoVectors()
{
	#ifndef TEST_VECTOR_DIR
	#define TEST_VECTOR_DIR "aesgcmtestvectors/"
	#endif

	// Check against known test vectors
	TestSymmetricAuthCrypto_EncryptTestVectorFile( TEST_VECTOR_DIR "gcmEncryptExtIV256.rsp" );
}

//-----------------------------------------------------------------------------
// Purpose: Test elliptic-curve primitives (ed25519 signing, curve25519 key exchange)
//-----------------------------------------------------------------------------
void TestEllipticCrypto()
{
	// test vectors from curve25519 reference impl
	const char rgchAlicePriv[] = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
	const char rgchAlicePub[] = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
	const char rgchBobPriv[] = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
	const char rgchBobPub[] = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
	const char rgchExpectSharedPreHash[] = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

	uint8 buf[64];
	CECKeyExchangePrivateKey alicePriv, bobPriv;
	CECKeyExchangePublicKey alicePub, bobPub;
	alicePriv.SetFromHexEncodedString( rgchAlicePriv );
	alicePub.SetFromHexEncodedString( rgchAlicePub );
	CHECK( alicePriv.MatchesPublicKey( alicePub ) );

	bobPriv.SetFromHexEncodedString( rgchBobPriv );
	bobPub.SetFromHexEncodedString( rgchBobPub );
	CHECK( bobPriv.MatchesPublicKey( bobPub ) );

	V_hextobinary( rgchExpectSharedPreHash, 64, buf, 32 );
	SHA256Digest_t expectedResult;
	CCrypto::GenerateSHA256Digest( buf, 32, &expectedResult );

	SHA256Digest_t aliceSharedSecret = {0};
	SHA256Digest_t bobSharedSecret = {1};
	CHECK( CCrypto::PerformKeyExchange( alicePriv, bobPub, &aliceSharedSecret ) );
	CHECK( CCrypto::PerformKeyExchange( bobPriv, alicePub, &bobSharedSecret ) );

	CHECK( V_memcmp( aliceSharedSecret, bobSharedSecret, sizeof(SHA256Digest_t) ) == 0 );
	CHECK( V_memcmp( expectedResult, aliceSharedSecret, sizeof(SHA256Digest_t) ) == 0 );
	CHECK( V_memcmp( expectedResult, bobSharedSecret, sizeof(SHA256Digest_t) ) == 0 );

	// test key extraction and comparison operations
	CECKeyExchangePublicKey testPubFromPriv;
	alicePriv.GetPublicKey( &testPubFromPriv );
	CHECK( testPubFromPriv == alicePub );
	CHECK( testPubFromPriv != bobPub );
	CHECK( alicePriv.MatchesPublicKey( testPubFromPriv ) );
	CHECK( !bobPriv.MatchesPublicKey( testPubFromPriv ) );

	// test key exchange with random keys
	alicePriv.Wipe();
	alicePub.Wipe();
	bobPriv.Wipe();
	bobPub.Wipe();
	memset( aliceSharedSecret, 0, sizeof( aliceSharedSecret ) );
	memset( bobSharedSecret, 0xFF, sizeof( bobSharedSecret ) );
	CCrypto::GenerateKeyExchangeKeyPair( &alicePub, &alicePriv );
	CCrypto::GenerateKeyExchangeKeyPair( &bobPub, &bobPriv );
	// alice and bob send each other only their public keys.
	CHECK( CCrypto::PerformKeyExchange( alicePriv, bobPub, &aliceSharedSecret ) );
	CHECK( CCrypto::PerformKeyExchange( bobPriv, alicePub, &bobSharedSecret ) );
	// alice and bob should have computed the same shared secret.
	CHECK( V_memcmp( aliceSharedSecret, bobSharedSecret, sizeof( bobSharedSecret ) ) == 0 );


	// test vectors from ed25519 reference impl
	const char rgchSignPriv[] = "b18e1d0045995ec3d010c387ccfeb984d783af8fbb0f40fa7db126d889f6dadd";
	const char rgchSignPub[] = "77f48b59caeda77751ed138b0ec667ff50f8768c25d48309a8f386a2bad187fb";
	const char rgchMessageToSign[] = "916c7d1d268fc0e77c1bef238432573c39be577bbea0998936add2b50a653171"
									 "ce18a542b0b7f96c1691a3be6031522894a8634183eda38798a0c5d5d79fbd01"
									 "dd04a8646d71873b77b221998a81922d8105f892316369d5224c9983372d2313"
									 "c6b1f4556ea26ba49d46e8b561e0fc76633ac9766e68e21fba7edca93c4c7460"
									 "376d7f3ac22ff372c18f613f2ae2e856af40";
	const char rgchExpected[] = "6bd710a368c1249923fc7a1610747403040f0cc30815a00f9ff548a896bbda0b"
							    "4eb2ca19ebcf917f0f34200a9edbad3901b64ab09cc5ef7b9bcc3c40c0ff7509";

	CECSigningPrivateKey signPriv;
	CECSigningPublicKey signPub;
	signPriv.SetFromHexEncodedString( rgchSignPriv );
	signPub.SetFromHexEncodedString( rgchSignPub );
	CHECK( signPriv.MatchesPublicKey( signPub ) );

	uint8 bufMessageToSign[ (sizeof(rgchMessageToSign)-1)/2 ];
	V_hextobinary( rgchMessageToSign, sizeof(rgchMessageToSign)-1, bufMessageToSign, sizeof(bufMessageToSign) );

	uint8 bufExpected[64];
	V_hextobinary( rgchExpected, 128, bufExpected, 64 );

	CryptoSignature_t signature;
	CCrypto::GenerateSignature( bufMessageToSign, sizeof(bufMessageToSign), signPriv, &signature );
	CHECK( V_memcmp( signature, bufExpected, sizeof(CryptoSignature_t) ) == 0 );

	bufMessageToSign[5] ^= 1;
	CHECK( !CCrypto::VerifySignature( bufMessageToSign, sizeof( bufMessageToSign ), signPub, signature ) );
	bufMessageToSign[5] ^= 1;

	signature[20] ^= 1;
	CHECK( !CCrypto::VerifySignature( bufMessageToSign, sizeof( bufMessageToSign ), signPub, signature ) );
	signature[20] ^= 1;

	CHECK( CCrypto::VerifySignature( bufMessageToSign, sizeof( bufMessageToSign ), signPub, signature ) );

	// test signing with random keys
	signPriv.Wipe();
	signPub.Wipe();
	CCrypto::GenerateSigningKeyPair( &signPub, &signPriv );
	CCrypto::GenerateSignature( bufMessageToSign, sizeof( bufMessageToSign ), signPriv, &signature );

	bufMessageToSign[5] ^= 1;
	CHECK( !CCrypto::VerifySignature( bufMessageToSign, sizeof( bufMessageToSign ), signPub, signature ) );
	bufMessageToSign[5] ^= 1;

	signature[20] ^= 1;
	CHECK( !CCrypto::VerifySignature( bufMessageToSign, sizeof( bufMessageToSign ), signPub, signature ) );
	signature[20] ^= 1;

	CHECK( CCrypto::VerifySignature( bufMessageToSign, sizeof( bufMessageToSign ), signPub, signature ) );

	// test public/private key relationships and operators
	CECSigningPublicKey testSignPubFromPriv;
	signPriv.GetPublicKey( &testSignPubFromPriv );
	CHECK( testSignPubFromPriv == signPub );
	CHECK( signPriv.MatchesPublicKey( testSignPubFromPriv ) );

	CCrypto::GenerateSigningKeyPair( &signPub, &signPriv );
	CHECK( testSignPubFromPriv != signPub );
	CHECK( !signPriv.MatchesPublicKey( testSignPubFromPriv ) );
}

//-----------------------------------------------------------------------------
// Purpose: Test parsing and re-writing of keys in OpenSSH formats
//-----------------------------------------------------------------------------
void TestOpenSSHEd25519()
{
	char buf[ 2048 ];

	// Generate some keys, and make sure we can round trip them
	{
		CECSigningPublicKey pubKey;
		CECSigningPrivateKey privKey;
		CCrypto::GenerateSigningKeyPair( &pubKey, &privKey );

		uint32 cbBuf;

		// Get public key as authorized_keys format.
		// Should fail if we tell it the buffer is too small
		CHECK( !pubKey.GetAsOpenSSHAuthorizedKeys( buf, 16, &cbBuf, "" ) );
		CHECK( pubKey.GetAsOpenSSHAuthorizedKeys( buf, sizeof(buf), &cbBuf, "" ) );
		CHECK( (int)cbBuf == V_strlen( buf )+1 );
		CHECK( 75 <= cbBuf && cbBuf <= 85 ); // typical size (assuming no password or key comment).  Not necessarily a bug if this assert fires, but maybe something suspicious

		// Parse it back out, make sure it matches
		CECSigningPublicKey pubKey2;
		CHECK( pubKey2.LoadFromAndWipeBuffer( buf, cbBuf ) );
		CHECK( pubKey2 == pubKey );

		// Get private key in openSSH PEM-ish format.
		// Should fail if we tell it the buffer is too small
		CHECK( !privKey.GetAsPEM( buf, 64, &cbBuf ) );
		CHECK( privKey.GetAsPEM( buf, sizeof(buf), &cbBuf ) );
		CHECK( (int)cbBuf == V_strlen( buf )+1 );
		CHECK( 370 <= cbBuf && cbBuf <= 390 ); // typical size (assuming no password or key comment).  Not necessarily a bug if this assert fires, but maybe something suspicious

		// Parse it back out, make sure it matches
		CECSigningPrivateKey privKey2;
		CHECK( privKey2.LoadFromAndWipeBuffer( buf, cbBuf ) );
		CHECK( privKey2 == privKey );

		CHECK( privKey2.MatchesPublicKey( pubKey2 ) );
	}

	// Parse some known keys
	{
		CUtlBuffer bufPrivKeyPEMA;
		bufPrivKeyPEMA.PutString(
R"PEM(
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW
QyNTUxOQAAACC3vdN6llE0by4d7aFur0nBXdu5hXJb7LLkiC5UCEPFDgAAAJgJaJG1CWiR
tQAAAAtzc2gtZWQyNTUxOQAAACC3vdN6llE0by4d7aFur0nBXdu5hXJb7LLkiC5UCEPFDg
AAAECpUfg4C0BkgsCO+GlFAbcTQZUeFFQcamXzDA1tx7aNWre903qWUTRvLh3toW6vScFd
27mFclvssuSILlQIQ8UOAAAAEmZsZXRjaGVyZEBzcmNkczAwMwECAw==
-----END OPENSSH PRIVATE KEY-----
)PEM" );
		CECSigningPrivateKey privKeyA;
		CHECK( privKeyA.LoadFromAndWipeBuffer( bufPrivKeyPEMA.Base(), bufPrivKeyPEMA.TellPut() ) );

		CUtlBuffer bufPubKeyA;
		bufPubKeyA.PutString( "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAILe903qWUTRvLh3toW6vScFd27mFclvssuSILlQIQ8UO" );
		CECSigningPublicKey pubKeyA;
		CHECK( pubKeyA.LoadFromAndWipeBuffer( bufPubKeyA.Base(), bufPubKeyA.TellPut() ) );

		CHECK( privKeyA.MatchesPublicKey( pubKeyA ) );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Tests elliptic crypto perf
//-----------------------------------------------------------------------------
void TestEllipticPerf()
{
	const int k_cubPktBig = 1024*1024*10;
	const int k_cubPktSmall = 128;

	const int k_cIterationsECDH = 500;
	const int k_cIterationsSignSmall = 500;
	const int k_cIterationsSignBig = 25;

	uint64 usecStart;
	CUtlBuffer bufData;
	bufData.EnsureCapacity( k_cubPktBig );
	bufData.SeekPut( CUtlBuffer::SEEK_HEAD, k_cubPktBig );
	CCrypto::GenerateRandomBlock(bufData.Base(), k_cubPktBig);

	CECKeyExchangePublicKey lastPub;
	CECKeyExchangePrivateKey lastPriv;
	CCrypto::GenerateKeyExchangeKeyPair( &lastPub, &lastPriv );

	usecStart = Plat_USTime();
	int x = 0;
	for ( int i = 0; i < k_cIterationsECDH; ++i )
	{
		SHA256Digest_t sharedsecret;
		CECKeyExchangePublicKey pub;
		CECKeyExchangePrivateKey priv;
		CCrypto::GenerateKeyExchangeKeyPair( &pub, &priv );
		CHECK( CCrypto::PerformKeyExchange( priv, lastPub, &sharedsecret ) );
		x ^= sharedsecret[0] ^ sharedsecret[sizeof(sharedsecret)-1];
	}
	double dMicrosecPerECDH = double( Plat_USTime() - usecStart ) / k_cIterationsECDH;


	CECSigningPublicKey signPub;
	CECSigningPrivateKey signPriv;
	CCrypto::GenerateSigningKeyPair( &signPub, &signPriv );

	CryptoSignature_t signature;
	// small data sign
	usecStart = Plat_USTime();
	for ( int i = 0; i < k_cIterationsSignSmall; ++i )
	{
		CCrypto::GenerateSignature( (uint8*)bufData.Base(), k_cubPktSmall, signPriv, &signature );
		x ^= signature[0] ^ signature[sizeof( signature ) - 1];
	}
	double dMicrosecPerSignSmall = double( Plat_USTime() - usecStart ) / k_cIterationsSignSmall;

	// small data verify
	usecStart = Plat_USTime();
	for ( int i = 0; i < k_cIterationsSignSmall; ++i )
	{
		x ^= (int)CCrypto::VerifySignature( (uint8*)bufData.Base(), k_cubPktSmall, signPub, signature );
	}
	double dMicrosecPerSignCheckSmall = double( Plat_USTime() - usecStart ) / k_cIterationsSignSmall;

	// large data sign
	usecStart = Plat_USTime();
	for ( int i = 0; i < k_cIterationsSignBig; ++i )
	{
		CCrypto::GenerateSignature( (uint8*)bufData.Base(), k_cubPktBig, signPriv, &signature );
		x ^= signature[0] ^ signature[sizeof( signature ) - 1];
	}
	double elapsed = Plat_USTime() - usecStart;
	double dMicrosecPerSignBig = elapsed / k_cIterationsSignBig;
	double dRateLargeMBPerSec = double( k_cubPktBig ) * k_cIterationsSignBig / elapsed;

	// large data verify
	usecStart = Plat_USTime();
	for ( int i = 0; i < k_cIterationsSignBig; ++i )
	{
		x ^= (int)CCrypto::VerifySignature( (uint8*)bufData.Base(), k_cubPktBig, signPub, signature );
	}
	elapsed = Plat_USTime() - usecStart;
	double dMicrosecPerSignCheckBig = elapsed / k_cIterationsSignBig;
	double dRateLargeMBPerSecCheck = double( k_cubPktBig ) * k_cIterationsSignBig / elapsed;

	printf( "\tEphemeral curve25519 key exchange:\t\t\t%f microseconds each (%d iterations)\n", dMicrosecPerECDH, k_cIterationsSignSmall );
	printf( "\tCalculate ed25519 signature (small):\t\t\t%f microseconds each (%d iterations)\n", dMicrosecPerSignSmall, k_cIterationsSignSmall );
	printf( "\tCalculate ed25519 signature (big):\t\t\t%f microseconds each (%d iterations)\n", dMicrosecPerSignBig, k_cIterationsSignBig );
	printf( "\tCalculate ed25519 signature (big):\t\t\t%f MB/sec (%d iterations)\n", dRateLargeMBPerSec, k_cIterationsSignBig );
	printf( "\tVerify ed25519 signature (small):\t\t\t%f microseconds each (%d iterations)\n", dMicrosecPerSignCheckSmall, k_cIterationsSignSmall );
	printf( "\tVerify ed25519 signature (big):\t\t\t%f microseconds each (%d iterations)\n", dMicrosecPerSignCheckBig, k_cIterationsSignBig );
	printf( "\tVerify ed25519 signature (big):\t\t\t%f MB/sec (%d iterations)\n", dRateLargeMBPerSecCheck, k_cIterationsSignBig );
}

//-----------------------------------------------------------------------------
// Purpose: Performs specified # of symmetric encryptions
//-----------------------------------------------------------------------------
void SymmetricAuthEncryptRepeatedly( int cIterations, AES_GCM_EncryptContext &ctxEnc, uint8 *pubData, int cubToEncrypt, uint8 *pubIV )
{
	int nBufSize = cubToEncrypt + 32;				// 16 = AES block size.. worst case for padded data
	uint8 *pEncrypted = new uint8[ nBufSize ];

	// try a bunch of iterations of symmetric encrypting big packets
	for ( int iIteration = 0; iIteration < cIterations; iIteration++ )
	{
		uint cubEncrypted = nBufSize;
		bool bRet = ctxEnc.Encrypt(
			&pubData[iIteration], cubToEncrypt,
			pubIV,
			pEncrypted, &cubEncrypted,
			nullptr, 0 );
		CHECK( bRet );	// must succeed
	}

	delete [] pEncrypted;
}

//-----------------------------------------------------------------------------
// Purpose: Performs specified # of symmetric descryptions
//-----------------------------------------------------------------------------
void SymmetricAuthDecryptRepeatedly( int cIterations, AES_GCM_DecryptContext &ctxDec, uint8 *pubEncrypted, int cubEncrypted, uint8 *pubIV )
{
	int nBufSize = cubEncrypted + 32;				// 16 = AES block size.. worst case for padded data
	uint8 *pDecrypted = new uint8[ nBufSize ];

	// try a bunch of iterations of symmetric encrypting big packets
	for ( int iIteration = 0; iIteration < cIterations; iIteration++ )
	{
		uint cubOutput = nBufSize;
		bool bRet = ctxDec.Decrypt(
			pubEncrypted, cubEncrypted,
			pubIV,
			pDecrypted, &cubOutput,
			nullptr, 0 );
		CHECK( bRet );												// must succeed
	}

	delete [] pDecrypted;
}

//-----------------------------------------------------------------------------
// Purpose: Tests symmetric crypto perf
//-----------------------------------------------------------------------------
void TestSymmetricAuthCryptoPerf()
{
	const int k_cIterations = 10000;

	const int k_cMaxData = 800;
	const int k_cBufs = 5;
	const int k_cubTestBuf = k_cMaxData * k_cBufs + k_cIterations;

	const int k_cubPktBig = 1200;
	const int k_cubPktSmall = 100;

	AES_GCM_EncryptContext ctxEnc;
	AES_GCM_DecryptContext ctxDec;

	uint64 usecStart;

	// generate a random key
	uint8 rgubKey[k_nSymmetricKeyLen];
	uint8 rgubIV[k_nSymmetricIVSize];

	CCrypto::GenerateRandomBlock( rgubKey, V_ARRAYSIZE( rgubKey ) );
	CCrypto::GenerateRandomBlock( rgubIV, V_ARRAYSIZE( rgubIV ) );

	uint8 rgubData[k_cubTestBuf];

	// Initialize encrypt/decrypt contexts
	ctxEnc.Init(
		rgubKey, k_nSymmetricKeyLen,
		V_ARRAYSIZE(rgubIV),
		k_nSymmetricGCMTagSize );
	ctxDec.Init(
		rgubKey, k_nSymmetricKeyLen,
		V_ARRAYSIZE(rgubIV),
		k_nSymmetricGCMTagSize );

	// fill data buffer with arbitrary data
	uint8 rgubEncrypted[ k_cubPktBig + 32 ];		// 16 = AES block size.. worst case for padded data
	for ( int iubData = 0; iubData < V_ARRAYSIZE( rgubData ); iubData ++ )
			rgubData[iubData] = (uint8) iubData;

	// try a bunch of iterations of symmetric encrypting small packets
	usecStart = Plat_USTime();
	SymmetricAuthEncryptRepeatedly( k_cIterations, ctxEnc, rgubData, k_cubPktSmall, rgubIV );
	int cMicroSecPerEncryptSmall = Plat_USTime() - usecStart;

	// try a bunch of iterations of symmetric encrypting big packets
	usecStart = Plat_USTime();
	SymmetricAuthEncryptRepeatedly( k_cIterations, ctxEnc, rgubData, k_cubPktBig, rgubIV );
	int cMicroSecPerEncryptBig = Plat_USTime() - usecStart;
	double dRateLargeEncrypt = double( k_cubPktBig ) * k_cIterations / cMicroSecPerEncryptBig;

	// try a bunch of iterations decrypting small packets
	uint cubEncrypted = V_ARRAYSIZE( rgubEncrypted );
	bool bRet = ctxEnc.Encrypt(
			rgubData, k_cubPktSmall,
			rgubIV,
			rgubEncrypted, &cubEncrypted,
			nullptr, 0 );
	CHECK( bRet );
	usecStart = Plat_USTime();
	SymmetricAuthDecryptRepeatedly( k_cIterations, ctxDec, rgubEncrypted, cubEncrypted, rgubIV );
	int cMicroSecPerDecryptSmall = Plat_USTime() - usecStart;

	// try a bunch of iterations decrypting big packets
	cubEncrypted = V_ARRAYSIZE( rgubEncrypted );
	bRet = ctxEnc.Encrypt(
			rgubData, k_cubPktBig,
			rgubIV,
			rgubEncrypted, &cubEncrypted,
			nullptr, 0 );
	CHECK( bRet );
	usecStart = Plat_USTime();
	SymmetricAuthDecryptRepeatedly( k_cIterations, ctxDec, rgubEncrypted, cubEncrypted, rgubIV );
	int cMicroSecPerDecryptBig = Plat_USTime() - usecStart;
	double dRateLargeDecrypt = double( k_cubPktBig ) * k_cIterations / cMicroSecPerDecryptBig;

	printf( "\tSymmetric GCM encrypt (small):\t\t%d microsec (%d iterations)\n", cMicroSecPerEncryptSmall, k_cIterations );
	printf( "\tSymmetric GCM encrypt (big):\t\t%d microsec (%d iterations)\n", cMicroSecPerEncryptBig, k_cIterations );
	printf( "\tSymmetric GCM encrypt (big):\t\t%f MB/sec (%d iterations)\n", dRateLargeEncrypt, k_cIterations );
	printf( "\tSymmetric GCM decrypt (small):\t\t%d microsec (%d iterations)\n", cMicroSecPerDecryptSmall, k_cIterations );
	printf( "\tSymmetric GCM decrypt (big):\t\t%d microsec (%d iterations)\n", cMicroSecPerDecryptBig, k_cIterations );
	printf( "\tSymmetric GCM decrypt (big):\t\t%f MB/sec (%d iterations)\n", dRateLargeDecrypt, k_cIterations );
}

bool chdir_to_bindir()
{
#ifdef LINUX
	char rgchPath[PATH_MAX], *pchPathTail;
	memset(rgchPath, 0, sizeof(rgchPath));

	if (readlink("/proc/self/exe", rgchPath, sizeof(rgchPath)) == -1)
		return false;

	pchPathTail = strrchr(rgchPath, '/');
	if (!pchPathTail)
		return false;

	*pchPathTail = 0;
	if (chdir(rgchPath) == -1)
		return false;
#endif
	return true;
}

int main()
{
	if (!chdir_to_bindir())
		printf("WARNING: Could not change working directory, AES-GCM test vectors will probably break.\n");

	CCrypto::Init();

	TestCryptoEncoding();
	TestSymmetricAuthCryptoVectors();
	TestEllipticCrypto();
	TestOpenSSHEd25519();
	TestEllipticPerf();
	TestSymmetricAuthCryptoPerf();

	return g_failed ? 1 : 0;
}
