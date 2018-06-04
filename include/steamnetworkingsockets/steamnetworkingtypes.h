//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Purpose: misc networking utilities
//
//=============================================================================

#ifndef STEAMNETWORKINGTYPES
#define STEAMNETWORKINGTYPES
#ifdef _WIN32
#pragma once
#endif

#include "steamnetworkingsockets_config.h"
#include <steam/steamtypes.h>
#include <steam/steamclientpublic.h>

#if defined( VALVE_CALLBACK_PACK_SMALL )
#pragma pack( push, 4 )
#elif defined( VALVE_CALLBACK_PACK_LARGE )
#pragma pack( push, 8 )
#else
#error "Must define VALVE_CALLBACK_PACK_SMALL or VALVE_CALLBACK_PACK_LARGE"
#endif

struct SteamNetworkPingLocation_t;
struct SteamDatagramRelayAuthTicket;
struct SteamDatagramServiceNetID;
struct SteamNetConnectionStatusChangedCallback_t;
struct P2PSessionRequest_t;
struct P2PSessionConnectFail_t;

/// Handle used to identify a connection to a remote host.
typedef uint32 HSteamNetConnection;
const HSteamNetConnection k_HSteamNetConnection_Invalid = 0;

/// Handle used to identify a "listen socket".
typedef uint32 HSteamListenSocket;
const HSteamListenSocket k_HSteamListenSocket_Invalid = 0;

const int k_nSteamNetworkingSendFlags_NoNagle = 1;
const int k_nSteamNetworkingSendFlags_NoDelay = 2;
const int k_nSteamNetworkingSendFlags_Reliable = 8;

/// Different methods that messages can be sent
enum ESteamNetworkingSendType
{
	// Send an unreliable message. Can be lost.  Messages *can* be larger than a single MTU (UDP packet), but there is no
	// retransmission, so if any piece of the message is lost, the entire message will be dropped.
	//
	// The sending API does have some knowledge of the underlying connection, so if there is no NAT-traversal accomplished or
	// there is a recognized adjustment happening on the connection, the packet will be batched until the connection is open again.
	//
	// NOTE: By default, Nagle's algorithm is applied to all outbound packets.  This means that the message will NOT be sent
	//       immediately, in case further messages are sent soon after you send this, which can be grouped together.
	//       Any time there is enough buffered data to fill a packet, the packets will be pushed out immediately, but
	//       partially-full packets not be sent until the Nagle timer expires.
	//       See k_ESteamNetworkingSendType_UnreliableNoNagle, ISteamNetworkingSockets::FlushMessagesOnConnection,
	//       ISteamNetworkingP2P::FlushMessagesToUser
	//
	// This is not exactly the same as k_EP2PSendUnreliable!  You probably want k_ESteamNetworkingSendType_UnreliableNoNagle
	k_ESteamNetworkingSendType_Unreliable = 0,

	// Send a message unreliably, bypassing Nagle's algorithm for this message and any messages currently pending on the Nagle timer.
	// This is equivalent to using k_ESteamNetworkingSendType_Unreliable,
	// and then immediately flushing the messages using ISteamNetworkingSockets::FlushMessagesOnConnection or ISteamNetworkingP2P::FlushMessagesToUser.
	// (But this is more efficient.)
	k_ESteamNetworkingSendType_UnreliableNoNagle = k_nSteamNetworkingSendFlags_NoNagle,

	// Send an unreliable message, but do not buffer it if it cannot be sent relatively quickly.
	// This is useful for messages that are not useful if they are excessively delayed, such as voice data.
	// The Nagle algorithm is not used, and if the message is not dropped, any messages waiting on the Nagle timer
	// are immediately flushed.
	//
	// A message will be dropped under the following circumstances:
	// - the connection is not fully connected.  (E.g. the "Connecting" or "FindingRoute" states)
	// - there is a sufficiently large number of messages queued up already such that the current message
	//   will not be placed on the wire in the next ~200ms or so.
	//
	// if a message is dropped for these reasons, k_EResultIgnored will be returned.
	k_ESteamNetworkingSendType_UnreliableNoDelay = k_nSteamNetworkingSendFlags_NoDelay|k_nSteamNetworkingSendFlags_NoNagle,

	// Reliable message send. Can send up to 512kb of data in a single message. 
	// Does fragmentation/re-assembly of messages under the hood, as well as a sliding window for
	// efficient sends of large chunks of data.
	//
	// The Nagle algorithm is used.  See notes on k_ESteamNetworkingSendType_Unreliable for more details.
	// See k_ESteamNetworkingSendType_ReliableNoNagle, ISteamNetworkingSockets::FlushMessagesOnConnection,
	// ISteamNetworkingP2P::FlushMessagesToUser
	//
	// This is NOT the same as k_EP2PSendReliable, it's more like k_EP2PSendReliableWithBuffering
	k_ESteamNetworkingSendType_Reliable = k_nSteamNetworkingSendFlags_Reliable,

	// Send a message reliably, but bypass Nagle's algorithm.
	// See k_ESteamNetworkingSendType_UnreliableNoNagle for more info.
	//
	// This is equivalent to k_EP2PSendReliable
	k_ESteamNetworkingSendType_ReliableNoNagle = k_nSteamNetworkingSendFlags_Reliable|k_nSteamNetworkingSendFlags_NoNagle,
};

/// High level connection status
enum ESteamNetworkingConnectionState
{

	/// Dummy value used to indicate an error condition in the API.
	/// Specified connection doesn't exist or has already been closed.
	k_ESteamNetworkingConnectionState_None = 0,

	/// We are trying to establish whether peers can talk to each other,
	/// whether they WANT to talk to each other, perform basic auth,
	/// and exchange crypt keys.
	///
	/// - For connections on the "client" side (initiated locally):
	///   We're in the process of trying to establish a connection.
	///   Depending on the connection type, we might not know who they are.
	///   Note that it is not possible to tell if we are waiting on the
	///   network to complete handshake packets, or for the application layer
	///   to accept the connection.
	///
	/// - For connections on the "server" side (accepted through listen socket):
	///   We have completed some basic handshake and the client has presented
	///   some proof of identity.  The connection is ready to be accepted
	///   using AcceptConnection().
	///
	/// In either case, any unreliable packets sent now are almost certain
	/// to be dropped.  Attempts to receive packets are guaranteed to fail.
	/// You may send messages if the send mode allows for them to be queued.
	/// but if you close the connection before the connection is actually
	/// established, any queued messages will be discarded immediately.
	/// (We will not attempt to flush the queue and confirm delivery to the
	/// remote host, which ordinarily happens when a connection is closed.)
	k_ESteamNetworkingConnectionState_Connecting = 1,

	/// Some connection types use a back channel or trusted 3rd party
	/// for earliest communication.  If the server accepts the connection,
	/// then these connections switch into the rendezvous state.  During this
	/// state, we still have not yet established an end-to-end route (through
	/// the relay network), and so if you send any messages unreliable, they
	/// are going to be discarded.
	k_ESteamNetworkingConnectionState_FindingRoute = 2,

	/// We've received communications from our peer (and we know
	/// who they are) and are all good.  If you close the connection now,
	/// we will make our best effort to flush out any reliable sent data that
	/// has not been acknowledged by the peer.  (But note that this happens
	/// from within the application process, so unlike a TCP connection, you are
	/// not totally handing it off to the operating system to deal with it.)
	k_ESteamNetworkingConnectionState_Connected = 3,

	/// Connection has been closed by our peer, but not closed locally.
	/// The connection still exists from an API perspective.  You must close the
	/// handle to free up resources.  If there are any messages in the inbound queue,
	/// you may retrieve them.  Otherwise, nothing may be done with the connection
	/// except to close it.
	///
	/// This stats is similar to CLOSE_WAIT in the TCP state machine.
	k_ESteamNetworkingConnectionState_ClosedByPeer = 4,

	/// A disruption in the connection has been detected locally.  (E.g. timeout,
	/// local internet connection disrupted, etc.)
	///
	/// The connection still exists from an API perspective.  You must close the
	/// handle to free up resources.
	///
	/// Attempts to send further messages will fail.  Any remaining received messages
	/// in the queue are available.
	k_ESteamNetworkingConnectionState_ProblemDetectedLocally = 5,

//
// The following values are used internally and will not be returned by any API.
// We document them here to provide a little insight into the state machine that is used
// under the hood.
//

	/// We've disconnected on our side, and from an API perspective the connection is closed.
	/// No more data may be sent or received.  All reliable data has been flushed, or else
	/// we've given up and discarded it.  We do not yet know for sure that the peer knows
	/// the connection has been closed, however, so we're just hanging around so that if we do
	/// get a packet from them, we can send them the appropriate packets so that they can
	/// know why the connection was closed (and not have to rely on a timeout, which makes
	/// it appear as if something is wrong).
	k_ESteamNetworkingConnectionState_FinWait = -1,

	// FIXME - Do we need a TIME_WAIT-like state, or is FinWait sufficient?  In this state
	// could just drop any data packets, assuming they were stray, and the only thing we really
	// need to do is respond to requests to close the connection (assumed to be retries because
	// our ack to them dropped).

	/// We've disconnected on our side, and from an API perspective the connection is closed.
	/// No more data may be sent or received.  From a network perspective, however, on the wire,
	/// we have not yet given any indication to the peer that the connection is closed.
	/// We are in the process of flushing out the last bit of reliable data.  Once that is done,
	/// we will inform the peer that the connection has been closed, and transition to the
	/// FinWait state.
	///
	/// Note that no indication is given to the remote host that we have closed the connection,
	/// until the data has been flushed.  If the remote host attempts to send us data, we will
	/// do whatever is necessary to keep the connection alive until it can be closed properly.
	/// But in fact the data will be discarded, since there is no way for the application to
	/// read it back.  Typically this is not a problem, as application protocols that utilize
	/// the lingering functionality are designed for the remote host to wait for the response
	/// before sending any more data.
	k_ESteamNetworkingConnectionState_Linger = -2, 

	/// Connection is completely inactive and ready to be destroyed
	k_ESteamNetworkingConnectionState_Dead = -3,
};

/// Identifier used for a network location point of presence.
/// Typically you won't need to directly manipulate these.
typedef uint32 SteamNetworkingPOPID;

/// Convert 3- or 4-character ID to 32-bit int.
inline SteamNetworkingPOPID CalculateSteamNetworkingPOPIDFromString( const char *pszCode )
{
	// OK we made a bad decision when we decided how to pack 3-character codes into a uint32.  We'd like to support
	// 4-character codes, but we don't want to break compatibility.  The migration path has some subtleties that make
	// this nontrivial, and there are already some IDs stored in SQL.  Ug, so the 4 character code "abcd" will
	// be encoded with the digits like "0xddaabbcc"
	return
		( (uint32)(pszCode[3]) << 24U ) 
		| ((uint32)(pszCode[0]) << 16U ) 
		| ((uint32)(pszCode[1]) << 8U )
		| (uint32)(pszCode[2]);
}

/// Unpack integer to string representation, including terminating '\0'
#ifdef __cplusplus
template <int N>
inline void GetSteamNetworkingLocationPOPStringFromID( SteamNetworkingPOPID id, char (&szCode)[N] )
{
	static_assert( N >= 5, "Fixed-size buffer not big enough to hold SDR POP ID" );
	szCode[0] = ( id >> 16U );
	szCode[1] = ( id >> 8U );
	szCode[2] = ( id );
	szCode[3] = ( id >> 24U ); // See comment above about deep regret and sadness
	szCode[4] = 0;
}
#endif

/// A local timestamp.  You can subtract two timestamps to get the number of elapsed
/// microseconds.  This is guaranteed to increase over time during the lifetime
/// of a process, but not globally across runs.  You don't need to worry about
/// the value wrapping around.  Note that the underlying clock might not actually have
/// microsecond *resolution*.
typedef int64 SteamNetworkingMicroseconds;

/// Max size of a single message that we can SEND.
/// Note: We might be wiling to receive larger messages,
/// and our peer might, too.
const int k_cbMaxSteamNetworkingSocketsMessageSizeSend = 512 * 1024;

/// Message that has been received
typedef struct _SteamNetworkingMessage_t
{

	/// SteamID that sent this to us.
	CSteamID m_steamIDSender;

	/// The user data associated with the connection.
	///
	/// This is *usually* the same as calling GetConnection() and then
	/// fetching the user data associated with that connection, but for
	/// the following subtle differences:
	///
	/// - This user data will match the connection's user data at the time
	///   is captured at the time the message is returned by the API.
	///   If you subsequently change the userdata on the connection,
	///   this won't be updated.
	/// - This is an inline call, so it's *much* faster.
	/// - You might have closed the connection, so fetching the user data
	///   would not be possible.
	int64 m_nConnUserData;

	/// Local timestamps when it was received
	SteamNetworkingMicroseconds m_usecTimeReceived;

	/// Message number assigned by the sender
	int64 m_nMessageNumber;

	/// Function used to clean up this object.  Normally you won't call
	/// this directly, use Release() instead.
	void (*m_pfnRelease)( struct _SteamNetworkingMessage_t *msg );

	/// Message payload
	void *m_pData;

	/// Size of the payload.
	uint32 m_cbSize;

	/// The connection this came from.  (Not used when using the P2P calls)
	HSteamNetConnection m_conn;

	/// The channel number the message was received on.
	/// (Not used for messages received on "connections")
	int m_nChannel;

	/// Pad to multiple of 8 bytes
	int m___nPadDummy;

	#ifdef __cplusplus

		/// You MUST call this when you're done with the object,
		/// to free up memory, etc.
		inline void Release()
		{
			m_pfnRelease( this );
		}

		// For code compatibility, some accessors
		inline uint32 GetSize() const { return m_cbSize; }
		inline const void *GetData() const { return m_pData; }
		inline CSteamID GetSenderSteamID() const { return m_steamIDSender; }
		inline int GetChannel() const { return m_nChannel; }
		inline HSteamNetConnection GetConnection() const { return m_conn; }
		inline int64 GetConnectionUserData() const { return m_nConnUserData; }
		inline SteamNetworkingMicroseconds GetTimeReceived() const { return m_usecTimeReceived; }
		inline int64 GetMessageNumber() const { return m_nMessageNumber; }
	#endif
} SteamNetworkingMessage_t;

// For code compatibility
typedef SteamNetworkingMessage_t ISteamNetworkingMessage;

/// Object that describes a "location" on the Internet with sufficient
/// detail that we can reasonably estimate an upper bound on the ping between
/// the two hosts, even if a direct route between the hosts is not possible,
/// and the connection must be routed through the Steam Datagram Relay network.
/// This does not contain any information that identifies the host.  Indeed,
/// if two hosts are in the same building or otherwise have nearly identical
/// networking characteristics, then it's valid to use the same location
/// object for both of them.
///
/// NOTE: This object should only be used in the same process!  Do not serialize it,
/// send it over the wire, or persist it in a file or database!  If you need
/// to do that, convert it to a string representation using the methods in
/// ISteamNetworkingUtils().
struct SteamNetworkPingLocation_t
{
	uint8 m_data[ 256 ];
};

/// Max possible length of a ping location, in string format.  This is quite
/// generous worst case and leaves room for future syntax enhancements.
/// Most strings are a lot shorter.
const int k_cchMaxSteamNetworkingPingLocationString = 512;

/// Special values that are returned by some functions that return a ping.
const int k_nSteamNetworkingPing_Failed = -1;
const int k_nSteamNetworkingPing_Unknown = -2;

/// Enumerate various causes of connection termination.  These are designed
/// to work sort of like HTTP error codes, in that the numeric range gives you
/// a ballpark as to where the problem is.
enum ESteamNetConnectionEnd
{
	// Invalid/sentinel value
	k_ESteamNetConnectionEnd_Invalid = 0,

	//
	// Application codes.  You can use these codes if you want to
	// plumb through application-specific error codes.  If you don't need this
	// facility, feel free to always use 0, which is a generic
	// application-initiated closure.
	//
	// The distinction between "normal" and "exceptional" termination is
	// one you may use if you find useful, but it's not necessary for you
	// to do so.  The only place where we distinguish between normal and
	// exceptional is in connection analytics.  If a significant
	// proportion of connections terminates in an exceptional manner,
	// this can trigger an alert.
	//

	// 1xxx: Application ended the connection in a "usual" manner.
	//       E.g.: user intentionally disconnected from the server,
	//             gameplay ended normally, etc
	k_ESteamNetConnectionEnd_App_Min = 1000,
		k_ESteamNetConnectionEnd_App_Generic = k_ESteamNetConnectionEnd_App_Min,
		// Use codes in this range for "normal" disconnection
	k_ESteamNetConnectionEnd_App_Max = 1999,

	// 2xxx: Application ended the connection in some sort of exceptional
	//       or unusual manner that might indicate a bug or configuration
	//       issue.
	// 
	k_ESteamNetConnectionEnd_AppException_Min = 2000,
		k_ESteamNetConnectionEnd_AppException_Generic = k_ESteamNetConnectionEnd_AppException_Min,
		// Use codes in this range for "unusual" disconnection
	k_ESteamNetConnectionEnd_AppException_Max = 2999,

	//
	// System codes:
	//

	// 3xxx: Connection failed or ended because of problem with the
	//       local host or their connection to the Internet.
	k_ESteamNetConnectionEnd_Local_Min = 3000,

		// You cannot do what you want to do because you're running in offline mode.
		k_ESteamNetConnectionEnd_Local_OfflineMode = 3001,

		// We're having trouble contacting many (perhaps all) relays.
		// Since it's unlikely that they all went offline at once, the best
		// explanation is that we have a problem on our end.  Note that we don't
		// bother distinguishing between "many" and "all", because in practice,
		// it takes time to detect a connection problem, and by the time
		// the connection has timed out, we might not have been able to
		// actively probe all of the relay clusters, even if we were able to
		// contact them at one time.  So this code just means that:
		//
		// * We don't have any recent successful communication with any relay.
		// * We have evidence of recent failures to communicate with multiple relays.
		k_ESteamNetConnectionEnd_Local_ManyRelayConnectivity = 3002,

		// A hosted server is having trouble talking to the relay
		// that the client was using, so the problem is most likely
		// on our end
		k_ESteamNetConnectionEnd_Local_HostedServerPrimaryRelay = 3003,

		// We're not able to get the network config.  This is
		// *almost* always a local issue, since the network config
		// comes from the CDN, which is pretty darn reliable.
		k_ESteamNetConnectionEnd_Local_NetworkConfig = 3004,

		// Steam rejected our request because we don't have rights
		// to do this.
		k_ESteamNetConnectionEnd_Local_Rights = 3005,

	k_ESteamNetConnectionEnd_Local_Max = 3999,

	// 4xxx: Connection failed or ended, and it appears that the
	//       cause does NOT have to do with the local host or their
	//       connection to the Internet.  It could be caused by the
	//       remote host, or it could be somewhere in between.
	k_ESteamNetConnectionEnd_Remote_Min = 4000,

		// The connection was lost, and as far as we can tell our connection
		// to relevant services (relays) has not been disrupted.  This doesn't
		// mean that the problem is "their fault", it just means that it doesn't
		// appear that we are having network issues on our end.
		k_ESteamNetConnectionEnd_Remote_Timeout = 4001,

		// Something was invalid with the cert or crypt handshake
		// info you gave me, I don't understand or like your key types,
		// etc.
		k_ESteamNetConnectionEnd_Remote_BadCrypt = 4002,

		// You presented me with a cert that was I was able to parse
		// and *technically* we could use encrypted communication.
		// But there was a problem that prevents me from checking your identity
		// or ensuring that somebody int he middle can't observe our communication.
		// E.g.: - the CA key was missing (and I don't accept unsigned certs)
		// - The CA key isn't one that I trust,
		// - The cert doesn't was appropriately restricted by app, user, time, data center, etc.
		// - The cert wasn't issued to you.
		// - etc
		k_ESteamNetConnectionEnd_Remote_BadCert = 4003,

		// We couldn't rendezvous with the remote host because
		// they aren't logged into Steam
		k_ESteamNetConnectionEnd_Remote_NotLoggedIn = 4004,

		// We couldn't rendezvous with the remote host because
		// they aren't running the right application.
		k_ESteamNetConnectionEnd_Remote_NotRunningApp = 4005,

	k_ESteamNetConnectionEnd_Remote_Max = 4999,

	// 5xxx: Connection failed for some other reason.
	k_ESteamNetConnectionEnd_Misc_Min = 5000,

		// A failure that isn't necessarily the result of a software bug,
		// but that should happen rarely enough that it isn't worth specifically
		// writing UI or making a localized message for.
		// The debug string should contain further details.
		k_ESteamNetConnectionEnd_Misc_Generic = 5001,

		// Generic failure that is most likely a software bug.
		k_ESteamNetConnectionEnd_Misc_InternalError = 5002,

		// The connection to the remote host timed out, but we
		// don't know if the problem is on our end, in the middle,
		// or on their end.
		k_ESteamNetConnectionEnd_Misc_Timeout = 5003,

		// We're having trouble talking to the relevant relay.
		// We don't have enough information to say whether the
		// problem is on our end or not.
		k_ESteamNetConnectionEnd_Misc_RelayConnectivity = 5004,

		// There's some trouble talking to Steam.
		k_ESteamNetConnectionEnd_Misc_SteamConnectivity = 5005,

		// A server in a dedicated hosting situation has no relay sessions
		// active with which to talk back to a client.  (It's the client's
		// job to open and maintain those sessions.)
		k_ESteamNetConnectionEnd_Misc_NoRelaySessionsToClient = 5006,

	k_ESteamNetConnectionEnd_Misc_Max = 5999,
};

/// Max length of diagnostic error message
const int k_cchMaxSteamDatagramErrMsg = 1024;

/// Used to return English-language diagnostic error messages to caller.
/// (For debugging or spewing to a console, etc.  Not intended for UI.)
typedef char SteamDatagramErrMsg[ k_cchMaxSteamDatagramErrMsg ];

/// Max length, in bytes (including null terminator) of the reason string
/// when a connection is closed.
const int k_cchSteamNetworkingMaxConnectionCloseReason = 128;

struct SteamNetConnectionInfo_t
{

	/// Handle to listen socket this was connected on, or k_HSteamListenSocket_Invalid if we initiated the connection
	HSteamListenSocket m_hListenSocket;

	/// Who is on the other end.  Depending on the connection type and phase of the connection, we might not know
	CSteamID m_steamIDRemote;

	// FIXME - some sort of connection type enum?

	/// Arbitrary user data set by the local application code
	int64 m_nUserData;

	/// Remote address.  Might be 0 if we don't know it
	uint32 m_unIPRemote;
	uint16 m_unPortRemote;
	uint16 m__pad1;

	/// What data center is the remote host in?  (0 if we don't know.)
	SteamNetworkingPOPID m_idPOPRemote;

	/// What relay are we using to communicate with the remote host?
	/// (0 if not applicable.)
	SteamNetworkingPOPID m_idPOPRelay;

	/// Local port that we're bound to for this connection.  Might not be applicable
	/// for all connection types.
	//uint16 m_unPortLocal;

	/// High level state of the connection
	int /* ESteamNetworkingConnectionState */ m_eState;

	/// Basic cause of the connection termination or problem.
	/// One of ESteamNetConnectionEnd
	int /* ESteamNetConnectionEnd */ m_eEndReason;

	/// Human-readable, but non-localized explanation for connection
	/// termination or problem.  This is intended for debugging /
	/// diagnostic purposes only, not to display to users.  It might
	/// have some details specific to the issue.
	char m_szEndDebug[ k_cchSteamNetworkingMaxConnectionCloseReason ];
};

/// Quick connection state, pared down to something you could call
/// more frequently without it being too big of a perf hit.
struct SteamNetworkingQuickConnectionStatus
{

	/// High level state of the connection
	int /* ESteamNetworkingConnectionState */ m_eState;

	/// Current ping (ms)
	int m_nPing;

	/// Connection quality measured locally, 0...1.  (Percentage of packets delivered
	/// end-to-end in order).
	float m_flConnectionQualityLocal;

	/// Packet delivery success rate as observed from remote host
	float m_flConnectionQualityRemote;

	/// Current data rates from recent history.
	float m_flOutPacketsPerSec;
	float m_flOutBytesPerSec;
	float m_flInPacketsPerSec;
	float m_flInBytesPerSec;

	/// Estimate rate that we believe that we can send data to our peer.
	/// Note that this could be significantly higher than m_flOutBytesPerSec,
	/// meaning the capacity of the channel is higher than you are sending data.
	/// (That's OK!)
	int m_nSendRateBytesPerSecond;

	/// Number of bytes pending to be sent.  This is data that you have recently
	/// requested to be sent but has not yet actually been put on the wire.  The
	/// reliable number ALSO includes data that was previously placed on the wire,
	/// but has now been scheduled for re-transmission.  Thus, it's possible to
	/// observe m_cbPendingReliable increasing between two checks, even if no
	/// calls were made to send reliable data between the checks.  Data that is
	/// awaiting the nagle delay will appear in these numbers.
	int m_cbPendingUnreliable;
	int m_cbPendingReliable;

	/// Number of bytes of reliable data that has been placed the wire, but
	/// for which we have not yet received an acknowledgment, and thus we may
	/// have to re-transmit.
	int m_cbSentUnackedReliable;

	/// If you asked us to send a message right now, how long would that message
	/// sit in the queue before we actually started putting packets on the wire?
	/// (And assuming Nagle does not cause any packets to be delayed.)
	///
	/// In general, data that is sent by the application is limited by the
	/// bandwidth of the channel.  If you send data faster than this, it must
	/// be queued and put on the wire at a metered rate.  Even sending a small amount
	/// of data (e.g. a few MTU, say ~3k) will require some of the data to be delayed
	/// a bit.
	///
	/// In general, the estimated delay will be approximately equal to
	///
	///		( m_cbPendingUnreliable+m_cbPendingReliable ) / m_nSendRateBytesPerSecond
	///
	/// plus or minus one MTU.  It depends on how much time has elapsed since the last
	/// packet was put on the wire.  For example, the queue might have *just* been emptied,
	/// and the last packet placed ont he wire, and we are exactly up against the send
	/// rate limit.  In that case we might need to wait for one packet's worth of time to
	/// elapse before we can send again.  On the other extreme, the queue might have data
	/// in it waiting for Nagle.  (This will always be less than one packet, because as soon
	/// as we have a complete packet we would send it.)  In that case, we might be ready
	/// to send data now, and this value will be 0.
	///
	/// Note that in general you should not make too many assumptions about *exactly* when
	/// data is going to be placed on the wire or exactly how these stats are going to behave.
	/// For example, when you call SendMessage, even if the channel is clear, we might not
	/// actually have completed the work of placing your data on the wire when API returns.
	/// We might have only queued that data and begun waking up a service thread, so that
	/// we can return to your code ASAP and do the work of encryption and talking to the
	/// OS in parallel in another thread.  Also the timing could change due to changes in
	/// the send rate (e.g. when a connection first starts, the send rate is conservative
	/// but will quickly ramp up to the true bandwidth, and if packets start dropping,
	/// we will lower the send rate.)
	SteamNetworkingMicroseconds m_usecQueueTime;
};

#pragma pack( pop )

/// Configuration values for Steam networking. 
///  
/// Most of these are for controlling extend logging or features 
/// of various subsystems 
enum ESteamNetworkingConfigurationValue
{
	/// 0-100 Randomly discard N pct of unreliable messages instead of sending
	/// Defaults to 0 (no loss).
	k_ESteamNetworkingConfigurationValue_FakeMessageLoss_Send = 0,

	/// 0-100 Randomly discard N pct of unreliable messages upon receive
	/// Defaults to 0 (no loss).
	k_ESteamNetworkingConfigurationValue_FakeMessageLoss_Recv = 1,

	/// 0-100 Randomly discard N pct of packets instead of sending
	k_ESteamNetworkingConfigurationValue_FakePacketLoss_Send = 2,

	/// 0-100 Randomly discard N pct of packets received
	k_ESteamNetworkingConfigurationValue_FakePacketLoss_Recv = 3,

	/// Globally delay all outbound packets by N ms before sending
	k_ESteamNetworkingConfigurationValue_FakePacketLag_Send = 4,

	/// Globally delay all received packets by N ms before processing
	k_ESteamNetworkingConfigurationValue_FakePacketLag_Recv = 5,

	/// Globally reorder some percentage of packets we send
	k_ESteamNetworkingConfigurationValue_FakePacketReorder_Send = 6,

	/// Globally reorder some percentage of packets we receive
	k_ESteamNetworkingConfigurationValue_FakePacketReorder_Recv = 7,

	/// Amount of delay, in ms, to apply to reordered packets.
	k_ESteamNetworkingConfigurationValue_FakePacketReorder_Time = 8,

	/// Upper limit of buffered pending bytes to be sent, if this is reached
	/// SendMessage will return k_EResultLimitExceeded
	/// Default is 512k (524288 bytes)
	k_ESteamNetworkingConfigurationValue_SendBufferSize = 9,

	/// Maximum send rate clamp, 0 is no limit
	/// This value will control the maximum allowed sending rate that congestion 
	/// is allowed to reach.  Default is 0 (no-limit)
	k_ESteamNetworkingConfigurationValue_MaxRate = 10,

	/// Minimum send rate clamp, 0 is no limit
	/// This value will control the minimum allowed sending rate that congestion 
	/// is allowed to reach.  Default is 0 (no-limit)
	k_ESteamNetworkingConfigurationValue_MinRate = 11,

	/// Set the nagle timer.  When SendMessage is called, if the outgoing message 
	/// is less than the size of the MTU, it will be queued for a delay equal to 
	/// the Nagle timer value.  This is to ensure that if the application sends
	/// several small messages rapidly, they are coalesced into a single packet.
	/// See historical RFC 896.  Value is in microseconds. 
	/// Default is 5000us (5ms).
	k_ESteamNetworkingConfigurationValue_Nagle_Time = 12,

	/// Set to true (non-zero) to enable logging of RTT's based on acks.
	/// This doesn't track all sources of RTT, just the inline ones based
	/// on acks, but those are the most common
	k_ESteamNetworkingConfigurationValue_LogLevel_AckRTT = 13,

	/// Log level of SNP packet decoding
	k_ESteamNetworkingConfigurationValue_LogLevel_Packet = 14,

	/// Log when messages are sent/received
	k_ESteamNetworkingConfigurationValue_LogLevel_Message = 15,

	/// Log level when individual packets drop
	k_ESteamNetworkingConfigurationValue_LogLevel_PacketGaps = 16,

	/// Log level for P2P rendezvous.
	k_ESteamNetworkingConfigurationValue_LogLevel_P2PRendezvous = 17,

	/// Log level for sending and receiving pings to relays
	k_ESteamNetworkingConfigurationValue_LogLevel_RelayPings = 18,

	/// If the first N pings to a port all fail, mark that port as unavailable for
	/// a while, and try a different one.  Some ISPs and routers may drop the first
	/// packet, so setting this to 1 may greatly disrupt communications.
	k_ESteamNetworkingConfigurationValue_ClientConsecutitivePingTimeoutsFailInitial = 19,

	/// If N consecutive pings to a port fail, after having received successful 
	/// communication, mark that port as unavailable for a while, and try a 
	/// different one.
	k_ESteamNetworkingConfigurationValue_ClientConsecutitivePingTimeoutsFail = 20,

	/// Minimum number of lifetime pings we need to send, before we think our estimate
	/// is solid.  The first ping to each cluster is very often delayed because of NAT,
	/// routers not having the best route, etc.  Until we've sent a sufficient number
	/// of pings, our estimate is often inaccurate.  Keep pinging until we get this
	/// many pings.
	k_ESteamNetworkingConfigurationValue_ClientMinPingsBeforePingAccurate = 21,

	/// Set all steam datagram traffic to originate from the same local port.  
	/// By default, we open up a new UDP socket (on a different local port) 
	/// for each relay.  This is not optimal, but it works around some 
	/// routers that don't implement NAT properly.  If you have intermittent 
	/// problems talking to relays that might be NAT related, try toggling 
	/// this flag
	k_ESteamNetworkingConfigurationValue_ClientSingleSocket = 22,

	/// Don't automatically fail IP connections that don't have strong auth.
	/// On clients, this means we will attempt the connection even if we don't
	/// know our SteamID or can't get a cert.  On the server, it means that we won't
	/// automatically reject a connection due to a failure to authenticate.
	/// (You can examine the incoming connection and decide whether to accept it.)
	k_ESteamNetworkingConfigurationValue_IP_Allow_Without_Auth = 23,

	/// Timeout value (in seconds) to use when first connecting
	k_ESteamNetworkingConfigurationValue_Timeout_Seconds_Initial = 24,

	/// Timeout value (in seconds) to use after connection is established
	k_ESteamNetworkingConfigurationValue_Timeout_Seconds_Connected = 25,

	/// Number of k_ESteamNetworkingConfigurationValue defines
	k_ESteamNetworkingConfigurationValue_Count,
};

/// Configuration strings for Steam networking. 
///  
/// Most of these are for controlling extend logging or features 
/// of various subsystems 
enum ESteamNetworkingConfigurationString
{
	// Code of relay cluster to use.  If not empty, we will only use relays in that cluster.  E.g. 'iad'
	k_ESteamNetworkingConfigurationString_ClientForceRelayCluster = 0,

	// For debugging, generate our own (unsigned) ticket, using the specified 
	// gameserver address.  Router must be configured to accept unsigned tickets.
	k_ESteamNetworkingConfigurationString_ClientDebugTicketAddress = 1,

	// For debugging.  Override list of relays from the config with this set
	// (maybe just one).  Comma-separated list.
	k_ESteamNetworkingConfigurationString_ClientForceProxyAddr = 2,

	// Number of k_ESteamNetworkingConfigurationString defines
	k_ESteamNetworkingConfigurationString_Count = k_ESteamNetworkingConfigurationString_ClientForceProxyAddr + 1,
};

/// Configuration values for Steam networking per connection
enum ESteamNetworkingConnectionConfigurationValue
{
	// Maximum send rate clamp, 0 is no limit
	// This value will control the maximum allowed sending rate that congestion 
	// is allowed to reach.  Default is 0 (no-limit)
	k_ESteamNetworkingConnectionConfigurationValue_SNP_MaxRate = 0,

	// Minimum send rate clamp, 0 is no limit
	// This value will control the minimum allowed sending rate that congestion 
	// is allowed to reach.  Default is 0 (no-limit)
	k_ESteamNetworkingConnectionConfigurationValue_SNP_MinRate = 1,

	// Number of k_ESteamNetworkingConfigurationValue defines
	k_ESteamNetworkingConnectionConfigurationValue_Count,
};

enum ESteamDatagramPartner
{
	k_ESteamDatagramPartner_None = -1,
	k_ESteamDatagramPartner_Steam = 0,
	k_ESteamDatagramPartner_China = 1,
};

#endif // #ifndef STEAMNETWORKINGTYPES
