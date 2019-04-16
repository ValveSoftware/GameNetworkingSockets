import re
import json
import subprocess

def run_cert_tool( args ):
    args = [ "../../../bin/win64/steamnetworkingsockets_certtool.exe", "--output-json" ] + args
    #print ' '.join( args )
    stdout = subprocess.check_output( args )
    return json.loads( stdout )

def create_keypair():
    return run_cert_tool( [ "gen_keypair" ] )

def compress_whitespace( s ):
    return re.sub( r'\s+', ' ', s )
def remove_all_whitespace( s ):
    return re.sub( r'\s+', '', s )

def get_pem_body( s ):
    m = re.match( r'-+.*BEGIN.*-\s+(.+)\s+.*END.*-+', s, re.DOTALL )
    body = m.group(1)
    return remove_all_whitespace( body )

def run_cert_tool_with_ca_key( ca_key, args ):
    args = [ '--ca-priv-key', compress_whitespace( ca_key['private_key'] ) ] + args
    return run_cert_tool( args )

def create_keypair_and_cert( ca_key, args ):
    args = args + [ 'gen_keypair', 'create_cert' ]
    return run_cert_tool_with_ca_key( ca_key, args )

def EmitKey( info, key_name, comment ):
    #print info
    print '// KeyID . . . .: ' + info['public_key_id']
    print '// Public key . : ' + info['public_key']
    priv_key_var = 'privkey_' + key_name;
    priv_key = compress_whitespace( info['private_key'] )
    print 'CECSigningPrivateKey %s;' % priv_key_var
    print 'DbgVerify( %s.ParsePEM( "%s", %d ) );' % ( priv_key_var, priv_key, len(priv_key) )

def EmitAddCert( cert, key_name, comment ):
    #print cert
    print '// ' + comment
    EmitKey( cert, key_name, comment )
    print '// CA KeyID . . : ' + cert['ca_key_id']
    if cert.get('app_ids'):
        print '// Apps . . . . : ' + str( cert['app_ids'] )
    if cert.get('pop_ids'):
        print '// POPs . . . . : ' + str( cert['pop_ids'] )
    print 'const uint64 k_key_' + key_name + ' = ' + cert['public_key_id'] + 'ull;'
    print 'DbgVerify( CertStore_AddCertFromBase64( "' + cert['cert'] + '", errMsg ) );'
    print

# Hardcoded root that matches the project #define
root1 = {
    "private_key" : "-----BEGIN OPENSSH PRIVATE KEY-----\r\nb3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZWQy\r\nNTUxOQAAACDjD7CQNxVGkErB5MmIVr3GLfb9128EQXFrlbmslhnStQAAAH8SNFZ4EjRWeAAA\r\nAAtzc2gtZWQyNTUxOQAAACDjD7CQNxVGkErB5MmIVr3GLfb9128EQXFrlbmslhnStQAAAECP\r\nodiZttz1v9cQKikYj69kVAjlKU7X78vBRreB9eJRJuMPsJA3FUaQSsHkyYhWvcYt9v3XbwRB\r\ncWuVuayWGdK1\r\n-----END OPENSSH PRIVATE KEY-----\r\n",
    "public_key" : "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMPsJA3FUaQSsHkyYhWvcYt9v3XbwRBcWuVuayWGdK1 ",
    "public_key_id" : "9417917822780561193"
}

# Another root key, this one not hardcoded.  Generate a self-signed cert
root2 = create_keypair()
root2cert = run_cert_tool_with_ca_key( root2, [ '--pub-key', root2['public_key'], "create_cert" ] )
root2cert['private_key'] = root2['private_key']
EmitAddCert( root2cert, 'dynamic_root', "Dynamic (not-hardcoded) self-signed cert" )

csgo = create_keypair_and_cert( root1, [ '--app', '730' ] )
EmitAddCert( csgo, 'csgo', "Intermediate cert for app (CSGO), signed by hardcoded key" )

csgo_eatmwh = create_keypair_and_cert( csgo, [ '--pop', 'eat,mwh' ] )
EmitAddCert( csgo_eatmwh, 'csgo_eatmwh', "Cert for particular data center.  Not specifically scoped to app, but signed by CSGO cert, so should effectively be scoped." )

tf2 = create_keypair_and_cert( root2, [ '--app', '440' ] )
EmitAddCert( tf2, 'tf2', "Intermediate cert for app (TF2), signed by self-signed cert" )

dota_revoked = create_keypair_and_cert( root1, [ '--app', '570' ] )
EmitAddCert( dota_revoked, 'dota_revoked', "Another intermediate cert signed by hardcoded root" )
