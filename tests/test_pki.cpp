#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <string>

#include <steamnetworkingsockets/steamnetworkingsockets_certstore.h>
#include <google/protobuf/text_format.h>
#include <common/crypto.h>
#include <common/crypto_25519.h>

using namespace SteamNetworkingSocketsLib;

// Assert() compiles out of release builds, so track failures the same
// way test_crypto does, and make sure the process exits nonzero.
static bool g_failed = false;
#define CHECK( x ) \
	do { \
		if ( !(x) ) \
		{ \
			fprintf( stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #x ); \
			g_failed = true; \
		} \
	} while ( false )

// All certs are generated at runtime, so we can just use the real clock.
static const time_t k_timeNow = time( nullptr );

// A test CA: a keypair, plus a cert for it that we can install in the cert store.
struct TestCA
{
	CECSigningPrivateKey m_keyPrivate;
	CECSigningPublicKey m_keyPublic;
	uint64 m_nKeyID;
	std::string m_sCertBase64; // CMsgSteamDatagramCertificateSigned, base64-encoded
};

// Finish a cert for the given public key, sign it, and return the
// base64-encoded CMsgSteamDatagramCertificateSigned
static std::string SignCert( CMsgSteamDatagramCertificate &msgCert, const CECSigningPublicKey &keyCertPublic, const CECSigningPrivateKey &keySignerPrivate, uint64 nSignerKeyID )
{
	msgCert.set_time_expiry( k_timeNow + 3600*8 );
	CHECK( keyCertPublic.GetRawDataAsStdString( msgCert.mutable_key_data() ) );
	msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );

	CMsgSteamDatagramCertificateSigned msgSigned;
	CHECK( msgCert.SerializeToString( msgSigned.mutable_cert() ) );
	CryptoSignature_t sig;
	keySignerPrivate.GenerateSignature( msgSigned.cert().c_str(), msgSigned.cert().length(), &sig );
	msgSigned.set_ca_key_id( nSignerKeyID );
	msgSigned.set_ca_signature( &sig, sizeof(sig) );

	std::string sSerialized;
	CHECK( msgSigned.SerializeToString( &sSerialized ) );

	uint32 cchEncoded = CCrypto::Base64EncodeMaxOutput( (uint32)sSerialized.length(), nullptr );
	std::string sBase64;
	sBase64.resize( cchEncoded );
	CHECK( CCrypto::Base64Encode( sSerialized.c_str(), sSerialized.length(), &sBase64[0], &cchEncoded, nullptr ) );
	sBase64.resize( strlen( sBase64.c_str() ) );
	return sBase64;
}

// Create a CA keypair and a cert for it.  pszFields is protobuf TextFormat
// for any restrictions (app_ids, gameserver_datacenter_ids); empty string
// means unrestricted.  If pSigner is nullptr, the cert is self-signed.
static void MakeCA( TestCA &ca, const char *pszFields, const TestCA *pSigner )
{
	CCrypto::GenerateSigningKeyPair( &ca.m_keyPublic, &ca.m_keyPrivate );
	ca.m_nKeyID = CalculatePublicKeyID( ca.m_keyPublic );
	if ( !pSigner )
		pSigner = &ca;

	CMsgSteamDatagramCertificate msgCert;
	CHECK( google::protobuf::TextFormat::ParseFromString( std::string( pszFields ), &msgCert ) );
	ca.m_sCertBase64 = SignCert( msgCert, ca.m_keyPublic, pSigner->m_keyPrivate, pSigner->m_nKeyID );
}

// Generate an end-entity cert, with its own throwaway keypair and the
// requested fields, signed by the given CA
static void GenerateCert( CMsgSteamDatagramCertificateSigned &msgOut, const char *certData, const TestCA &ca )
{
	msgOut.Clear();

	CMsgSteamDatagramCertificate msgCert;
	CHECK( google::protobuf::TextFormat::ParseFromString( std::string( certData ), &msgCert ) );

	CECSigningPrivateKey tempIdentityPrivateKey;
	CECSigningPublicKey tempIdentityPublicKey;
	CCrypto::GenerateSigningKeyPair( &tempIdentityPublicKey, &tempIdentityPrivateKey );

	std::string sBase64 = SignCert( msgCert, tempIdentityPublicKey, ca.m_keyPrivate, ca.m_nKeyID );

	// Hand back the signed message, decoded
	SteamNetworkingErrMsg errMsg;
	CHECK( ParseCertFromBase64( sBase64.c_str(), sBase64.length(), msgOut, errMsg ) );
}

int main()
{
	SteamNetworkingErrMsg errMsg;

	//
	// Build a test PKI at runtime and install it in the cert store.
	//
	// root_app  . . . . : self-signed, explicitly installed by the "app"
	// root_network  . . : self-signed, added WITHOUT app trust.  Must not become
	//                     trusted, because a hardcoded root CA key is in use.
	// root_absent . . . : self-signed, never installed
	// ca_csgo . . . . . : intermediate for app 730, signed by root_app
	// ca_csgo_eatmwh  . : signed by ca_csgo, restricted to the eat/mwh datacenters
	// ca_tf2  . . . . . : intermediate for app 440, signed by root_network
	// ca_dota_revoked . : intermediate for app 570, signed by root_app, then revoked
	//

	const SteamNetworkingPOPID iad = CalculateSteamNetworkingPOPIDFromString( "iad" );
	const SteamNetworkingPOPID sto = CalculateSteamNetworkingPOPIDFromString( "sto" );
	const SteamNetworkingPOPID mwh = CalculateSteamNetworkingPOPIDFromString( "mwh" );
	const SteamNetworkingPOPID eat = CalculateSteamNetworkingPOPIDFromString( "eat" );
	char fields[ 256 ];

	TestCA root_app; MakeCA( root_app, "", nullptr );
	TestCA root_network; MakeCA( root_network, "", nullptr );
	TestCA root_absent; MakeCA( root_absent, "", nullptr );
	TestCA ca_csgo; MakeCA( ca_csgo, "app_ids: 730", &root_app );
	V_sprintf_safe( fields, "gameserver_datacenter_ids: %u gameserver_datacenter_ids: %u", eat, mwh );
	TestCA ca_csgo_eatmwh; MakeCA( ca_csgo_eatmwh, fields, &ca_csgo );
	TestCA ca_tf2; MakeCA( ca_tf2, "app_ids: 440", &root_network );
	TestCA ca_dota_revoked; MakeCA( ca_dota_revoked, "app_ids: 570", &root_app );

	// The app installs its root of trust through the app-trusted entry point.
	// Wrap this one in a PEM-style block, to exercise both accepted input forms.
	std::string sRootAppPEM = "-----BEGIN STEAMDATAGRAM CERT-----\n" + root_app.m_sCertBase64 + "\n-----END STEAMDATAGRAM CERT-----\n";
	CHECK( CertStore_AddTrustedCertFromPEM( sRootAppPEM.c_str(), errMsg ) );

	// Intermediates can arrive through the ordinary path; their trust
	// flows from the chain, not from who installed them.
	CHECK( CertStore_AddCertFromBase64( ca_csgo.m_sCertBase64.c_str(), errMsg ) );
	CHECK( CertStore_AddCertFromBase64( ca_csgo_eatmwh.m_sCertBase64.c_str(), errMsg ) );
	CHECK( CertStore_AddCertFromBase64( ca_dota_revoked.m_sCertBase64.c_str(), errMsg ) );

	// A self-signed root added without app trust parses fine...
	CHECK( CertStore_AddCertFromBase64( root_network.m_sCertBase64.c_str(), errMsg ) );
	CHECK( CertStore_AddCertFromBase64( ca_tf2.m_sCertBase64.c_str(), errMsg ) );

	// Revoke a key
	CertStore_AddKeyRevocation( ca_dota_revoked.m_nKeyID );

	CMsgSteamDatagramCertificateSigned msgCertSigned;
	CMsgSteamDatagramCertificate msgCert;
	const CertAuthScope *pCertScope;

	//
	// The app-installed self-signed root is trusted, and coexists with
	// the hardcoded root CA key.  The one installed without app trust
	// is not.
	//
	CHECK( CertStore_CheckPublicKey( root_app.m_nKeyID, k_timeNow, errMsg ) != nullptr );
	CHECK( CertStore_CheckPublicKey( root_network.m_nKeyID, k_timeNow, errMsg ) == nullptr );

	//
	// Basic check for an identity cert issued by an intermediary.
	//
	GenerateCert( msgCertSigned, "app_ids: 730 identity_string: \"str:Hercule Poirot\"", ca_csgo );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( pCertScope );
	CHECK( CheckCertAppID( msgCert, pCertScope, 730, errMsg ) );

	// Shouldn't work for wrong app
	CHECK( !CheckCertAppID( msgCert, pCertScope, 570, errMsg ) );

	// Should work for any POPID
	CHECK( CheckCertPOPID( msgCert, pCertScope, iad, errMsg ) );
	CHECK( CheckCertPOPID( msgCert, pCertScope, sto, errMsg ) );

	//
	// Try to use CSGO CA cert to authorize for Dota
	//
	GenerateCert( msgCertSigned, "app_ids: 570 identity_string: \"str:Hercule Poirot\"", ca_csgo );

	// Signature should check out here.
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( pCertScope );

	// But app check should fail
	CHECK( !CheckCertAppID( msgCert, pCertScope, 570, errMsg ) );

	//
	// Cert for data center, signed directly by global app intermediary,
	// with the POP restriction in the issued cert
	//
	V_sprintf_safe( fields, "app_ids: 730 gameserver_datacenter_ids: %u", iad );
	GenerateCert( msgCertSigned, fields, ca_csgo );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( pCertScope );
	CHECK( CheckCertAppID( msgCert, pCertScope, 730, errMsg ) );

	// Should only work for the authorized POP
	CHECK( CheckCertPOPID( msgCert, pCertScope, iad, errMsg ) );
	CHECK( !CheckCertPOPID( msgCert, pCertScope, sto, errMsg ) );

	//
	// Cert for data center, signed by app that is further restricted by POPID
	//
	V_sprintf_safe( fields, "app_ids: 730 gameserver_datacenter_ids: %u gameserver_datacenter_ids: %u", iad, mwh );
	GenerateCert( msgCertSigned, fields, ca_csgo_eatmwh );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( pCertScope );
	CHECK( CheckCertAppID( msgCert, pCertScope, 730, errMsg ) );
	CHECK( !CheckCertPOPID( msgCert, pCertScope, iad, errMsg ) ); // Not in CA chain
	CHECK( CheckCertPOPID( msgCert, pCertScope, mwh, errMsg ) ); // In both CA chain and cert
	CHECK( !CheckCertPOPID( msgCert, pCertScope, eat, errMsg ) ); // In CA chain but not cert

	//
	// Cert with no POP restriction of its own, signed by a POP-restricted
	// chain: the chain restriction must apply
	//
	GenerateCert( msgCertSigned, "app_ids: 730", ca_csgo_eatmwh );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( pCertScope );
	CHECK( CheckCertPOPID( msgCert, pCertScope, eat, errMsg ) ); // Granted by chain
	CHECK( !CheckCertPOPID( msgCert, pCertScope, iad, errMsg ) ); // Not granted by chain

	//
	// Try to use a cert that chains to a root that was never installed
	//
	GenerateCert( msgCertSigned, "app_ids: 440 identity_string: \"str:Hercule Poirot\"", root_absent );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( !pCertScope );

	//
	// Try to use a cert that chains to the self-signed root that was
	// added without app trust
	//
	GenerateCert( msgCertSigned, "app_ids: 440 identity_string: \"str:Hercule Poirot\"", ca_tf2 );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( !pCertScope );

	//
	// Try to use a cert signed by a revoked key
	//
	GenerateCert( msgCertSigned, "app_ids: 570 identity_string: \"str:Hercule Poirot\"", ca_dota_revoked );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( !pCertScope );

	//
	// Re-adding the same self-signed root through the app-trusted path
	// upgrades it, and its chain starts to verify.  (This also exercises
	// the raw-base64 input form.)
	//
	CHECK( CertStore_AddTrustedCertFromPEM( root_network.m_sCertBase64.c_str(), errMsg ) );
	CHECK( CertStore_CheckPublicKey( root_network.m_nKeyID, k_timeNow, errMsg ) != nullptr );
	GenerateCert( msgCertSigned, "app_ids: 440 identity_string: \"str:Hercule Poirot\"", ca_tf2 );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeNow, errMsg );
	CHECK( pCertScope );
	CHECK( CheckCertAppID( msgCert, pCertScope, 440, errMsg ) );

	if ( g_failed )
	{
		fprintf( stderr, "test_pki FAILED\n" );
		return 1;
	}
	printf( "test_pki passed\n" );
	return 0;
}
