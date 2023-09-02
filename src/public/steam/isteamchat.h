//====== Copyright © 1996-2009, Valve Corporation, All rights reserved. =======
//
// Purpose: interface to Steam group chats
//
//=============================================================================

#ifndef ISTEAMCHAT_H
#define ISTEAMCHAT_H

#ifdef _WIN32
#pragma once
#endif

#include "steam_api_common.h"
#include "isteamfriends.h"

enum EChatNotificationFormat
{
	kChatNotificationFormat_Disable,					// disable notifications for this channel
	kChatNotificationFormat_TextOnlyAdjusted,			// leave raw text; replace certain BBcode tokens with localized text (ie., "user uploaded an image"); remove other BBcode entirely
};

//-----------------------------------------------------------------------------
// Purpose: interface to Steam group chat content.
//-----------------------------------------------------------------------------
class ISteamChat
{
public:
	// A running game can subscribe to the activity inside a Steam group chat. This functionality can be used by
	// games to, for example, have a guild chat that's accessible outside the game (using normal Steam group chat
	// functionality) and inside the game (using this API). Using this API from inside a game won't affect or limit
	// users interacting with the chat channel through other means.
	//
	// RequestSteamGroupChatMessageNotifications is the entry point for this functionality. A successful subscription
	// will result in a GroupChatMessageNotification_t callback being posted whenever content new message is sent to
	// that chat channel. A successful subscription is also a prerequisite for usage of other parts of the API (ie.,
	// requesting message history). To succeed, the group chat in question is required to have been created by the same
	// same app ID as the running game.
	//
	// Requesting permission to subscribe to a channel is an async operation. The results of the permissions
	// check will be sent via the RequestGroupChatMessageNotificationsResponse_t callback. Actual message content
	// will only be sent if the subscription attempt succeeds, meaning that the
	// RequestGroupChatMessageNotificationsResponse_t carries the value k_EResultOK.
	//
	// In the event that no async work is needed (either you're unsubscribing from a channel by specifying 
	// kChatNotificationFormat_Disable, you're already subscribed to the channel so no permissions check is needed,
	// or you specified invalid parameters so there is no permissions check possible), the function will immediately
	// return k_EResultOK or k_EResultInvalidParam as appropriate.
	//
	// If a subscription attempt is disabled (kChatNotificationFormat_Disable) while waiting for the initial
	// subscription response, no RequestGroupChatMessageNotificationsResponse_t message will be sent. If multiple
	// subscriptions attempts for the same channel are made before any RequestGroupChatMessageNotificationsResponse_t
	// are sent, only a single callback response callback will be posted.
	//
	// A summary of all possible return codes from this function:
	//
	//		- k_EResultOK: no permissions check required for this operation. You are now subscribed and will receive events.
	//		- k_EResultPending: a permissions check has been initiated for this request. Details will be available in a
	//							RequestGroupChatMessageNotificationsResponse_t callback.
	//		- k_EResultInvalidParam: you specified an invalid value for ulGroupChatID, ulChatChannelID, or eFormat.
	//		- k_EResultAccessDenied: unable to determine which game was running, and so which game was requesting permission.
	//		- k_EResultServiceUnavailable: the user is running a version of the Steam client that doesn't support Steam
	//									   group chats.
	virtual EResult RequestSteamGroupChatMessageNotifications( uint64 ulGroupChatID, uint64 ulChatChannelID, EChatNotificationFormat eFormat ) = 0;

	// Request historical message content for a subscribed chat channel. This is an asynchronous call that
	// will generate a number of GroupChatMessageNotification_t callbacks, followed by a single
	// RequestGroupChatMessageHistoryNotification_t callback to summarize the state.
	//
	// Requesting historical messages for a channel requires that it have already been successfully subscribed to
	// via RequestSteamGroupChatMessageNotifications().
	//
	// There's an internal limit of one day (86,400 seconds). Attempts to request history from further back will
	// return k_EResultInvalidParam.
	//
	// A summary of all possible return codes from this function:
	//
	//		- k_EResultPending: the request has been initiated. Results will come through the
	//							RequestGroupChatMessageHistoryNotification_t callback.
	//		- k_EResultInvalidParam: you specified an invalid value for ulGroupChatID, ulChatChannelID, or nHistorySeconds.
	//		- k_EResultAccessDenied: the game has not successfully subscribed to this chat channel. A call to
	//								 RequestSteamGroupChatMessageNotifications that results in permissions being granted
	//								 must be made before this API is used.
	virtual EResult RequestSteamGroupChatMessageHistory( uint64 ulGroupChatID, uint64 ulChatChannelID, uint32 nHistorySeconds ) = 0;

	// Games can provide a game-native UI for both reading and sending group chat messages. This function can be used
	// to take user chat content typed into game-native UI and say it on the user's behalf into a Steam group chat.
	//
	// Sending a message to a channel channel requires that it have already been successfully subscribed to via
	// RequestSteamGroupChatMessageNotifications().
	//
	// The nAppSpecificMessageIdentifier parameter is purely passed through this API so that the calling code can
	// identify particular requests. In the event of failure to send, for example, a unique message can be identified
	// and retried.
	//
	// A summary of all possible return codes from this function:
	//
	//		- k_EResultPending: the request has been initiated. Results will come through the
	//							RequestSendSteamGroupChatMessageNotification_t callback.
	//		- k_EResultInvalidParam: you specified an invalid value for ulGroupChatID or ulChatChannelID.
	//		- k_EResultAccessDenied: the game has not successfully subscribed to this chat channel. A call to
	//								 RequestSteamGroupChatMessageNotifications that results in permissions being granted
	//								 must be made before this API is used.
	virtual EResult RequestSendSteamGroupChatMessage( uint64 ulGroupChatID, uint64 ulChatChannelID, uint32 nAppSpecificMessageIdentifier, const char *pszMessage ) = 0;
};

#define STEAMCHAT_INTERFACE_VERSION "STEAMCHAT_INTERFACE_VERSION003"

// Global interface accessor
inline ISteamChat *SteamChat();
STEAM_DEFINE_USER_INTERFACE_ACCESSOR( ISteamChat *, SteamChat, STEAMCHAT_INTERFACE_VERSION );

// callbacks
#if defined( VALVE_CALLBACK_PACK_SMALL )
#pragma pack( push, 4 )
#elif defined( VALVE_CALLBACK_PACK_LARGE )
#pragma pack( push, 8 )
#else
#error steam_api_common.h should define VALVE_CALLBACK_PACK_xxx
#endif 

STEAM_CALLBACK_BEGIN( RequestGroupChatMessageNotificationsResponse_t, k_iSteamChatCallbacks + 1 )
	STEAM_CALLBACK_MEMBER( 0, uint64, m_ulGroupChatID );
	STEAM_CALLBACK_MEMBER( 1, uint64, m_ulChatChannelID );

	// The result of the attempt to subscribe to notifications for a particular group chat channel.
	// The value will be one of:
	//
	//		k_EResultOK -- subscription attempt succeeded
	//		k_EResultFail -- unspecified error (unable to find channel, unable to contact Steam servers, etc.)
	//		k_EResultAccessDenied -- the requesting game doesn't have permission to read the chat messages from the
	//								 specified group chat
	//		k_EResultAccountDisabled -- the current user is not a member of the specified group chat
	STEAM_CALLBACK_MEMBER( 2, EResult, m_eResult );
STEAM_CALLBACK_END( 3 )

STEAM_CALLBACK_BEGIN( RequestGroupChatMessageHistoryNotification_t, k_iSteamChatCallbacks + 2 )
	STEAM_CALLBACK_MEMBER( 0, uint64, m_ulGroupChatID );
	STEAM_CALLBACK_MEMBER( 1, uint64, m_ulChatChannelID );
	STEAM_CALLBACK_MEMBER( 2, EResult, m_eResult );					// k_EResultOK or k_EResultFail
STEAM_CALLBACK_END( 3 )

STEAM_CALLBACK_BEGIN( RequestSendSteamGroupChatMessageNotification_t, k_iSteamChatCallbacks + 3 )
	STEAM_CALLBACK_MEMBER( 0, uint64, m_ulGroupChatID );
	STEAM_CALLBACK_MEMBER( 1, uint64, m_ulChatChannelID );
	STEAM_CALLBACK_MEMBER( 2, uint32, m_nAppSpecificMessageIdentifier );
	STEAM_CALLBACK_MEMBER( 3, EResult, m_eResult );					// k_EResultOK or k_EResultFail
STEAM_CALLBACK_END( 4 )

enum
{
	k_cchMaxChatMessageNotificationContentSize = 512,
};

STEAM_CALLBACK_BEGIN( GroupChatMessageNotification_t, k_iSteamChatCallbacks + 4 )
	// Where?
	STEAM_CALLBACK_MEMBER( 0, uint64, m_ulGroupChatID );
	STEAM_CALLBACK_MEMBER( 1, uint64, m_ulChatChannelID );

	// When?
	STEAM_CALLBACK_MEMBER( 2, RTime32, m_rtTimestamp );				// when the message was posted to the channel, not necessarily when delivered here
	STEAM_CALLBACK_MEMBER( 3, uint32, m_nOrdinal );					// there can be multiple messages at the same timestamp; the pair of [m_rtTimestamp, m_nOrdinal] provides a persistent, unique identifier per channel
	STEAM_CALLBACK_MEMBER( 4, bool, m_bHistorical );				// is this notification coming from a history request (value true), or did it happen in realtime just now (value false)?

	// Who?
	STEAM_CALLBACK_MEMBER( 5, CSteamID, m_steamSpeaker );
	
	// What?
	STEAM_CALLBACK_MEMBER_ARRAY( 6, char, m_rgchMessage, k_cchMaxChatMessageNotificationContentSize );	// UTF8, formatted as per request
STEAM_CALLBACK_END( 7 )

enum ESteamClientChatRoomMemberStateChange
{
	k_ESteamChatRoomMemberStateChange_Joined = 1,
	k_ESteamChatRoomMemberStateChange_Parted = 2,
};

STEAM_CALLBACK_BEGIN( GroupChatMemberStateChangeNotification_t, k_iSteamChatCallbacks + 5 )
	STEAM_CALLBACK_MEMBER( 0, uint64, m_ulGroupChatID );
	STEAM_CALLBACK_MEMBER( 1, uint64, m_ulChatChannelID );
	STEAM_CALLBACK_MEMBER( 2, CSteamID, m_steamID );
	STEAM_CALLBACK_MEMBER( 3, int /* ESteamClientChatRoomMemberStateChange */, m_eStateChange );
STEAM_CALLBACK_END( 4 )

enum
{
	k_nMaxChatInitialMemberCount = 64,
};

STEAM_CALLBACK_BEGIN( GroupChatInitialStateNotification_t, k_iSteamChatCallbacks + 6 )
	STEAM_CALLBACK_MEMBER( 0, uint64, m_ulGroupChatID );
	STEAM_CALLBACK_MEMBER( 1, uint64, m_ulChatChannelID );
	STEAM_CALLBACK_MEMBER( 2, int, m_nTotalChatMemberCount );
	STEAM_CALLBACK_MEMBER( 3, int, m_nThisMessageInitialChatMemberOffset );
	STEAM_CALLBACK_MEMBER( 4, int, m_nThisMessageChatMemberCount );
	STEAM_CALLBACK_MEMBER_ARRAY( 5, CSteamID, m_steamIDs, k_nMaxChatInitialMemberCount );
STEAM_CALLBACK_END( 6 )

#pragma pack( pop )

#endif // ISTEAMHTTP_H