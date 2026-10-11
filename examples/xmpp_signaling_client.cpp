//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Client for XMPP (Jabber) signaling service (RFC 6120 / RFC 6121).
// Serves as an example of how to hook up an XMPP signaling server
// to SteamNetworkingSockets P2P connections (Issue #136).
//
//=============================================================================

#include "xmpp_signaling_client.h"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <mutex>
#include <deque>
#include <utility>

#ifndef _WIN32
	#include <unistd.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <netinet/in.h>
	#include <netdb.h>
	#include <sys/ioctl.h>
	#include <poll.h>
	#include <errno.h>
	typedef int SOCKET;
	constexpr SOCKET INVALID_SOCKET = -1;
	inline void closesocket( SOCKET s ) { close( s ); }
	inline int GetSocketError() { return errno; }
	inline bool IgnoreSocketError( int e )
	{
		return e == EAGAIN || e == ENOTCONN || e == EWOULDBLOCK || e == EINPROGRESS;
	}
	#ifndef ioctlsocket
		#define ioctlsocket ioctl
	#endif
	struct ScopedWinsockInit_t {};
#else
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef int socklen_t;
	inline int GetSocketError() { return WSAGetLastError(); }
	inline bool IgnoreSocketError( int e )
	{
		return e == WSAEWOULDBLOCK || e == WSAENOTCONN || e == WSAEINPROGRESS;
	}
	#ifndef poll
		#define poll WSAPoll
	#endif
	struct ScopedWinsockInit_t
	{
		ScopedWinsockInit_t()
		{
			WSADATA wsaData;
			WSAStartup( MAKEWORD( 2, 2 ), &wsaData );
		}
		~ScopedWinsockInit_t()
		{
			WSACleanup();
		}
	};
#endif

static bool SplitHostPort( const char *pszAddress, std::string &sHost, std::string &sPort, const char *pszDefaultPort )
{
	if ( pszAddress == nullptr || pszAddress[0] == '\0' )
	{
		return false;
	}

	std::string sAddr( pszAddress );
	size_t colon = sAddr.rfind( ':' );
	if ( colon == std::string::npos )
	{
		sHost = sAddr;
		sPort = pszDefaultPort ? pszDefaultPort : "";
	}
	else
	{
		sHost = sAddr.substr( 0, colon );
		sPort = sAddr.substr( colon + 1 );
	}
	return !sHost.empty();
}

#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
	#include <openssl/ssl.h>
	#include <openssl/err.h>
#endif

static const char s_chBase64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string XmppBase64Encode( const void *pData, int cbData )
{
	std::string sOut;
	if ( cbData <= 0 || pData == nullptr )
	{
		return sOut;
	}

	const uint8_t *pBytes = (const uint8_t *)pData;
	sOut.reserve( ( ( cbData + 2 ) / 3 ) * 4 );

	for ( int i = 0; i < cbData; i += 3 )
	{
		uint32_t nVal = ( (uint32_t)pBytes[i] ) << 16;
		if ( i + 1 < cbData )
		{
			nVal |= ( (uint32_t)pBytes[i + 1] ) << 8;
		}
		if ( i + 2 < cbData )
		{
			nVal |= (uint32_t)pBytes[i + 2];
		}

		sOut.push_back( s_chBase64Table[ ( nVal >> 18 ) & 0x3F ] );
		sOut.push_back( s_chBase64Table[ ( nVal >> 12 ) & 0x3F ] );
		sOut.push_back( ( i + 1 < cbData ) ? s_chBase64Table[ ( nVal >> 6 ) & 0x3F ] : '=' );
		sOut.push_back( ( i + 2 < cbData ) ? s_chBase64Table[ nVal & 0x3F ] : '=' );
	}

	return sOut;
}

bool XmppBase64Decode( const char *pszInput, int cchInput, std::string &sOutDecoded )
{
	sOutDecoded.clear();
	if ( cchInput <= 0 || pszInput == nullptr )
	{
		return true;
	}

	auto decodeChar = []( char c ) -> int
	{
		if ( c >= 'A' && c <= 'Z' ) return c - 'A';
		if ( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
		if ( c >= '0' && c <= '9' ) return c - '0' + 52;
		if ( c == '+' ) return 62;
		if ( c == '/' ) return 63;
		if ( c == '=' ) return -1;
		return -2;
	};

	sOutDecoded.reserve( ( cchInput / 4 ) * 3 );
	int nBlock[4];
	int nBlockLen = 0;

	for ( int i = 0; i < cchInput; ++i )
	{
		char c = pszInput[i];
		if ( c == ' ' || c == '\t' || c == '\r' || c == '\n' )
		{
			continue;
		}
		int val = decodeChar( c );
		if ( val == -2 )
		{
			return false;
		}
		nBlock[ nBlockLen++ ] = val;
		if ( nBlockLen == 4 )
		{
			if ( nBlock[0] < 0 || nBlock[1] < 0 )
			{
				return false;
			}
			uint32_t nVal = ( (uint32_t)nBlock[0] << 18 ) | ( (uint32_t)nBlock[1] << 12 );
			sOutDecoded.push_back( (char)( ( nVal >> 16 ) & 0xFF ) );

			if ( nBlock[2] >= 0 )
			{
				nVal |= ( (uint32_t)nBlock[2] << 6 );
				sOutDecoded.push_back( (char)( ( nVal >> 8 ) & 0xFF ) );

				if ( nBlock[3] >= 0 )
				{
					nVal |= (uint32_t)nBlock[3];
					sOutDecoded.push_back( (char)( nVal & 0xFF ) );
				}
			}
			nBlockLen = 0;
		}
	}

	return ( nBlockLen == 0 );
}

std::string XmppFormatMessageStanza( const char *pszFrom, const char *pszTo, const void *pPayload, int cbPayload )
{
	std::string sStanza;
	sStanza.reserve( 128 + ( cbPayload * 4 ) / 3 );
	sStanza = "<message to='";
	sStanza += pszTo;
	sStanza += "' from='";
	sStanza += pszFrom;
	sStanza += "' type='chat'><body>GNS:";
	sStanza += XmppBase64Encode( pPayload, cbPayload );
	sStanza += "</body></message>\n";
	return sStanza;
}

bool XmppExtractMessage( std::string &sBuffer, std::string &sOutFrom, std::string &sOutPayload )
{
	size_t posMsg = sBuffer.find( "<message" );
	if ( posMsg == std::string::npos )
	{
		if ( sBuffer.length() > 4096 && sBuffer.find( '<' ) == std::string::npos )
		{
			sBuffer.clear();
		}
		return false;
	}

	size_t posEndMsg = sBuffer.find( "</message>", posMsg );
	if ( posEndMsg == std::string::npos )
	{
		return false;
	}

	std::string sStanza = sBuffer.substr( posMsg, posEndMsg + 10 - posMsg );
	sBuffer.erase( 0, posEndMsg + 10 );

	size_t posFrom = sStanza.find( "from=" );
	if ( posFrom != std::string::npos && posFrom + 6 < sStanza.length() )
	{
		char quote = sStanza[ posFrom + 5 ];
		if ( quote == '\'' || quote == '\"' )
		{
			size_t posEndQuote = sStanza.find( quote, posFrom + 6 );
			if ( posEndQuote != std::string::npos )
			{
				sOutFrom = sStanza.substr( posFrom + 6, posEndQuote - ( posFrom + 6 ) );
			}
		}
	}

	size_t posBody = sStanza.find( "<body>" );
	size_t posEndBody = sStanza.find( "</body>" );
	if ( posBody != std::string::npos && posEndBody != std::string::npos && posEndBody > posBody + 6 )
	{
		std::string sBody = sStanza.substr( posBody + 6, posEndBody - ( posBody + 6 ) );
		const char *pszPrefix = "GNS:";
		size_t prefixLen = strlen( pszPrefix );
		if ( sBody.compare( 0, prefixLen, pszPrefix ) == 0 )
		{
			std::string sBase64 = sBody.substr( prefixLen );
			if ( XmppBase64Decode( sBase64.c_str(), (int)sBase64.length(), sOutPayload ) )
			{
				return true;
			}
		}
	}

	return false;
}

/// Implementation of IXmppSignalingClient
class CXmppSignalingClient : public IXmppSignalingClient
{
	struct ConnectionSignaling : ISteamNetworkingConnectionSignaling
	{
		CXmppSignalingClient *const m_pOwner;
		std::string const m_sPeerJid;

		ConnectionSignaling( CXmppSignalingClient *pOwner, const char *pszPeerJid )
		: m_pOwner( pOwner )
		, m_sPeerJid( pszPeerJid )
		{
		}

		virtual ~ConnectionSignaling() {}

		virtual bool SendSignal( HSteamNetConnection hConn, const SteamNetConnectionInfo_t &info, const void *pMsg, int cbMsg ) override
		{
			(void)info;
			(void)hConn;
			return m_pOwner->SendSignalToPeer( m_sPeerJid.c_str(), pMsg, cbMsg );
		}

		virtual void Release() override
		{
			delete this;
		}
	};

	sockaddr_storage m_adrServer;
	size_t const m_adrServerSize;
	ISteamNetworkingSockets *const m_pSteamNetworkingSockets;
	std::string m_sJid;
	std::string m_sPassword;
	bool m_bUseTLS;

	std::recursive_mutex m_sockMutex;
	SOCKET m_sock;
#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
	SSL_CTX *m_pSSLCtx;
	SSL *m_pSSL;
#endif
	std::deque< std::string > m_queueSend;
	std::string m_sBufferedData;
	bool m_bConnected;
	bool m_bAuthSent;

	void SendAuthStanza()
	{
		if ( m_bAuthSent )
		{
			return;
		}
		m_bAuthSent = true;

		size_t at = m_sJid.find( '@' );
		std::string sUser = ( at != std::string::npos ) ? m_sJid.substr( 0, at ) : m_sJid;

		std::string sPlain;
		sPlain.push_back( '\0' );
		sPlain.append( sUser );
		sPlain.push_back( '\0' );
		sPlain.append( m_sPassword );

		std::string sB64 = XmppBase64Encode( sPlain.data(), (int)sPlain.size() );
		std::string sAuth = "<auth xmlns='urn:ietf:params:xml:ns:xmpp-sasl' mechanism='PLAIN'>" + sB64 + "</auth>\n";
		SendRaw( sAuth );
		SendRaw( "<presence/>\n" );
	}

public:
	CXmppSignalingClient(
		const sockaddr *pAdrServer,
		size_t adrServerSize,
		const char *pszJid,
		const char *pszPassword,
		bool bUseTLS,
		ISteamNetworkingSockets *pSteamNetworkingSockets
	)
	: m_adrServerSize( adrServerSize )
	, m_pSteamNetworkingSockets( pSteamNetworkingSockets )
	, m_sJid( pszJid )
	, m_sPassword( pszPassword )
	, m_bUseTLS( bUseTLS )
	, m_sock( INVALID_SOCKET )
#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
	, m_pSSLCtx( nullptr )
	, m_pSSL( nullptr )
#endif
	, m_bConnected( false )
	, m_bAuthSent( false )
	{
		memcpy( &m_adrServer, pAdrServer, adrServerSize );
		Connect();
	}

	virtual ~CXmppSignalingClient()
	{
		CloseSocket();
	}

	void CloseSocket()
	{
		std::lock_guard< std::recursive_mutex > lock( m_sockMutex );
#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
		if ( m_pSSL != nullptr )
		{
			SSL_shutdown( m_pSSL );
			SSL_free( m_pSSL );
			m_pSSL = nullptr;
		}
		if ( m_pSSLCtx != nullptr )
		{
			SSL_CTX_free( m_pSSLCtx );
			m_pSSLCtx = nullptr;
		}
#endif
		if ( m_sock != INVALID_SOCKET )
		{
			closesocket( m_sock );
			m_sock = INVALID_SOCKET;
		}
		m_bConnected = false;
		m_bAuthSent = false;
		m_sBufferedData.clear();
		m_queueSend.clear();
	}

	void Connect()
	{
		std::lock_guard< std::recursive_mutex > lock( m_sockMutex );
		CloseSocket();

		int sockType = SOCK_STREAM;
		#ifdef LINUX
			sockType |= SOCK_CLOEXEC;
		#endif
		m_sock = socket( m_adrServer.ss_family, sockType, IPPROTO_TCP );
		if ( m_sock == INVALID_SOCKET )
		{
			return;
		}

		unsigned long opt = 1;
		if ( ioctlsocket( m_sock, FIONBIO, &opt ) == -1 )
		{
			CloseSocket();
			return;
		}

		connect( m_sock, (const sockaddr *)&m_adrServer, (socklen_t)m_adrServerSize );

		if ( m_bUseTLS )
		{
#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
			m_pSSLCtx = SSL_CTX_new( TLS_client_method() );
			if ( m_pSSLCtx != nullptr )
			{
				m_pSSL = SSL_new( m_pSSLCtx );
				if ( m_pSSL != nullptr )
				{
					SSL_set_fd( m_pSSL, (int)m_sock );
					SSL_set_connect_state( m_pSSL );
				}
			}
#else
			CloseSocket();
			return;
#endif
		}

		std::string sStreamOpen = "<?xml version='1.0'?><stream:stream to='";
		size_t at = m_sJid.find( '@' );
		std::string sDomain = ( at != std::string::npos ) ? m_sJid.substr( at + 1 ) : "localhost";
		size_t slash = sDomain.find( '/' );
		if ( slash != std::string::npos )
		{
			sDomain.erase( slash );
		}
		sStreamOpen += sDomain;
		sStreamOpen += "' xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams' version='1.0'>\n";
		SendRaw( sStreamOpen );
	}

	void SendRaw( const std::string &s )
	{
		std::lock_guard< std::recursive_mutex > lock( m_sockMutex );
		while ( m_queueSend.size() > 64 )
		{
			m_queueSend.pop_front();
		}
		m_queueSend.push_back( s );
	}

	bool SendSignalToPeer( const char *pszPeerJid, const void *pMsg, int cbMsg )
	{
		std::string sStanza = XmppFormatMessageStanza( m_sJid.c_str(), pszPeerJid, pMsg, cbMsg );
		SendRaw( sStanza );
		return true;
	}

	virtual ISteamNetworkingConnectionSignaling *CreateSignalingForConnection(
		const SteamNetworkingIdentity &identityPeer,
		SteamNetworkingErrMsg &errMsg
	) override
	{
		SteamNetworkingIdentityRender sIdentity( identityPeer );
		const char *pszPeer = sIdentity.c_str();
		if ( pszPeer[0] == '\0' )
		{
			sprintf( errMsg, "Cannot create signaling without peer identity" );
			return nullptr;
		}
		return new ConnectionSignaling( this, pszPeer );
	}

	virtual void Poll() override
	{
		std::deque< std::pair< std::string, std::string > > messagesToDispatch;

		{
			std::lock_guard< std::recursive_mutex > lock( m_sockMutex );
			if ( m_sock == INVALID_SOCKET )
			{
				return;
			}

			struct pollfd pfd;
			pfd.fd = m_sock;
			pfd.events = POLLIN;
			if ( !m_queueSend.empty() || ( m_bUseTLS && !m_bConnected ) )
			{
				pfd.events |= POLLOUT;
			}
			pfd.revents = 0;

			#ifdef _WIN32
				int pr = WSAPoll( &pfd, 1, 0 );
			#else
				int pr = poll( &pfd, 1, 0 );
			#endif

			if ( pr < 0 )
			{
				if ( !IgnoreSocketError( GetSocketError() ) )
				{
					CloseSocket();
					return;
				}
			}

			if ( !m_bConnected )
			{
#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
				if ( m_bUseTLS && m_pSSL != nullptr )
				{
					int r = SSL_connect( m_pSSL );
					if ( r == 1 )
					{
						m_bConnected = true;
						SendAuthStanza();
					}
					else
					{
						int err = SSL_get_error( m_pSSL, r );
						if ( err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE )
						{
							CloseSocket();
							return;
						}
					}
				}
				else
#endif
				{
					if ( pfd.revents & ( POLLOUT | POLLIN ) )
					{
						int optval = 0;
						socklen_t optlen = sizeof( optval );
						getsockopt( m_sock, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen );
						if ( optval == 0 )
						{
							m_bConnected = true;
							SendAuthStanza();
						}
						else
						{
							CloseSocket();
							return;
						}
					}
				}
			}

			if ( m_bConnected )
			{
				char buf[1024];
				for (;;)
				{
					int r = 0;
#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
					if ( m_bUseTLS && m_pSSL != nullptr )
					{
						r = SSL_read( m_pSSL, buf, sizeof( buf ) );
						if ( r <= 0 )
						{
							int err = SSL_get_error( m_pSSL, r );
							if ( err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE )
							{
								break;
							}
							CloseSocket();
							break;
						}
					}
					else
#endif
					{
						r = recv( m_sock, buf, sizeof( buf ), 0 );
						if ( r <= 0 )
						{
							if ( r < 0 && IgnoreSocketError( GetSocketError() ) )
							{
								break;
							}
							CloseSocket();
							break;
						}
					}

					m_sBufferedData.append( buf, r );
					if ( r < (int)sizeof( buf ) )
					{
						break;
					}
				}

				while ( !m_queueSend.empty() && m_sock != INVALID_SOCKET )
				{
					const std::string &s = m_queueSend.front();
					int l = (int)s.length();
					int r = 0;
#if defined(XMPP_HAS_OPENSSL) && XMPP_HAS_OPENSSL
					if ( m_bUseTLS && m_pSSL != nullptr )
					{
						r = SSL_write( m_pSSL, s.c_str(), l );
						if ( r <= 0 )
						{
							int err = SSL_get_error( m_pSSL, r );
							if ( err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE )
							{
								break;
							}
							CloseSocket();
							break;
						}
					}
					else
#endif
					{
						r = ::send( m_sock, s.c_str(), l, 0 );
						if ( r <= 0 )
						{
							if ( r < 0 && IgnoreSocketError( GetSocketError() ) )
							{
								break;
							}
							CloseSocket();
							break;
						}
					}

					if ( r == l )
					{
						m_queueSend.pop_front();
					}
					else
					{
						CloseSocket();
						break;
					}
				}

				std::string sFrom, sPayload;
				while ( XmppExtractMessage( m_sBufferedData, sFrom, sPayload ) )
				{
					messagesToDispatch.push_back( std::make_pair( sFrom, sPayload ) );
				}
			}
		} // Unlock m_sockMutex!

		for ( const auto &msg : messagesToDispatch )
		{
			const std::string &sPeerFrom = msg.first;
			const std::string &sPayload = msg.second;

			struct Context : ISteamNetworkingSignalingRecvContext
			{
				CXmppSignalingClient *m_pOwner;
				std::string m_sPeerJid;

				virtual ISteamNetworkingConnectionSignaling *OnConnectRequest(
					HSteamNetConnection hConn,
					const SteamNetworkingIdentity &identityPeer,
					int nLocalVirtualPort
				) override
				{
					(void)hConn;
					(void)nLocalVirtualPort;
					SteamNetworkingErrMsg errMsg;
					SteamNetworkingIdentity identityExpected;
					identityExpected.SetGenericString( m_sPeerJid.c_str() );
					return m_pOwner->CreateSignalingForConnection( identityPeer.IsInvalid() ? identityExpected : identityPeer, errMsg );
				}

				virtual void SendRejectionSignal(
					const SteamNetworkingIdentity &identityPeer,
					const void *pMsg,
					int cbMsg
				) override
				{
					(void)identityPeer;
					(void)pMsg;
					(void)cbMsg;
				}
			};

			Context context;
			context.m_pOwner = this;
			context.m_sPeerJid = sPeerFrom;

			m_pSteamNetworkingSockets->ReceivedP2PCustomSignal(
				sPayload.data(),
				(int)sPayload.size(),
				&context
			);
		}
	}

	virtual void Release() override
	{
		CloseSocket();
		delete this;
	}
};

IXmppSignalingClient *CreateXmppSignalingClient(
	const char *pszServerAddress,
	const char *pszJid,
	const char *pszPassword,
	bool bUseTLS,
	ISteamNetworkingSockets *pSteamNetworkingSockets,
	SteamNetworkingErrMsg &errMsg
)
{
#if !defined(XMPP_HAS_OPENSSL) || !XMPP_HAS_OPENSSL
	if ( bUseTLS )
	{
		sprintf( errMsg, "TLS requested but XMPP signaling client was built without OpenSSL support." );
		return nullptr;
	}
#endif
	ScopedWinsockInit_t winsock;
	(void)winsock;
	std::string sAddress, sPort;
	if ( !SplitHostPort( pszServerAddress, sAddress, sPort, bUseTLS ? "5223" : "5222" ) )
	{
		sprintf( errMsg, "Invalid XMPP server address '%s'", pszServerAddress ? pszServerAddress : "" );
		return nullptr;
	}

	addrinfo hints;
	memset( &hints, 0, sizeof( hints ) );
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo *pAddrInfo = nullptr;
	int r = getaddrinfo( sAddress.c_str(), sPort.c_str(), &hints, &pAddrInfo );
	if ( r != 0 || pAddrInfo == nullptr )
	{
		sprintf( errMsg, "Invalid XMPP server address '%s:%s', getaddrinfo returned %d", sAddress.c_str(), sPort.c_str(), r );
		return nullptr;
	}

	CXmppSignalingClient *pClient = new CXmppSignalingClient(
		pAddrInfo->ai_addr,
		pAddrInfo->ai_addrlen,
		pszJid,
		pszPassword,
		bUseTLS,
		pSteamNetworkingSockets
	);

	freeaddrinfo( pAddrInfo );
	return pClient;
}
