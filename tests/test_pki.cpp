#include <assert.h>
#include <time.h>
#include <string>

#include <steamnetworkingsockets/steamnetworkingsockets_certstore.h>
#include <google/protobuf/text_format.h>
#include <common/crypto.h>
#include <common/crypto_25519.h>

using namespace SteamNetworkingSocketsLib;

// Time as I write this.  So that the tests will still work even after these generated keys expire.
time_t k_timeTestNow = 1555374048;

void GenerateCert( CMsgSteamDatagramCertificateSigned &msgOut, const char *certData, const CECSigningPrivateKey &keyCAPrivateKey, uint64 nCAKeyID )
{
	msgOut.Clear();

	// Generate a dummy cert with the requested fields and give it a keypair
	{
		CMsgSteamDatagramCertificate msgCert;
		DbgVerify( google::protobuf::TextFormat::ParseFromString( std::string( certData ), &msgCert ) );

		msgCert.set_time_expiry( k_timeTestNow + 3600*8 );

		CECSigningPrivateKey tempIdentityPrivateKey;
		CECSigningPublicKey tempIdentityPublicKey;
		CCrypto::GenerateSigningKeyPair( &tempIdentityPublicKey, &tempIdentityPrivateKey );
		DbgVerify( tempIdentityPublicKey.GetRawDataAsStdString( msgCert.mutable_key_data() ) );
		msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );

		DbgVerify( msgCert.SerializeToString( msgOut.mutable_cert() ) );
	}

	// Sign it
	CryptoSignature_t sig;
	keyCAPrivateKey.GenerateSignature( msgOut.cert().c_str(), msgOut.cert().length(), &sig );
	msgOut.set_ca_key_id( nCAKeyID );
	msgOut.set_ca_signature( &sig, sizeof(sig) );
}

int main()
{
	SteamNetworkingErrMsg errMsg;

	//
	// Populate our cert store with some certs.
	// See make_test_certs.py
	//

	// Dynamic (not-hardcoded) self-signed cert
	// KeyID . . . .: 8112647883641536425
	// Public key . : ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBkU/enzJscDJp0N1RbYkL0E9wXVO5krNr8rm4JDrNBE 
	CECSigningPrivateKey privkey_dynamic_root;
	DbgVerify( privkey_dynamic_root.ParsePEM( "-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZWQy NTUxOQAAACAZFP3p8ybHAyadDdUW2JC9BPcF1TuZKza/K5uCQ6zQRAAAAH8SNFZ4EjRWeAAA AAtzc2gtZWQyNTUxOQAAACAZFP3p8ybHAyadDdUW2JC9BPcF1TuZKza/K5uCQ6zQRAAAAEDq vSVEpg9EZkMej6Fw1EFCuiAnNtMCTKmf8ZRXSwzrXRkU/enzJscDJp0N1RbYkL0E9wXVO5kr Nr8rm4JDrNBE -----END OPENSSH PRIVATE KEY----- ", 375 ) );
	// CA KeyID . . : 8112647883641536425
	const uint64 k_key_dynamic_root = 8112647883641536425ull;
	DbgVerify( CertStore_AddCertFromBase64( "Ii4IARIgGRT96fMmxwMmnQ3VFtiQvQT3BdU7mSs2vyubgkOs0ERFmSm1XE2ZkHdgKak/R3xE6pVwMkBlDV+UgOQHEwEg5GnlKLxK5aqKAWl8J0Eo2pl6+grtk5fitu9U15EXtkHhw1o7q8+sZFvRJw8/zXuohkzVB1AC", errMsg ) );

	// Intermediate cert for app (CSGO), signed by hardcoded key
	// KeyID . . . .: 1790264268120135407
	// Public key . : ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAILumwWENaKq+n5xzAvfLgOOaeLvQqky4LzU0HI0qBnU/ 
	CECSigningPrivateKey privkey_csgo;
	DbgVerify( privkey_csgo.ParsePEM( "-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZWQy NTUxOQAAACC7psFhDWiqvp+ccwL3y4Djmni70KpMuC81NByNKgZ1PwAAAH8SNFZ4EjRWeAAA AAtzc2gtZWQyNTUxOQAAACC7psFhDWiqvp+ccwL3y4Djmni70KpMuC81NByNKgZ1PwAAAEAs mu57b1o/lDSwUKD4LvIM/kQMwFIbzEbFIoyuyDEf3bumwWENaKq+n5xzAvfLgOOaeLvQqky4 LzU0HI0qBnU/ -----END OPENSSH PRIVATE KEY----- ", 375 ) );
	// CA KeyID . . : 9417917822780561193
	// Apps . . . . : [730]
	const uint64 k_key_csgo = 1790264268120135407ull;
	DbgVerify( CertStore_AddCertFromBase64( "IjEIARIgu6bBYQ1oqr6fnHMC98uA45p4u9CqTLgvNTQcjSoGdT9FmSm1XE2ZkHdgUNoFKSnXp45cKrOCMkCPs0eTzHWsN0oDNrxnAvvi3MiDv6Tv4CudquT4D/nss3usW6xUPD3YIbbISWxL8YE1HGYVRILCYWDCqxoBOK4M", errMsg ) );

	// Cert for particular data center.  Not specifically scoped to app, but signed by CSGO cert, so should effectively be scoped.
	// KeyID . . . .: 10851291850214533835
	// Public key . : ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIKd+8wfN8OYAQ+P4fdiC+7xwakeqOlDSqKY5/9wtkUim 
	CECSigningPrivateKey privkey_csgo_eatmwh;
	DbgVerify( privkey_csgo_eatmwh.ParsePEM( "-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZWQy NTUxOQAAACCnfvMHzfDmAEPj+H3Ygvu8cGpHqjpQ0qimOf/cLZFIpgAAAH8SNFZ4EjRWeAAA AAtzc2gtZWQyNTUxOQAAACCnfvMHzfDmAEPj+H3Ygvu8cGpHqjpQ0qimOf/cLZFIpgAAAEA0 pWdXwJgrvazaE/69qtE0zsjQJfzshriDJxfC467ktqd+8wfN8OYAQ+P4fdiC+7xwakeqOlDS qKY5/9wtkUim -----END OPENSSH PRIVATE KEY----- ", 375 ) );
	// CA KeyID . . : 1790264268120135407
	// POPs . . . . : [u'eat', u'mwh']
	const uint64 k_key_csgo_eatmwh = 10851291850214533835ull;
	DbgVerify( CertStore_AddCertFromBase64( "IjgIARIgp37zB83w5gBD4/h92IL7vHBqR6o6UNKopjn/3C2RSKYtdGFlAC1od20ARZkptVxNmZB3YCnvFmDb3UvYGDJAYXfYn+ofbs5Fz4EYiMYNh4SFD302+S/xXsAzmk8awH7nuasCV+RUWjoOshkKMK6ONCYzmkMiD0so7tOR+7zsDQ==", errMsg ) );

	// Intermediate cert for app (TF2), signed by self-signed cert
	// KeyID . . . .: 12206663272037732248
	// Public key . : ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC/nkdg+La27cA2ptQj1t0buCYoo2OAQI+lf2P/QaRq4 
	CECSigningPrivateKey privkey_tf2;
	DbgVerify( privkey_tf2.ParsePEM( "-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZWQy NTUxOQAAACAv55HYPi2tu3ANqbUI9bdG7gmKKNjgECPpX9j/0GkauAAAAH8SNFZ4EjRWeAAA AAtzc2gtZWQyNTUxOQAAACAv55HYPi2tu3ANqbUI9bdG7gmKKNjgECPpX9j/0GkauAAAAEDf 8k3ME+Xapo2rNSUTO7SLog3hNCGP4cWcvM4bnEBkwC/nkdg+La27cA2ptQj1t0buCYoo2OAQ I+lf2P/QaRq4 -----END OPENSSH PRIVATE KEY----- ", 375 ) );
	// CA KeyID . . : 8112647883641536425
	// Apps . . . . : [440]
	const uint64 k_key_tf2 = 12206663272037732248ull;
	DbgVerify( CertStore_AddCertFromBase64( "IjEIARIgL+eR2D4trbtwDam1CPW3Ru4JiijY4BAj6V/Y/9BpGrhFmSm1XE2ZkHdgULgDKak/R3xE6pVwMkBCdDdDrAn6IkpuRwksFtXHUTgJNtColLLNPdoEhfyg/Fb5EDnTcOmaNzfoJbv2aFGmjPv2CUzYg+G8qKJv09wN", errMsg ) );

	// Another intermediate cert signed by hardcoded root
	// KeyID . . . .: 15210429824691730624
	// Public key . : ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIO+dnkgm1SI2UAMbGkrotrHeTe30Mu4mhne9s7kb+knI 
	CECSigningPrivateKey privkey_dota_revoked;
	DbgVerify( privkey_dota_revoked.ParsePEM( "-----BEGIN OPENSSH PRIVATE KEY----- b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZWQy NTUxOQAAACDvnZ5IJtUiNlADGxpK6Lax3k3t9DLuJoZ3vbO5G/pJyAAAAH8SNFZ4EjRWeAAA AAtzc2gtZWQyNTUxOQAAACDvnZ5IJtUiNlADGxpK6Lax3k3t9DLuJoZ3vbO5G/pJyAAAAEB8 CNRDPePmSmm66c7QyeOSiQyMHtrcouvxqzNq6GnRz++dnkgm1SI2UAMbGkrotrHeTe30Mu4m hne9s7kb+knI -----END OPENSSH PRIVATE KEY----- ", 375 ) );
	// CA KeyID . . : 9417917822780561193
	// Apps . . . . : [570]
	const uint64 k_key_dota_revoked = 15210429824691730624ull;
	DbgVerify( CertStore_AddCertFromBase64( "IjEIARIg752eSCbVIjZQAxsaSui2sd5N7fQy7iaGd72zuRv6SchFmSm1XE2ZkHdgULoEKSnXp45cKrOCMkCzt988yidn25C8fBC47EyW35w6SA9GbhPx6CUVeI5h8c/GGHrE4d/Mwvm5t3gv37xUg/uSquFhqWuERmUO4xAP", errMsg ) );


	// Revoke a key
	CertStore_AddKeyRevocation( k_key_dota_revoked );

	CMsgSteamDatagramCertificateSigned msgCertSigned;
	CMsgSteamDatagramCertificate msgCert;
	const CertAuthScope *pCertScope;
	const SteamNetworkingPOPID iad = CalculateSteamNetworkingPOPIDFromString( "iad" );
	const SteamNetworkingPOPID sto = CalculateSteamNetworkingPOPIDFromString( "sto" ); // 7566447
	const SteamNetworkingPOPID mwh = CalculateSteamNetworkingPOPIDFromString( "mwh" );
	const SteamNetworkingPOPID eat = CalculateSteamNetworkingPOPIDFromString( "eat" );

	//
	// Basic check for an identity cert issued by an intermediary.
	//
	GenerateCert( msgCertSigned, "app_ids: 730 identity: { generic_string: \"Hercule Poirot\" }", privkey_csgo, k_key_csgo );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeTestNow, errMsg );
	Assert( pCertScope );
	DbgVerify( CheckCertAppID( msgCert, pCertScope, 730, errMsg ) );

	// Shouldn't work for wrong app
	DbgVerify( !CheckCertAppID( msgCert, pCertScope, 570, errMsg ) );

	// Should work for any POPID
	DbgVerify( CheckCertPOPID( msgCert, pCertScope, iad, errMsg ) );
	DbgVerify( CheckCertPOPID( msgCert, pCertScope, sto, errMsg ) );

	//
	// Try to use CSGO CA cert to authorize for Dota
	//
	GenerateCert( msgCertSigned, "app_ids: 570 identity: { generic_string: \"Hercule Poirot\" }", privkey_csgo, k_key_csgo );

	// Signature should check out here.
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeTestNow, errMsg );
	Assert( pCertScope );

	// But app check should fail
	DbgVerify( !CheckCertAppID( msgCert, pCertScope, 570, errMsg ) );

	//
	// Cert for data center, signed directly by global app intermediary,
	// with the POP restriction in the issued cert
	//
	Assert( iad == 6906212 );
	GenerateCert( msgCertSigned, "app_ids: 730 gameserver_datacenter_ids: 6906212", privkey_csgo, k_key_csgo );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeTestNow, errMsg );
	Assert( pCertScope );
	DbgVerify( CheckCertAppID( msgCert, pCertScope, 730, errMsg ) );

	// Should only work for the authorized POP
	DbgVerify( CheckCertPOPID( msgCert, pCertScope, iad, errMsg ) );
	DbgVerify( !CheckCertPOPID( msgCert, pCertScope, sto, errMsg ) );

	//
	// Cert for data center, signed by app that is further restricted by POPID
	//
	Assert( iad == 6906212 );
	Assert( mwh == 7173992 );
	GenerateCert( msgCertSigned, "app_ids: 730 gameserver_datacenter_ids: 6906212 gameserver_datacenter_ids: 7173992", privkey_csgo_eatmwh, k_key_csgo_eatmwh );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeTestNow, errMsg );
	Assert( pCertScope );
	DbgVerify( CheckCertAppID( msgCert, pCertScope, 730, errMsg ) );
	DbgVerify( !CheckCertPOPID( msgCert, pCertScope, iad, errMsg ) ); // Not in CA chain
	DbgVerify( CheckCertPOPID( msgCert, pCertScope, mwh, errMsg ) ); // In both CA chain and cert
	DbgVerify( !CheckCertPOPID( msgCert, pCertScope, eat, errMsg ) ); // In CA chain but not cert

	//
	// Try to use a cert where only cert is from root that isn't hardcoded
	//
	GenerateCert( msgCertSigned, "app_ids: 440 identity: { generic_string: \"Hercule Poirot\" }", privkey_tf2, k_key_tf2 );
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeTestNow, errMsg );
	Assert( !pCertScope );

	//
	// Try to use a cert signed by a revoked key
	//
	GenerateCert( msgCertSigned, "app_ids: 570 identity: { generic_string: \"Hercule Poirot\" }", privkey_dota_revoked, k_key_dota_revoked );

	// Should fail
	pCertScope = CertStore_CheckCert( msgCertSigned, msgCert, k_timeTestNow, errMsg );
	Assert( !pCertScope );

	return 0;
}
