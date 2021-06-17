//========= Copyright Valve LLC, All rights reserved. ============


#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <minbase/minbase_identify.h>
#include <minbase/minbase_types.h>
#include <tier0/dbg.h>
#include <vstdlib/strtools.h>

#include "steamid.h"

static const char *DecimalToUint64( const char *pchStr, uint64 unLimit,
                                    uint64 *punVal )
{
    const char *pchStart = pchStr;
    uint64 unVal = 0;

    while ( *pchStr >= '0' && *pchStr <= '9' )
    {
        uint64 unNext = unVal * 10;
        
        if ( unNext < unVal )
        {
            // 64-bit overflow.
            return NULL;
        }

        unVal = unNext + (uint64)( *pchStr - '0' );
        if ( unVal > unLimit )
        {
            // Limit overflow.
            return NULL;
        }

        pchStr++;
    }
    if ( pchStr == pchStart )
    {
        // No number at all.
        return NULL;
    }

    *punVal = unVal;
    return pchStr;
}

//-----------------------------------------------------------------------------
// Purpose: Constructor
// Input  : pchSteamID -		text representation of a Steam ID
//-----------------------------------------------------------------------------
CSteamID::CSteamID( const char *pchSteamID, EUniverse eDefaultUniverse /* = k_EUniverseInvalid */  )
{
	SetFromString( pchSteamID, eDefaultUniverse );
}


//-----------------------------------------------------------------------------
// Purpose: Initializes a steam ID from a string
// Input  : pchSteamID -		text representation of a Steam ID
//-----------------------------------------------------------------------------
void CSteamID::SetFromString( const char *pchSteamID, EUniverse eDefaultUniverse )
{
	uint nAccountID = 0;
	uint nInstance = 1;
	EUniverse eUniverse = eDefaultUniverse;
	EAccountType eAccountType = k_EAccountTypeIndividual;
    const char *pchSteamIDString = pchSteamID;
    CSteamID StrictID;

    StrictID.SetFromStringStrict( pchSteamID, eDefaultUniverse );

	if ( *pchSteamID == '[' )
		pchSteamID++;

	// BUGBUG Rich use the Q_ functions
	if ( *pchSteamID == 'A' || *pchSteamID == 'a' )
	{
		// This is test only
		if ( *pchSteamID == 'A' )
			eAccountType = k_EAccountTypeAnonGameServer;
		else
		{
			eAccountType = k_EAccountTypeAnonUser;
			nInstance = 0;
		}
		pchSteamID++; // skip the A
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :

		if ( strchr( pchSteamID, '(' ) )
			sscanf( strchr( pchSteamID, '(' ), "(%u)", &nInstance );
		const char *pchColon = strchr( pchSteamID, ':' );
		if ( pchColon && *pchColon != 0 && strchr( pchColon+1, ':' ))
		{
			sscanf( pchSteamID, "%u:%u:%u", (uint*)&eUniverse, &nAccountID, &nInstance );
		}
		else if ( pchColon )
		{
			sscanf( pchSteamID, "%u:%u", (uint*)&eUniverse, &nAccountID );
		}
		else
		{
			sscanf( pchSteamID, "%u", &nAccountID );
		}

		if ( nAccountID == 0 )
		{
			// i dont care what number you entered
			CreateBlankAnonLogon(eUniverse);
		}
		else
		{
			InstancedSet( nAccountID, nInstance, eUniverse, eAccountType );
		}
        // Catch cases where we're allowing sloppy input that we
        // might not want to allow.
        AssertMsg1( this->operator==( StrictID ), "Steam ID does not pass strict parsing: '%s'", pchSteamIDString );
		return;
	}
	else if (*pchSteamID == 'G')
	{
		pchSteamID++; // skip the G
		eAccountType = k_EAccountTypeGameServer;
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}
	else if (*pchSteamID == 'C')
	{
		pchSteamID++; // skip the C
		eAccountType = k_EAccountTypeContentServer;
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}
	else if (*pchSteamID == 'g')
	{
		pchSteamID++; // skip the g
		eAccountType = k_EAccountTypeClan;
		nInstance = 0;
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}
	else if (*pchSteamID == 'c')
	{
		pchSteamID++; // skip the c
		eAccountType = k_EAccountTypeChat;
		nInstance = k_EChatInstanceFlagClan;
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}
	else if (*pchSteamID == 'L')
	{
		pchSteamID++; // skip the c
		eAccountType = k_EAccountTypeChat;
		nInstance = k_EChatInstanceFlagLobby;
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}
	else if (*pchSteamID == 'T')
	{
		pchSteamID++; // skip the T
		eAccountType = k_EAccountTypeChat;
		nInstance = 0;	// Anon chat
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}
	else if (*pchSteamID == 'U')
	{
		pchSteamID++; // skip the U
		eAccountType = k_EAccountTypeIndividual;
		nInstance = 1;
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}
	else if (*pchSteamID == 'i')
	{
		pchSteamID++; // skip the i
		eAccountType = k_EAccountTypeInvalid;
		nInstance = 1;
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
	}

	if ( strchr( pchSteamID, ':' ) )
	{
		if (*pchSteamID == '[')
			pchSteamID++; // skip the optional [
		sscanf( pchSteamID, "%u:%u", (uint*)&eUniverse, &nAccountID );
		if ( eUniverse == k_EUniverseInvalid )
			eUniverse = eDefaultUniverse; 
	}
	else
	{
        uint64 unVal64 = 0;
        
		sscanf( pchSteamID, "%llu", &unVal64 );
        if ( unVal64 > UINT_MAX )
        {
            // Assume a full 64-bit Steam ID.
            SetFromUint64( unVal64 );
            // Catch cases where we're allowing sloppy input that we
            // might not want to allow.
            AssertMsg1( this->operator==( StrictID ), "Steam ID does not pass strict parsing: '%s'", pchSteamIDString );
            return;
        }
        else
        {
            nAccountID = (uint)unVal64;
        }
	}	
	
	Assert( (eUniverse > k_EUniverseInvalid) && (eUniverse < k_EUniverseMax) );

	InstancedSet( nAccountID, nInstance, eUniverse, eAccountType );

    // Catch cases where we're allowing sloppy input that we
    // might not want to allow.
    AssertMsg1( this->operator==( StrictID ), "Steam ID does not pass strict parsing: '%s'", pchSteamIDString );
}

// SetFromString allows many partially-correct strings, constraining how
// we might be able to change things in the future.
// SetFromStringStrict requires the exact string forms that we support
// and is preferred when the caller knows it's safe to be strict.
// Returns whether the string parsed correctly.  The ID may
// still be invalid even if the string parsed correctly.
// If the string didn't parse correctly the ID will always be invalid.
bool CSteamID::SetFromStringStrict( const char *pchSteamID, EUniverse eDefaultUniverse )
{
	uint nAccountID = 0;
	uint nInstance = 1;
    uint unMaxVal = 2;
	EUniverse eUniverse = eDefaultUniverse;
	EAccountType eAccountType = k_EAccountTypeIndividual;
    char chPrefix;
    bool bBracket = false;
    bool bValid = true;
    uint64 unVal[3];
    const char *pchEnd;

    // Start invalid.
    Clear();
    
    if ( !pchSteamID )
    {
        return false;
    }
    
	if ( *pchSteamID == '[' )
    {
		pchSteamID++;
        bBracket = true;
    }

    chPrefix = *pchSteamID;
    switch( chPrefix )
    {
    case 'A':
		// This is test only
		eAccountType = k_EAccountTypeAnonGameServer;
        unMaxVal = 3;
        break;

	case 'a':
		// This is test only
		eAccountType = k_EAccountTypeAnonUser;
		nInstance = 0;
        unMaxVal = 3;
        break;

    case 'G':
		eAccountType = k_EAccountTypeGameServer;
        break;

    case 'C':
		eAccountType = k_EAccountTypeContentServer;
        break;

    case 'g':
		eAccountType = k_EAccountTypeClan;
		nInstance = 0;
        break;

    case 'c':
		eAccountType = k_EAccountTypeChat;
		nInstance = k_EChatInstanceFlagClan;
        break;

    case 'L':
		eAccountType = k_EAccountTypeChat;
		nInstance = k_EChatInstanceFlagLobby;
        break;

    case 'T':
		eAccountType = k_EAccountTypeChat;
		nInstance = 0;	// Anon chat
        break;

    case 'U':
		eAccountType = k_EAccountTypeIndividual;
		nInstance = 1;
        break;

    case 'i':
		eAccountType = k_EAccountTypeInvalid;
		nInstance = 1;
        break;

    default:
        // We're reserving other leading characters so
        // this should only be the plain-digits case.
        if (chPrefix < '0' || chPrefix > '9')
        {
            bValid = false;
        }
        chPrefix = 0;
        break;
    }

    if ( chPrefix )
    {
        pchSteamID++; // skip the prefix
		if (*pchSteamID == '-' || *pchSteamID == ':')
			pchSteamID++; // skip the optional - or :
    }

    uint unIdx = 0;

    for (;;)
    {
        pchEnd = DecimalToUint64( pchSteamID, UINT64_MAX, &unVal[unIdx] );
        if ( !pchEnd )
        {
            bValid = false;
            break;
        }

        unIdx++;

        // For 'A' we can have a trailing instance, which must
        // be the end of the string.
        if ( *pchEnd == '(' &&
             chPrefix == 'A' )
        {
            if ( unIdx > 2 )
            {
                // Two instance IDs provided.
                bValid = false;
            }
            
            pchEnd = DecimalToUint64( pchEnd + 1, k_unSteamAccountInstanceMask, &unVal[2] );
            if ( !pchEnd ||
                 *pchEnd != ')' )
            {
                bValid = false;
                break;
            }
            else
            {
                nInstance = (uint)unVal[2];

                pchEnd++;
                if ( *pchEnd == ':' )
                {
                    // Not expecting more values.
                    bValid = false;
                    break;
                }
            }
        }

        if ( *pchEnd != ':' )
        {
            if ( bBracket )
            {
                if ( *pchEnd != ']' ||
                     *(pchEnd + 1) != 0 )
                {
                    bValid = false;
                }
            }
            else if ( *pchEnd != 0 )
            {
                bValid = false;
            }

            break;
        }

        if ( unIdx >= unMaxVal )
        {
            bValid = false;
            break;
        }

        pchSteamID = pchEnd + 1;
    }

    if ( unIdx > 2 )
    {
        if ( unVal[2] <= k_unSteamAccountInstanceMask )
        {
            nInstance = (uint)unVal[2];
        }
        else
        {
            bValid = false;
        }
    }
    if ( unIdx > 1 )
    {
        if ( unVal[0] >= k_EUniverseInvalid &&
             unVal[0] < k_EUniverseMax )
        {
            eUniverse = (EUniverse)unVal[0];
            if ( eUniverse == k_EUniverseInvalid )
                eUniverse = eDefaultUniverse;
        }
        else
        {
            bValid = false;
        }

        if ( unVal[1] <= k_unSteamAccountIDMask )
        {
            nAccountID = (uint)unVal[1];
        }
        else
        {
            bValid = false;
        }
    }
    else if ( unIdx > 0 )
    {
        if ( unVal[0] <= k_unSteamAccountIDMask )
        {
            nAccountID = (uint)unVal[0];
        }
        else if ( !chPrefix )
        {
            if ( bValid )
            {
                SetFromUint64( unVal[0] );
            }
            return bValid;
        }
        else
        {
            bValid = false;
        }
    }
    else
    {
        bValid = false;
    }

    if ( bValid )
    {
        if ( chPrefix == 'A' )
        {
            if ( nAccountID == 0 )
            {
                // i dont care what number you entered
                CreateBlankAnonLogon(eUniverse);
                return bValid;
            }
        }

        InstancedSet( nAccountID, nInstance, eUniverse, eAccountType );
    }

    return bValid;
}


#if defined( INCLUDED_STEAM2_USERID_STRUCTS ) 
//-----------------------------------------------------------------------------
// Purpose: Initializes a steam ID from a Steam2 ID string
// Input:	pchSteam2ID -	Steam2 ID (as a string #:#:#) to convert
//			eUniverse -		universe this ID belongs to
// Output:	true if successful, false otherwise
//-----------------------------------------------------------------------------
bool CSteamID::SetFromSteam2String( const char *pchSteam2ID, EUniverse eUniverse )
{
	Assert( pchSteam2ID );

	// Convert the Steam2 ID string to a Steam2 ID structure
	TSteamGlobalUserID steam2ID;
	steam2ID.m_SteamInstanceID = 0;
	steam2ID.m_SteamLocalUserID.Split.High32bits = 0;
	steam2ID.m_SteamLocalUserID.Split.Low32bits	= 0;

	const char *pchTSteam2ID = pchSteam2ID;

	// Customer support is fond of entering steam IDs in the following form:  STEAM_n:x:y
	const char *pchOptionalLeadString = "STEAM_";
	if ( V_strnicmp( pchSteam2ID, pchOptionalLeadString, V_strlen( pchOptionalLeadString ) ) == 0 )
		pchTSteam2ID = pchSteam2ID + V_strlen( pchOptionalLeadString );

	char cExtraCharCheck = 0;

	int cFieldConverted = sscanf( pchTSteam2ID, "%hu:%u:%u%c", &steam2ID.m_SteamInstanceID, 
		&steam2ID.m_SteamLocalUserID.Split.High32bits, &steam2ID.m_SteamLocalUserID.Split.Low32bits, &cExtraCharCheck );

	// Validate the conversion ... a special case is steam2 instance ID 1 which is reserved for special DoD handling
	if ( cExtraCharCheck != 0 || cFieldConverted == EOF || cFieldConverted < 2 || ( cFieldConverted < 3 && steam2ID.m_SteamInstanceID != 1 ) )
		return false;

	// Now convert to steam ID from the Steam2 ID structure
	SetFromSteam2( &steam2ID, eUniverse );
	return true;
}
#endif

//-----------------------------------------------------------------------------
// Purpose: Renders the steam ID to a buffer.  NOTE: for convenience of calling
//			code, this code returns a pointer to a static buffer and is NOT thread-safe.
// Output:  buffer with rendered Steam ID
//-----------------------------------------------------------------------------
const char * CSteamID::Render() const
{
	// longest length of returned string is k_cBufLen
	//	[A:%u:%u:%u]
	//	 %u == 10 * 3 + 6 == 36, plus terminator == 37
	const int k_cBufLen = 37;

	const int k_cBufs = 4;	// # of static bufs to use (so people can compose output with multiple calls to Render() )
	static char rgchBuf[k_cBufs][k_cBufLen];
	static int nBuf = 0;
	char * pchBuf = rgchBuf[nBuf];	// get pointer to current static buf
	nBuf ++;	// use next buffer for next call to this method
	nBuf %= k_cBufs;

	if ( k_EAccountTypeAnonGameServer == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[A:%u:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID, m_steamid.m_comp.m_unAccountInstance );
	}
	else if ( k_EAccountTypeGameServer == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[G:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	}
	else if ( k_EAccountTypeMultiseat == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[M:%u:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID, m_steamid.m_comp.m_unAccountInstance );
	} 
	else if ( k_EAccountTypePending == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[P:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	} 
	else if ( k_EAccountTypeContentServer == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[C:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	}
	else if ( k_EAccountTypeClan == m_steamid.m_comp.m_EAccountType )
	{
		// 'g' for "group"
		V_snprintf( pchBuf, k_cBufLen, "[g:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	}
	else if ( k_EAccountTypeChat == m_steamid.m_comp.m_EAccountType )
	{
		if ( m_steamid.m_comp.m_unAccountInstance & k_EChatInstanceFlagClan )
		{
			V_snprintf( pchBuf, k_cBufLen, "[c:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
		}
		else if ( m_steamid.m_comp.m_unAccountInstance & k_EChatInstanceFlagLobby )
		{
			V_snprintf( pchBuf, k_cBufLen, "[L:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
		}
		else // Anon chat
		{
			V_snprintf( pchBuf, k_cBufLen, "[T:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
		}
	}
	else if ( k_EAccountTypeInvalid == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[I:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	}
	else if ( k_EAccountTypeIndividual == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[U:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	}
	else if ( k_EAccountTypeAnonUser == m_steamid.m_comp.m_EAccountType )
	{
		V_snprintf( pchBuf, k_cBufLen, "[a:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	}
	else
	{
		V_snprintf( pchBuf, k_cBufLen, "[i:%u:%u]", m_steamid.m_comp.m_EUniverse, m_steamid.m_comp.m_unAccountID );
	}
	return pchBuf;
}


//-----------------------------------------------------------------------------
// Purpose: Renders the passed-in steam ID to a buffer.  NOTE: for convenience of calling
//			code, this code returns a pointer to a static buffer and is NOT thread-safe.
// Input:	64-bit representation of Steam ID to render
// Output:  buffer with rendered Steam ID
//-----------------------------------------------------------------------------
const char * CSteamID::Render( uint64 ulSteamID )
{
	CSteamID steamID( ulSteamID );
	return steamID.Render();
}


//-----------------------------------------------------------------------------
// Purpose: some steamIDs are for internal use only
// This is really debug code, but we run with asserts on in retail, so ...
//-----------------------------------------------------------------------------
bool CSteamID::BValidExternalSteamID() const
{
	if ( m_steamid.m_comp.m_EAccountType == k_EAccountTypePending )
		return false;
	if ( m_steamid.m_comp.m_EAccountType != k_EAccountTypeAnonGameServer && m_steamid.m_comp.m_EAccountType != k_EAccountTypeContentServer && m_steamid.m_comp.m_EAccountType != k_EAccountTypeAnonUser )
	{
		if ( m_steamid.m_comp.m_unAccountID == 0 && m_steamid.m_comp.m_unAccountInstance == 0 )
			return false;
	}
	return true;
}

#ifdef STEAM
//-----------------------------------------------------------------------------
// Purpose:	Returns the matching chat steamID, with the default instance of 0
// Input:	SteamID, either a Clan or a Chat type
// Output:	SteamID with account type changed to chat, and the Clan flag set. 
//			If account type was not chat to start with, instance will be set to 0
//-----------------------------------------------------------------------------
CSteamID ChatIDFromSteamID( const CSteamID &steamID )
{
	if ( steamID.GetEAccountType() == k_EAccountTypeChat )
		return steamID;

	return ChatIDFromClanID( steamID );
}


//-----------------------------------------------------------------------------
// Purpose:	Returns the matching chat steamID, with the default instance of 0
// Input:	SteamID, either a Clan type or a Chat type w/ the Clan flag set
// Output:	SteamID with account type changed to clan.  
//			If account type was not clan to start with, instance will be set to 0
//-----------------------------------------------------------------------------
CSteamID ClanIDFromSteamID( const CSteamID &steamID )
{
	if ( steamID.GetEAccountType() == k_EAccountTypeClan )
		return steamID;

	return ClanIDFromChatID( steamID );
}


// Asserts steamID type before conversion
CSteamID ChatIDFromClanID( const CSteamID &steamIDClan )
{
	Assert( steamIDClan.GetEAccountType() == k_EAccountTypeClan );

	return CSteamID( steamIDClan.GetAccountID(), k_EChatInstanceFlagClan, steamIDClan.GetEUniverse(), k_EAccountTypeChat );
}


// Asserts steamID type before conversion
CSteamID ClanIDFromChatID( const CSteamID &steamIDChat )
{
	Assert( steamIDChat.GetEAccountType() == k_EAccountTypeChat );
	Assert( k_EChatInstanceFlagClan & steamIDChat.GetUnAccountInstance() );

	return CSteamID( steamIDChat.GetAccountID(), 0, steamIDChat.GetEUniverse(), k_EAccountTypeClan );
}


//-----------------------------------------------------------------------------
// Purpose:	CGameID "hidden" functions
//			move these somewhere else maybe
//-----------------------------------------------------------------------------
CGameID::CGameID( const char *pchGameID )
{
	m_ulGameID = 0;

	sscanf( pchGameID, "%llu", &m_ulGameID );

	switch ( m_gameID.m_nType )
	{
	default:
		AssertMsg( false, "Unknown GameID type" );
		m_ulGameID = 0;
		break;
	case k_EGameIDTypeApp:
	case k_EGameIDTypeGameMod:
	case k_EGameIDTypeShortcut:
	case k_EGameIDTypeP2P:
		break;
	}
}


// renders this Game ID to string
const char * CGameID::Render() const
{
	// longest buffer is log10(2**64) == 20 + 1 == 21
	const int k_cBufLen = 21;

	const int k_cBufs = 4;	// # of static bufs to use (so people can compose output with multiple calls to Render() )
	static char rgchBuf[k_cBufs][k_cBufLen];
	static int nBuf = 0;
	char * pchBuf = rgchBuf[nBuf];	// get pointer to current static buf
	nBuf ++;	// use next buffer for next call to this method
	nBuf %= k_cBufs;

	V_snprintf( pchBuf, k_cBufLen, "%llu", m_ulGameID );

	return pchBuf;
}

// static method to render a uint64 representation of a Game ID to a string
const char * CGameID::Render( uint64 ulGameID )
{
	CGameID nGameID( ulGameID );
	return nGameID.Render();
}
#endif
