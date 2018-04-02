//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Steam datagram routing server
//
//=============================================================================

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../steamnetworkingsockets_internal.h"
#include "crypto.h"
#include "keypair.h"
#include <vstdlib/random.h>
//#include "curl/curl.h"

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
std::vector<SteamNetworkingPOPID> s_vecDataCenterIDs;
int s_nExpiryDays = k_nDefaultExpiryDays;

static void PrintArgSummaryAndExit( int returnCode = 1 )
{
	printf( R"usage(Usage:

To generate a keypair:

	steamnetworkingsockets_certtool [options] gen_keypair

To create a cert for a keypair:

	steamnetworkingsockets_certtool [options] create_cert

To do both steps at once:

	steamnetworkingsockets_certtool [options] gen_keypair create_cert

Options:

  --help                       You're looking at it
  --ca-priv-key-file FILENAME  Load up CA master private key from file (PEM-like blob)
  --pub-key-file FILENAME      Load public key key from file (authorized_keys)
  --pub-key KEY                Use specific public key (authorized_keys blob)
  --data-center CODE[,CODE...] 3- or 4-character data center code(s)
  --expiry DAYS                Cert will expire in N days (default=%d)
)usage",
	k_nDefaultExpiryDays
);

	exit( returnCode );
}

///////////////////////////////////////////////////////////////////////////////
//
// Cert creation
//
///////////////////////////////////////////////////////////////////////////////

void GenKeyPair()
{
	Msg( "Generating keypair...\n" );
	CECSigningPrivateKey privKey;
	CCrypto::GenerateSigningKeyPair( &s_keyCertPub, &privKey );

	uint64 nKeyID = CalculatePublicKeyID( s_keyCertPub );
	DbgVerify( nKeyID != 0 );

	// Generate the key comment
	std::string sComment;
	for ( uint32 id: s_vecDataCenterIDs )
	{
		char szTemp[ 8 ];
		GetSteamNetworkingLocationPOPStringFromID( id, szTemp );
		sComment += szTemp;
		sComment += '-';
	}

	// Key ID
	sComment += "ID";
	sComment += nKeyID;

	uint32 cbText = 0;
	char text[ 16000 ];

	DbgVerify( s_keyCertPub.GetAsOpenSSHAuthorizedKeys( text, sizeof(text), &cbText, sComment.c_str() ) );
	Msg( "\nPublic key:\n" );
	Msg( "%s\n", text );

	// Round trip sanity check
	{
		CECSigningPublicKey pubKeyCheck;
		DbgVerify( pubKeyCheck.LoadFromAndWipeBuffer( text, cbText ) );
		DbgVerify( pubKeyCheck == s_keyCertPub );
	}

	DbgVerify( privKey.GetAsPEM( text, sizeof(text), &cbText ) );
	Msg( "\nPrivate key:\n" );
	Msg( "%s\n", text );

	// Round trip sanity check
	{
		CECSigningPrivateKey privKeyCheck;
		DbgVerify( privKeyCheck.LoadFromAndWipeBuffer( text, cbText ) );
		DbgVerify( privKeyCheck == privKey );
	}
}

static const char k_szSDRCertPEMHeader[] = "-----BEGIN STEAMDATAGRAM CERT-----";
static const char k_szSDRCertPEMFooter[] = "-----END STEAMDATAGRAM CERT-----";

void PrintCertInfo( const CMsgSteamDatagramCertificateSigned &msgSigned, std::string &sOutResult, const char *pszJSONIndent )
{
	char szTemp[ 256 ];

	CMsgSteamDatagramCertificate msgCert;
	msgCert.ParseFromString( msgSigned.cert() );

	CECSigningPublicKey pubKey;
	pubKey.Set( msgCert.key_data().c_str(), (uint32)msgCert.key_data().length() );

	time_t timeCreated = msgCert.time_created();
	time_t timeExpiry = msgCert.time_expiry();

	char szTimeCreated[ 128 ];
	Plat_ctime( &timeCreated, szTimeCreated, sizeof(szTimeCreated) );
	V_StripTrailingWhitespaceASCII( szTimeCreated );

	char szTimeExpiry[ 128 ];
	Plat_ctime( &timeExpiry, szTimeExpiry, sizeof(szTimeExpiry) );
	V_StripTrailingWhitespaceASCII( szTimeExpiry );

	std::string sDataCenterIDs;
	for ( uint32 id: msgCert.gameserver_datacenter_ids() )
	{
		GetSteamNetworkingLocationPOPStringFromID( id, szTemp );

		if ( pszJSONIndent )
		{
			if ( !sDataCenterIDs.empty() )
				sDataCenterIDs += ',';
			sDataCenterIDs += '\"';
			sDataCenterIDs += szTemp;
			sDataCenterIDs += '\"';
		}
		else
		{
			if ( !sDataCenterIDs.empty() )
				sDataCenterIDs += ' ';
			sDataCenterIDs += szTemp;
		}
	}

	if ( pszJSONIndent )
	{
		V_sprintf_safe( szTemp, "%s\"key_id\": %" PRIu64 ",\n", pszJSONIndent, CalculatePublicKeyID( pubKey ) );
		sOutResult += szTemp;
		if ( !sDataCenterIDs.empty() )
		{
			V_sprintf_safe( szTemp, "%s\"data_centers\": [ %s ],\n", pszJSONIndent, sDataCenterIDs.c_str() );
			sOutResult += szTemp;
		}
		V_sprintf_safe( szTemp, "%s\"created\": \"%s\",\n", pszJSONIndent, szTimeCreated );
		sOutResult += szTemp;
		V_sprintf_safe( szTemp, "%s\"expires\": \"%s\",\n", pszJSONIndent, szTimeExpiry );
		sOutResult += szTemp;
		V_sprintf_safe( szTemp, "%s\"ca_key_id\": %" PRIu64 "\n", pszJSONIndent, msgSigned.ca_key_id() );
		sOutResult += szTemp;
	}
	else
	{
		V_sprintf_safe( szTemp, "Public key ID. . : %" PRIu64 "\n", CalculatePublicKeyID( pubKey ) );
		sOutResult += szTemp;
		V_sprintf_safe( szTemp, "Created. . . . . : %s\n", szTimeCreated );
		sOutResult += szTemp;
		V_sprintf_safe( szTemp, "Expires. . . . . : %s\n", szTimeExpiry );
		sOutResult += szTemp;
		V_sprintf_safe( szTemp, "CA key ID. . . . : %" PRIu64 "\n", msgSigned.ca_key_id() );
		sOutResult += szTemp;
		if ( !sDataCenterIDs.empty() )
		{
			V_sprintf_safe( szTemp, "Data center(s) . : %s\n", sDataCenterIDs.c_str() );
			sOutResult += szTemp;
		}
	}
}

void CreateCert()
{
	if ( !s_keyCAPriv.IsValid() )
		Plat_FatalError( "CA private key not specified" );
	if ( !s_keyCertPub.IsValid() )
		Plat_FatalError( "Public key not specified" );
	if ( s_vecDataCenterIDs.size() == 0 )
		Plat_FatalError( "Must restrict certificate to a data center" );

	CECSigningPublicKey caPubKey;
	s_keyCAPriv.GetPublicKey( &caPubKey );
	uint64 nCAKeyID = CalculatePublicKeyID( caPubKey );
	Assert( nCAKeyID != 0 );

	CMsgSteamDatagramCertificate msgCert;
	msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );
	msgCert.set_key_data( s_keyCertPub.GetData(), s_keyCertPub.GetLength() );
	msgCert.set_time_created( time( nullptr ) );
	msgCert.set_time_expiry( msgCert.time_created() + s_nExpiryDays*24*3600 );
	for ( uint32 id: s_vecDataCenterIDs )
		msgCert.add_gameserver_datacenter_ids( id );
	//cert.set_app_id( FIXME )

	CMsgSteamDatagramCertificateSigned msgSigned;
	msgSigned.set_cert( msgCert.SerializeAsString() );

	CryptoSignature_t sig;
	CCrypto::GenerateSignature( (const uint8 *)msgSigned.cert().c_str(), (uint32)msgSigned.cert().length(), s_keyCAPriv, &sig );
	msgSigned.set_ca_key_id( nCAKeyID );
	msgSigned.set_ca_signature( &sig, sizeof(sig) );

	std::string sSigned = msgSigned.SerializeAsString();

//	char text[ 16000 ];
//	uint32 cbText = sizeof(text);
//	DbgVerify( CCrypto::Base64Encode( (const uint8 *)sSigned.c_str(), sSigned.length(), text, &cbText, "" ) );
//	V_StripTrailingWhitespace( text );
//
//	Msg( "Cert JSON blob:\n" );
//	CUtlStringBuilder sJSON;
//	sJSON.AppendFormat( "{\n" );
//	sJSON.AppendFormat( "\t\"cert\": \"%s\"\n", text );
//	PrintCertInfo( msgSigned, sJSON, "\t" );
//	sJSON.AppendFormat( "}\n" );
//	Msg( "%s", sJSON.String() );

	char text[ 16000 ];
	uint32 cbText = sizeof(text);
	DbgVerify( CCrypto::Base64Encode( (const uint8 *)sSigned.c_str(), (uint32)sSigned.length(), text, &cbText, "\n" ) );
	V_StripTrailingWhitespaceASCII( text );

	Msg( "Cert:\n" );
	Msg( "%s\n", k_szSDRCertPEMHeader );
	Msg( "%s\n", text );
	Msg( "%s\n", k_szSDRCertPEMFooter );
	std::string sDetails;
	PrintCertInfo( msgSigned, sDetails, nullptr );
	Msg( "%s", sDetails.c_str() );
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
				Warning( "Expected argument after %s\n", pszSwitch ); \
				PrintArgSummaryAndExit(); \
			} \
			const char *pszArg = argv[nCurArg++];

		#define INVALID_ARG() \
			{ \
				Warning( "Invalid value for %s: '%s'\n", pszSwitch, pszArg ); \
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

		if ( !V_stricmp( pszSwitch, "--pub-key-file" ) )
		{
			GET_ARG();
			CUtlBuffer buf;
			LoadFileIntoBuffer( pszArg, buf );
			if ( !s_keyCertPub.LoadFromAndWipeBuffer( buf.Base(), buf.TellPut() ) )
				Plat_FatalError( "File '%s' doesn't contain a valid authorized_keys style public Ed25519 keyfile.  (Try exporting from OpenSSH)\n", pszArg );
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--pub-key" ) )
		{
			GET_ARG();
			if ( !s_keyCertPub.Set( pszArg, V_strlen(pszArg) ) )
				Plat_FatalError( "'%s' isn't a valid authorized_keys style public Ed25519 keyfile.  (Try exporting from OpenSSH)\n", pszArg );
			continue;
		}

		if ( !V_stricmp( pszSwitch, "--data-center" ) )
		{
			GET_ARG();

			CUtlVectorAutoPurge<char *> vecCodes;
			V_SplitString( pszArg, ",", vecCodes );
			if ( vecCodes.IsEmpty() )
				Plat_FatalError( "'%s' isn't a valid comma-separated list of data center codes\n", pszArg );

			for ( const char *pszCode: vecCodes )
			{
				int l = V_strlen( pszCode );
				if ( l < 3 || l > 4 )
					Plat_FatalError( "'%s' isn't a valid data center code\n", pszCode );
				s_vecDataCenterIDs.push_back( CalculateSteamNetworkingPOPIDFromString( pszCode ) );
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

		//
		// Anything else?
		//

		Warning( "Unrecognized option '%s'\n", pszSwitch );
		PrintArgSummaryAndExit();
	}

	if ( !bDidSomething )
		PrintArgSummaryAndExit( 0 );

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
