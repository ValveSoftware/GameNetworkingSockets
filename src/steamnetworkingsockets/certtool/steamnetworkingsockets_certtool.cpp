//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Steam datagram routing server
//
//=============================================================================

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include "../steamnetworkingsockets_internal.h"
#include "crypto.h"
#include "crypto_25519.h"
#include "keypair.h"
#include <vstdlib/random.h>
//#include "curl/curl.h"

#include <picojson.h>

// Really?
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Must be the last include
#include <tier0/memdbgon.h>

using namespace SteamNetworkingSocketsLib;

///////////////////////////////////////////////////////////////////////////////
//
// Misc
//
///////////////////////////////////////////////////////////////////////////////

static void LoadFileIntoBuffer( const char *pszFilename, CUtlBuffer &buf )
{
	// Try to open
	FILE *f = fopen( pszFilename, "rb" );
	if ( f == NULL )
		Plat_FatalError( "Can't open file '%s'\n", pszFilename );

	// Determine file size by seeking around
	fseek( f, 0, SEEK_END );
	int cbFile = ftell( f );
	fseek( f, 0, SEEK_SET );

	// Allocate once
	buf.EnsureCapacity( cbFile );

	// Read data
	size_t r = fread( buf.Base(), 1, cbFile, f );

	// Clean up
	fclose(f);

	// Check for failure
	if ( r != cbFile )
		Plat_FatalError( "Error reading from file '%s'\n", pszFilename );

	// Set buffer file pointers
	buf.SeekPut( CUtlBuffer::SEEK_HEAD, cbFile );
	buf.SeekGet( CUtlBuffer::SEEK_HEAD, 0 );
}

///////////////////////////////////////////////////////////////////////////////
//
// Command line help and options
//
///////////////////////////////////////////////////////////////////////////////

const int k_nDefaultExpiryDays = 365*2;

CECSigningPrivateKey s_keyCAPriv;
CECSigningPublicKey s_keyCertPub;
std::vector<SteamNetworkingPOPID> s_vecPOPIDs;
std::vector<AppId_t> s_vecAppIDs;
int s_nExpiryDays = k_nDefaultExpiryDays;
bool s_bOutputJSON;
picojson::object s_jsonOutput;

static void PrintArgSummaryAndExit( int returnCode = 1 )
{
	printf( R"usage(Usage:

To generate a signing keypair (currently always Ed25519):

	steamnetworkingsockets_certtool [options] gen_keypair

To create a cert for a keypair:

	steamnetworkingsockets_certtool [options] create_cert

To do both steps at once:

	steamnetworkingsockets_certtool [options] gen_keypair create_cert

To generate a Diffie-Hellman key exchange keypair (X25519, for sending
private messages, not for signing):

	steamnetworkingsockets_certtool [options] gen_keyexchange_keypair

Options:

  --help                       You're looking at it
  --ca-priv-key-file FILENAME  Load CA private key from file (PEM-like blob)
  --ca-priv-key KEY            Use CA private key data (PEM-like blob.  Don't
                               forget to quote it!)
  --pub-key-file FILENAME      Load public key key from file (authorized_keys)
  --pub-key KEY                Use specific public key (authorized_keys blob)
  --pop CODE[,CODE...]         Restrict POP(s).  (3- or 4-character code(s))
  --app APPID[,APPID...]       Restrict to appid(s).
  --expiry DAYS                Cert will expire in N days (default=%d)
  --output-json                Output JSON.
)usage",
	k_nDefaultExpiryDays
);

	exit( returnCode );
}

void Printf( const char *pszFmt, ... )
{
	if ( s_bOutputJSON )
		return;
	va_list ap;
	va_start( ap, pszFmt );
	vprintf( pszFmt, ap );
	va_end( ap );
}

static std::string KeyIDAsString( uint64 nKeyID )
{
	char temp[ 64 ];
	V_sprintf_safe( temp, "%llu", (unsigned long long)nKeyID );
	return std::string( temp );
}

static std::string PublicKeyAsAuthorizedKeys( const CECSigningPublicKey &pubKey )
{
	uint32 cbText = 0;
	char text[ 2048 ];
	DbgVerify( pubKey.GetAsOpenSSHAuthorizedKeys( text, sizeof(text), &cbText, "" ) );
	return std::string( text );
}

static std::string PublicKeyAsAuthorizedKeys()
{
	return PublicKeyAsAuthorizedKeys( s_keyCertPub );
}

static std::string PublicKeyIDAsString()
{
	uint64 nKeyID = CalculatePublicKeyID( s_keyCertPub );
	DbgVerify( nKeyID != 0 );
	return KeyIDAsString( nKeyID );
}

///////////////////////////////////////////////////////////////////////////////
//
// Cert creation
//
///////////////////////////////////////////////////////////////////////////////

static void AddPublicKeyInfoToJSON()
{
	std::string sKeyID = PublicKeyIDAsString();
	std::string sComment = "ID"+sKeyID;

	s_jsonOutput[ "public_key_id" ] = picojson::value( PublicKeyIDAsString() );
	s_jsonOutput[ "public_key" ] = picojson::value( PublicKeyAsAuthorizedKeys() );
}

void GenKeyPair()
{
	Printf( "Generating keypair...\n" );
	CECSigningPrivateKey privKey;
	CCrypto::GenerateSigningKeyPair( &s_keyCertPub, &privKey );

	std::string sKeyID( PublicKeyIDAsString() );

	// Generate the key comment
	std::string sComment;
	for ( AppId_t id: s_vecAppIDs )
	{
		char szTemp[ 8 ];
		V_sprintf_safe( szTemp, "%u", id );
		sComment += szTemp;
		sComment += '-';
	}
	for ( SteamNetworkingPOPID id: s_vecPOPIDs )
	{
		char szTemp[ 8 ];
		GetSteamNetworkingLocationPOPStringFromID( id, szTemp );
		sComment += szTemp;
		sComment += '-';
	}

	// Key ID
	sComment += "ID";
	sComment += sKeyID;

	uint32 cbText = 0;
	char text[ 16000 ];

	DbgVerify( s_keyCertPub.GetAsOpenSSHAuthorizedKeys( text, sizeof(text), &cbText, sComment.c_str() ) );
	Printf( "\nPublic key: %s\n", text );
	AddPublicKeyInfoToJSON();

	// Round trip sanity check
	{
		CECSigningPublicKey pubKeyCheck;
		DbgVerify( pubKeyCheck.LoadFromAndWipeBuffer( text, cbText ) );
		DbgVerify( pubKeyCheck == s_keyCertPub );
	}

	DbgVerify( privKey.GetAsPEM( text, sizeof(text), &cbText ) );
	Printf( "%s\n", text );

	s_jsonOutput[ "private_key" ] = picojson::value( text );

	// Round trip sanity check
	{
		CECSigningPrivateKey privKeyCheck;
		DbgVerify( privKeyCheck.LoadFromAndWipeBuffer( text, cbText ) );
		DbgVerify( privKeyCheck == privKey );
	}
}

static const char k_szSDRCertPEMHeader[] = "-----BEGIN STEAMDATAGRAM CERT-----";
static const char k_szSDRCertPEMFooter[] = "-----END STEAMDATAGRAM CERT-----";

void PrintCertInfo( const CMsgSteamDatagramCertificateSigned &msgSigned, picojson::object &outJSON )
{
	char szTemp[ 256 ];

	CMsgSteamDatagramCertificate msgCert;
	msgCert.ParseFromString( msgSigned.cert() );

	CECSigningPublicKey pubKey;
	if ( !pubKey.SetRawDataWithoutWipingInput( msgCert.key_data().c_str(), msgCert.key_data().length() ) )
		Plat_FatalError( "Cert has bad public key" );

	time_t timeCreated = msgCert.time_created();
	time_t timeExpiry = msgCert.time_expiry();

	char szTimeCreated[ 128 ];
	V_strcpy_safe( szTimeCreated, ctime( &timeCreated ) );
	V_StripTrailingWhitespaceASCII( szTimeCreated );

	char szTimeExpiry[ 128 ];
	V_strcpy_safe( szTimeExpiry, ctime( &timeExpiry ) );
	V_StripTrailingWhitespaceASCII( szTimeExpiry );

	std::string sPOPIDs;
	{
		picojson::array pop_ids;
		for ( SteamNetworkingPOPID id: msgCert.gameserver_datacenter_ids() )
		{
			GetSteamNetworkingLocationPOPStringFromID( id, szTemp );

			if ( !sPOPIDs.empty() )
				sPOPIDs += ' ';
			sPOPIDs += szTemp;
			pop_ids.push_back( picojson::value( szTemp ) );
		}
		if ( !pop_ids.empty() )
		{
			outJSON[ "pop_ids" ] = picojson::value( pop_ids );
		}
	}

	std::string sAppIDs;
	{
		picojson::array app_ids;
		for ( AppId_t id: msgCert.app_ids() )
		{
			V_sprintf_safe( szTemp, "%u", id );

			if ( !sAppIDs.empty() )
				sAppIDs += ' ';
			sAppIDs += szTemp;
			app_ids.push_back( picojson::value( (double)id ) );
		}
		if ( !app_ids.empty() )
		{
			outJSON[ "app_ids" ] = picojson::value( app_ids );
		}
	}

	uint64 key_id = CalculatePublicKeyID( pubKey );

	outJSON[ "time_created" ] = picojson::value( (double)timeCreated );
	outJSON[ "time_created_string" ] = picojson::value( szTimeCreated );
	outJSON[ "time_expiry" ] = picojson::value( (double)timeExpiry );
	outJSON[ "time_expiry_string" ] = picojson::value( szTimeExpiry );
	outJSON[ "ca_key_id" ] = picojson::value( KeyIDAsString( msgSigned.ca_key_id() ) );

	Printf( "#Public key . . . : %s ID%s\n", PublicKeyAsAuthorizedKeys( pubKey ).c_str(), KeyIDAsString( key_id ).c_str() );
	Printf( "#Created. . . . . : %s (%llu)\n", szTimeCreated, (unsigned long long)timeCreated );
	Printf( "#Expires. . . . . : %s (%llu)\n", szTimeExpiry, (unsigned long long)timeCreated );
	Printf( "#CA key ID. . . . : %s\n", KeyIDAsString( msgSigned.ca_key_id() ).c_str() );
	if ( !sAppIDs.empty() )
	{
		Printf( "#App ID(s). . . . : %s\n", sAppIDs.c_str() );
	}
	if ( !sPOPIDs.empty() )
	{
		Printf( "#POP ID(s). . . . : %s\n", sPOPIDs.c_str() );
	}
}

std::string CertToBase64( const CMsgSteamDatagramCertificateSigned &msgCert, const char *pszNewline )
{
	std::string sSigned = msgCert.SerializeAsString();

	char text[ 16000 ];
	uint32 cbText = sizeof(text);
	DbgVerify( CCrypto::Base64Encode( (const uint8 *)sSigned.c_str(), (uint32)sSigned.length(), text, &cbText, pszNewline ) );
	V_StripTrailingWhitespaceASCII( text );
	return std::string(text);
}

std::string CertToPEM( const CMsgSteamDatagramCertificateSigned &msgCert )
{
	std::string body = CertToBase64( msgCert, "\n" );
	return std::string( k_szSDRCertPEMHeader ) + '\n' + body + '\n' + k_szSDRCertPEMFooter + '\n';
}

void CreateCert()
{
	if ( !s_keyCAPriv.IsValid() )
		Plat_FatalError( "CA private key not specified" );
	if ( !s_keyCertPub.IsValid() )
		Plat_FatalError( "Public key not specified" );

	CECSigningPublicKey caPubKey;
	s_keyCAPriv.GetPublicKey( &caPubKey );
	uint64 nCAKeyID = CalculatePublicKeyID( caPubKey );
	Assert( nCAKeyID != 0 );

	CMsgSteamDatagramCertificate msgCert;
	msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );
	DbgVerify( s_keyCertPub.GetRawDataAsStdString( msgCert.mutable_key_data() ) );
	msgCert.set_time_created( time( nullptr ) );
	msgCert.set_time_expiry( msgCert.time_created() + s_nExpiryDays*24*3600 );
	for ( AppId_t nAppID: s_vecAppIDs )
		msgCert.add_app_ids( nAppID );
	for ( uint32 id: s_vecPOPIDs )
		msgCert.add_gameserver_datacenter_ids( id );
	//cert.set_app_id( FIXME )

	CMsgSteamDatagramCertificateSigned msgSigned;
	msgSigned.set_cert( msgCert.SerializeAsString() );

	CryptoSignature_t sig;
	s_keyCAPriv.GenerateSignature( msgSigned.cert().c_str(), msgSigned.cert().length(), &sig );
	msgSigned.set_ca_key_id( nCAKeyID );
	msgSigned.set_ca_signature( &sig, sizeof(sig) );

	Printf( "%s", CertToPEM( msgSigned ).c_str() );

	std::string pem_json = CertToBase64( msgSigned, "" );
	s_jsonOutput[ "cert" ] = picojson::value( pem_json );

	PrintCertInfo( msgSigned, s_jsonOutput );
}

template <typename TCryptoKey>
void PrintHDKey( const TCryptoKey &key, const char *pszPlainTextHeader, const char *pszJSON )
{
	CUtlBuffer bufTemp;
	bufTemp.EnsureCapacity( key.GetRawData( nullptr ) );
	uint32 cbRaw = key.GetRawData( bufTemp.Base() );
	uint32 cbText = cbRaw*2 + 8;

	CUtlBuffer bufText;
	bufText.EnsureCapacity( cbText );

	char *pszHex = (char *)bufText.Base();
	DbgVerify( CCrypto::HexEncode( bufTemp.Base(), cbRaw, pszHex, cbText ) );

	Printf( "%s: %s\n", pszPlainTextHeader, pszHex );
	s_jsonOutput[ pszJSON ] = picojson::value( pszHex );

	// !TEST! Round-trip to make sure we are working
	TCryptoKey keyCheck;
	DbgVerify( keyCheck.SetFromHexEncodedString( pszHex ) );
	DbgVerify( keyCheck == key );
}

void GenDHKeyPair()
{
	Printf( "Generating Diffie-Hellman X25519 keypair...\n" );
	CECKeyExchangePrivateKey privKey;
	CECKeyExchangePublicKey pubKey;
	CCrypto::GenerateKeyExchangeKeyPair( &pubKey, &privKey );

	PrintHDKey( privKey, "Private key . ", "private_key" );
	PrintHDKey( pubKey,  "Public key. . ", "public_key" );
}

///////////////////////////////////////////////////////////////////////////////
//
// main
//
///////////////////////////////////////////////////////////////////////////////

int main( int argc, char **argv )
{
//	// Tell engine we're headless, if we're not debugging
//	if ( !Plat_IsInDebugSession() )
//		Plat_EnableHeadlessMode();

	// Seed random number generator from a high quality source of genuine entropy
	{
		int seed;
		CCrypto::GenerateRandomBlock( &seed, sizeof(seed) );
		WeakRandomSeed( seed );
	}

	//// !TEST!
	//#ifdef _DEBUG
	//	TestKeyValuesJSONParser();
	//#endif

	//LoggingSystem_RegisterLoggingListener( new SDRLoggingListener );

	// Process command line
	bool bDidSomething = false;
	int nCurArg = 1;
	while ( nCurArg < argc )
	{
		#define GET_ARG() \
			if ( nCurArg >= argc ) \
			{ \
				printf( "Expected argument after %s\n", pszSwitch ); \
				PrintArgSummaryAndExit(); \
			} \
			const char *pszArg = argv[nCurArg++];

		#define INVALID_ARG() \
			{ \
				printf( "Invalid value for %s: '%s'\n", pszSwitch, pszArg ); \
				PrintArgSummaryAndExit(); \
			}

		const char *pszSwitch = argv[nCurArg++];
		if ( !V_stricmp( pszSwitch, "--help" ) || !V_stricmp( pszSwitch, "-h" ) || !V_stricmp( pszSwitch, "-?" ) )
		{
			PrintArgSummaryAndExit(0);
		}

		if ( !V_stricmp( pszSwitch, "--ca-priv-key-file" ) )
		{
			GET_ARG();
			CUtlBuffer buf;
			LoadFileIntoBuffer( pszArg, buf );
			if ( !s_keyCAPriv.LoadFromAndWipeBuffer( buf.Base(), buf.TellPut() ) )
				Plat_FatalError( "File '%s' doesn't contain a valid private Ed25519 keyfile.  (Try exporting from OpenSSH)\n", pszArg );

			continue;
		}

		if ( !V_stricmp( pszSwitch, "--ca-priv-key" ) )
		{
			GET_ARG();
			if ( !s_keyCAPriv.ParsePEM( pszArg, V_strlen(pszArg) ) )
				Plat_FatalError( "Argument after --ca-priv-key is not a valid private Ed25519 keyfile.  (Try exporting from OpenSSH.  And did you remember to quote the argument?)\n" );
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--pub-key-file" ) )
		{
			GET_ARG();
			CUtlBuffer buf;
			LoadFileIntoBuffer( pszArg, buf );
			if ( !s_keyCertPub.LoadFromAndWipeBuffer( buf.Base(), buf.TellPut() ) )
				Plat_FatalError( "File '%s' doesn't contain a valid authorized_keys style public Ed25519 keyfile.  (Try exporting from OpenSSH)\n", pszArg );
			AddPublicKeyInfoToJSON();
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--pub-key" ) )
		{
			GET_ARG();
			if ( !s_keyCertPub.SetFromOpenSSHAuthorizedKeys( pszArg, V_strlen(pszArg) ) )
				Plat_FatalError( "'%s' isn't a valid authorized_keys style public Ed25519 keyfile.  (Try exporting from OpenSSH)\n", pszArg );
			AddPublicKeyInfoToJSON();
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--pop" ) )
		{
			GET_ARG();

			CUtlVectorAutoPurge<char *> vecCodes;
			V_AllocAndSplitString( pszArg, ",", vecCodes );
			if ( vecCodes.IsEmpty() )
				Plat_FatalError( "'%s' isn't a valid comma-separated list of POPs\n", pszArg );

			for ( const char *pszCode: vecCodes )
			{
				int l = V_strlen( pszCode );
				if ( l < 3 || l > 4 )
					Plat_FatalError( "'%s' isn't a valid POP code\n", pszCode );
				s_vecPOPIDs.push_back( CalculateSteamNetworkingPOPIDFromString( pszCode ) );
			}
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--app" ) )
		{
			GET_ARG();

			CUtlVectorAutoPurge<char *> vecCodes;
			V_AllocAndSplitString( pszArg, ",", vecCodes );
			if ( vecCodes.IsEmpty() )
				Plat_FatalError( "'%s' isn't a valid comma-separated list of AppIDs\n", pszArg );

			for ( const char *pszCode: vecCodes )
			{
				int nAppID;
				if ( sscanf( pszCode, "%d", &nAppID) != 1 || nAppID < 0 )
					Plat_FatalError( "'%s' isn't a valid AppID\n", pszCode );
				s_vecAppIDs.push_back( AppId_t( nAppID ) );
			}
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--expiry" ) )
		{
			GET_ARG();
			s_nExpiryDays = 0;
			sscanf( pszArg, "%d", &s_nExpiryDays );
			if ( s_nExpiryDays <= 0 )
				Plat_FatalError( "Invalid expiry '%s'\n", pszArg );
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--output-json" ) )
		{
			s_bOutputJSON = true;
			continue;
		}

		#undef GET_ARG
		#undef INVALID_ARG

		//
		// Known commands
		//

		if ( !V_stricmp( pszSwitch, "gen_keypair" ) )
		{
			GenKeyPair();
			bDidSomething = true;
			continue;
		}
		if ( !V_stricmp( pszSwitch, "create_cert" ) )
		{
			CreateCert();
			bDidSomething = true;
			continue;
		}
		if ( !V_stricmp( pszSwitch, "gen_keyexchange_keypair" ) )
		{
			GenDHKeyPair();
			bDidSomething = true;
			continue;
		}

		//
		// Anything else?
		//

		fflush(stdout);
		fprintf( stderr, "Unrecognized option '%s'\n", pszSwitch );
		fflush(stderr);
		PrintArgSummaryAndExit();
	}

	if ( !bDidSomething )
		PrintArgSummaryAndExit( 0 );

	if ( s_bOutputJSON )
	{
		puts( picojson::value( s_jsonOutput ).serialize( true ).c_str() );
	}

	return 0;
}

#ifdef _WIN32
int APIENTRY WinMain( HINSTANCE hInstance,
	HINSTANCE hPrevInstance,
	LPSTR     lpCmdLine,
	int       nCmdShow )
{
	extern int __argc;
	extern char **__argv;
	int argc = __argc;
	char **argv = __argv;

	REFERENCE( hInstance );
	REFERENCE( hPrevInstance );
	REFERENCE( lpCmdLine );
	REFERENCE( nCmdShow );

	return main( argc, argv );
}
#endif
