//========= Copyright Valve LLC, All rights reserved. ========================

// Note: not using precompiled headers! This file is included directly by
// several different projects and may include Crypto++ headers depending
// on compile-time defines, which in turn pulls in other odd dependencies
#include "crypto.h"
#include <tier0/vprof.h>
#include <tier1/utlbuffer.h>

//-----------------------------------------------------------------------------
// Purpose: Hex-encodes a block of data.  (Binary -> text representation.)  The output
//			is null-terminated and can be treated as a string.
// Input:	pubData -			Data to encode
//			cubData -			Size of data to encode
//			pchEncodedData -	Pointer to string buffer to store output in
//			cchEncodedData -	Size of pchEncodedData buffer
//-----------------------------------------------------------------------------
bool CCrypto::HexEncode( const void *pData, const uint32 cubData, char *pchEncodedData, uint32 cchEncodedData )
{
	VPROF_BUDGET( "CCrypto::HexEncode", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pData );
	Assert( cubData );
	Assert( pchEncodedData );
	Assert( cchEncodedData > 0 );

	if ( cchEncodedData < ( ( cubData * 2 ) + 1 ) )
	{
		Assert( cchEncodedData >= ( cubData * 2 ) + 1 );  // expands to 2x input + NULL, must have room in output buffer
		*pchEncodedData = '\0';
		return false;
	}

	const uint8 *pubData = (const uint8 *)pData;
	for ( uint32 i = 0; i < cubData; ++i )
	{
		uint8 c = *pubData++;
		*pchEncodedData++ = "0123456789ABCDEF"[c >> 4];
		*pchEncodedData++ = "0123456789ABCDEF"[c & 15];
	}
	*pchEncodedData = '\0';

	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Hex-decodes a block of data.  (Text -> binary representation.)  
// Input:	pchData -			Null-terminated hex-encoded string 
//			pubDecodedData -	Pointer to buffer to store output in
//			pcubDecodedData -	Pointer to variable that contains size of
//								output buffer.  At exit, is filled in with actual size
//								of decoded data.
//-----------------------------------------------------------------------------
bool CCrypto::HexDecode( const char *pchData, void *pDecodedData, uint32 *pcubDecodedData )
{
	VPROF_BUDGET( "CCrypto::HexDecode", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pchData );
	Assert( pDecodedData );
	Assert( pcubDecodedData );
	Assert( *pcubDecodedData );

	const char *pchDataOrig = pchData;

	// Crypto++ HexDecoder silently skips unrecognized characters. So we do the same.
	uint8 *pubDecodedData = (uint8 *)pDecodedData;
	uint32 cubOut = 0;
	uint32 cubOutMax = *pcubDecodedData;
	while ( cubOut < cubOutMax )
	{
		uint8 val = 0;
		while ( true ) // try to parse first hex character in a pair
		{
			uint8 c = (uint8)*pchData++;
			if ( (uint8)(c - '0') < 10 )
			{
				val = (uint8)(c - '0') * 16;
				break;
			}
			if ( (uint8)(c - 'a') < 6 )
			{
				val = (uint8)(c - 'a') * 16 + 160;
				break;
			}
			if ( (uint8)(c - 'A') < 6 )
			{
				val = (uint8)(c - 'A') * 16 + 160;
				break;
			}
			if ( c == 0 )
			{
				// end of input string. done decoding.
				*pcubDecodedData = cubOut;
				return true;
			}
			// some other character, ignore, keep trying to get first half.
		}

		while ( true ) // try to parse second hex character in a pair
		{
			uint8 c = (uint8)*pchData++;
			if ( (uint8)(c - '0') < 10 )
			{
				val += (uint8)(c - '0');
				break;
			}
			if ( (uint8)(c - 'a') < 6 )
			{
				val += (uint8)(c - 'a') + 10;
				break;
			}
			if ( (uint8)(c - 'A') < 6 )
			{
				val += (uint8)(c - 'A') + 10;
				break;
			}
			if ( c == 0 )
			{
				// end of input string. done decoding. half-byte 'val' is discarded.
				*pcubDecodedData = cubOut;
				return true;
			}
			// some other character, ignore. keep trying to get second half.
		}

		// got a full byte.
		pubDecodedData[cubOut++] = val;
	}

	if ( *pchData == 0 )
	{
		// end of input string simultaneous with end of output buffer. done decoding.
		*pcubDecodedData = cubOut;
		return true;
	}

	// Out of space.
	AssertMsg2( false, "CCrypto::HexDecode: insufficient output buffer (input length %u, output size %u)", (uint) V_strlen( pchDataOrig ), cubOutMax );
	return false;
}


static const int k_LineBreakEveryNGroups = 18; // line break every 18 groups of 4 characters (every 72 characters)

//-----------------------------------------------------------------------------
// Purpose: Returns the expected buffer size that should be passed to Base64Encode.
// Input:	cubData -			Size of data to encode
//			bInsertLineBreaks -	If line breaks should be inserted automatically
//-----------------------------------------------------------------------------
uint32 CCrypto::Base64EncodeMaxOutput( const uint32 cubData, const char *pszLineBreak )
{
	// terminating null + 4 chars per 3-byte group + line break after every 18 groups (72 output chars) + final line break
	uint32 nGroups = (cubData+2)/3;
	str_size cchRequired = 1 + nGroups*4 + ( pszLineBreak ? V_strlen(pszLineBreak)*(1+(nGroups-1)/k_LineBreakEveryNGroups) : 0 );
	return cchRequired;
}


//-----------------------------------------------------------------------------
// Purpose: Base64-encodes a block of data.  (Binary -> text representation.)  The output
//			is null-terminated and can be treated as a string.
// Input:	pubData -			Data to encode
//			cubData -			Size of data to encode
//			pchEncodedData -	Pointer to string buffer to store output in
//			cchEncodedData -	Size of pchEncodedData buffer
//			bInsertLineBreaks -	If "\n" line breaks should be inserted automatically
//-----------------------------------------------------------------------------
bool CCrypto::Base64Encode( const void *pubData, uint32 cubData, char *pchEncodedData, uint32 cchEncodedData, bool bInsertLineBreaks )
{
	const char *pszLineBreak = bInsertLineBreaks ? "\n" : NULL;
	uint32 cchRequired = Base64EncodeMaxOutput( cubData, pszLineBreak );
	AssertMsg2( cchEncodedData >= cchRequired, "CCrypto::Base64Encode: insufficient output buffer for encoding, needed %d got %d\n", cchRequired, cchEncodedData );
	return Base64Encode( pubData, cubData, pchEncodedData, &cchEncodedData, pszLineBreak );
}

//-----------------------------------------------------------------------------
// Purpose: Base64-encodes a block of data.  (Binary -> text representation.)  The output
//			is null-terminated and can be treated as a string.
// Input:	pubData -			Data to encode
//			cubData -			Size of data to encode
//			pchEncodedData -	Pointer to string buffer to store output in
//			pcchEncodedData -	Pointer to size of pchEncodedData buffer; adjusted to number of characters written (before NULL)
//			pszLineBreak -		String to be inserted every 72 characters; empty string or NULL pointer for no line breaks
// Note: if pchEncodedData is NULL and *pcchEncodedData is zero, *pcchEncodedData is filled with the actual required length
// for output. A simpler approximation for maximum output size is (cubData * 4 / 3) + 5 if there are no linebreaks.
//-----------------------------------------------------------------------------
bool CCrypto::Base64Encode( const void *pData, uint32 cubData, char *pchEncodedData, uint32* pcchEncodedData, const char *pszLineBreak )
{
	VPROF_BUDGET( "CCrypto::Base64Encode", VPROF_BUDGETGROUP_ENCRYPTION );

	if ( pchEncodedData == NULL )
	{
		AssertMsg( *pcchEncodedData == 0, "NULL output buffer with non-zero size passed to Base64Encode" );
		*pcchEncodedData = Base64EncodeMaxOutput( cubData, pszLineBreak );
		return true;
	}

	const uint8 *pubData = (const uint8 *)pData;
	const uint8 *pubDataEnd = pubData + cubData;
	char *pchEncodedDataStart = pchEncodedData;
	str_size unLineBreakLen = pszLineBreak ? V_strlen( pszLineBreak ) : 0;
	int nNextLineBreak = unLineBreakLen ? k_LineBreakEveryNGroups : INT_MAX;

	const char * const pszBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	uint32 cchEncodedData = *pcchEncodedData;
	if ( cchEncodedData == 0 )
		goto out_of_space;

	--cchEncodedData; // pre-decrement for the terminating null so we don't forget about it

	// input 3 x 8-bit, output 4 x 6-bit
	while ( pubDataEnd - pubData >= 3 )
	{
		if ( cchEncodedData < 4 + unLineBreakLen )
			goto out_of_space;

		if ( nNextLineBreak == 0 )
		{
			memcpy( pchEncodedData, pszLineBreak, unLineBreakLen );
			pchEncodedData += unLineBreakLen;
			cchEncodedData -= unLineBreakLen;
			nNextLineBreak = k_LineBreakEveryNGroups;
		}

		uint32 un24BitsData;
		un24BitsData  = (uint32) pubData[0] << 16;
		un24BitsData |= (uint32) pubData[1] << 8;
		un24BitsData |= (uint32) pubData[2];
		pubData += 3;

		pchEncodedData[0] = pszBase64Chars[ (un24BitsData >> 18) & 63 ];
		pchEncodedData[1] = pszBase64Chars[ (un24BitsData >> 12) & 63 ];
		pchEncodedData[2] = pszBase64Chars[ (un24BitsData >>  6) & 63 ];
		pchEncodedData[3] = pszBase64Chars[ (un24BitsData      ) & 63 ];
		pchEncodedData += 4;
		cchEncodedData -= 4;
		--nNextLineBreak;
	}

	// Clean up remaining 1 or 2 bytes of input, pad output with '='
	if ( pubData != pubDataEnd )
	{
		if ( cchEncodedData < 4 + unLineBreakLen )
			goto out_of_space;

		if ( nNextLineBreak == 0 )
		{
			memcpy( pchEncodedData, pszLineBreak, unLineBreakLen );
			pchEncodedData += unLineBreakLen;
			cchEncodedData -= unLineBreakLen;
		}

		uint32 un24BitsData;
		un24BitsData = (uint32) pubData[0] << 16;
		if ( pubData+1 != pubDataEnd )
		{
			un24BitsData |= (uint32) pubData[1] << 8;
		}
		pchEncodedData[0] = pszBase64Chars[ (un24BitsData >> 18) & 63 ];
		pchEncodedData[1] = pszBase64Chars[ (un24BitsData >> 12) & 63 ];
		pchEncodedData[2] = pubData+1 != pubDataEnd ? pszBase64Chars[ (un24BitsData >> 6) & 63 ] : '=';
		pchEncodedData[3] = '=';
		pchEncodedData += 4;
		cchEncodedData -= 4;
	}

	if ( unLineBreakLen )
	{
		if ( cchEncodedData < unLineBreakLen )
			goto out_of_space;
		memcpy( pchEncodedData, pszLineBreak, unLineBreakLen );
		pchEncodedData += unLineBreakLen;
		cchEncodedData -= unLineBreakLen;
	}

	*pchEncodedData = 0;
	*pcchEncodedData = pchEncodedData - pchEncodedDataStart;
	return true;

out_of_space:
	*pchEncodedData = 0;
	*pcchEncodedData = Base64EncodeMaxOutput( cubData, pszLineBreak );
	AssertMsg( false, "CCrypto::Base64Encode: insufficient output buffer (up to n*4/3+5 bytes required, plus linebreaks)" );
	return false;
}


//-----------------------------------------------------------------------------
// Purpose: Base64-decodes a block of data.  (Text -> binary representation.)  
// Input:	pchData -			Null-terminated hex-encoded string 
//			pubDecodedData -	Pointer to buffer to store output in
//			pcubDecodedData -	Pointer to variable that contains size of
//								output buffer.  At exit, is filled in with actual size
//								of decoded data.
// Note: if NULL is passed as the output buffer and *pcubDecodedData is zero, the function
// will calculate the actual required size and place it in *pcubDecodedData. A simpler upper
// bound on the required size is ( strlen(pchData)*3/4 + 1 ).
//-----------------------------------------------------------------------------
bool CCrypto::Base64Decode( const char *pchData, void *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters )
{
	return Base64Decode( pchData, ~0u, pubDecodedData, pcubDecodedData, bIgnoreInvalidCharacters );
}

//-----------------------------------------------------------------------------
// Purpose: Base64-decodes a block of data.  (Text -> binary representation.)  
// Input:	pchData -			base64-encoded string, null terminated
//			cchDataMax -		maximum length of string unless a null is encountered first
//			pubDecodedData -	Pointer to buffer to store output in
//			pcubDecodedData -	Pointer to variable that contains size of
//								output buffer.  At exit, is filled in with actual size
//								of decoded data.
// Note: if NULL is passed as the output buffer and *pcubDecodedData is zero, the function
// will calculate the actual required size and place it in *pcubDecodedData. A simpler upper
// bound on the required size is ( strlen(pchData)*3/4 + 2 ).
//-----------------------------------------------------------------------------
bool CCrypto::Base64Decode( const char *pchData, uint32 cchDataMax, void *pDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters )
{
	VPROF_BUDGET( "CCrypto::Base64Decode", VPROF_BUDGETGROUP_ENCRYPTION );

	uint8 *pubDecodedData = (uint8 *)pDecodedData;
	uint32 cubDecodedData = *pcubDecodedData;
	uint32 cubDecodedDataOrig = cubDecodedData;

	if ( pubDecodedData == NULL )
	{
		AssertMsg( *pcubDecodedData == 0, "NULL output buffer with non-zero size passed to Base64Decode" );
		cubDecodedDataOrig = cubDecodedData = ~0u;
	}

	// valid base64 character range: '+' (0x2B) to 'z' (0x7A)
	// table entries are 0-63, -1 for invalid entries, -2 for '='
	static const signed char rgchInvBase64[] = {
		62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
		-1, -1, -1, -2, -1, -1, -1,  0,  1,  2,  3,  4,  5,  6,  7,
		8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
		23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31,
		32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46,
		47, 48, 49, 50, 51
	};
	COMPILE_TIME_ASSERT( V_ARRAYSIZE(rgchInvBase64) == 0x7A - 0x2B + 1 );

	uint32 un24BitsWithSentinel = 1;
	while ( cchDataMax-- > 0 )
	{
		char c = *pchData++;

		if ( (uint8)(c - 0x2B) >= V_ARRAYSIZE( rgchInvBase64 ) )
		{
			if ( c == '\0' )
				break;

			if ( !bIgnoreInvalidCharacters && !( c == '\r' || c == '\n' || c == '\t' || c == ' ' ) )
				goto decode_failed;
			else
				continue;
		}

		c = rgchInvBase64[(uint8)(c - 0x2B)];
		if ( (signed char)c < 0 )
		{
			if ( (signed char)c == -2 ) // -2 -> terminating '='
				break;

			if ( !bIgnoreInvalidCharacters )
				goto decode_failed;
			else
				continue;
		}

		un24BitsWithSentinel <<= 6;
		un24BitsWithSentinel |= c;
		if ( un24BitsWithSentinel & (1<<24) )
		{
			if ( cubDecodedData < 3 ) // out of space? go to final write logic
				break;
			if ( pubDecodedData )
			{
				pubDecodedData[0] = (uint8)( un24BitsWithSentinel >> 16 );
				pubDecodedData[1] = (uint8)( un24BitsWithSentinel >> 8);
				pubDecodedData[2] = (uint8)( un24BitsWithSentinel );
				pubDecodedData += 3;
			}
			cubDecodedData -= 3;
			un24BitsWithSentinel = 1;
		}
	}

	// If un24BitsWithSentinel contains data, output the remaining full bytes
	if ( un24BitsWithSentinel >= (1<<6) )
	{
		// Possibilities are 3, 2, 1, or 0 full output bytes.
		int nWriteBytes = 3;
		while ( un24BitsWithSentinel < (1<<24) )
		{
			nWriteBytes--;
			un24BitsWithSentinel <<= 6;
		}

		// Write completed bytes to output
		while ( nWriteBytes-- > 0 )
		{
			if ( cubDecodedData == 0 )
			{
				AssertMsg( false, "CCrypto::Base64Decode: insufficient output buffer (up to n*3/4+2 bytes required)" );
				goto decode_failed;
			}
			if ( pubDecodedData )
			{
				*pubDecodedData++ = (uint8)(un24BitsWithSentinel >> 16);
			}
			--cubDecodedData;
			un24BitsWithSentinel <<= 8;
		}
	}

	*pcubDecodedData = cubDecodedDataOrig - cubDecodedData;
	return true;

decode_failed:
	*pcubDecodedData = cubDecodedDataOrig - cubDecodedData;
	return false;
}

inline bool BParsePEMHeaderOrFooter( const char *&pchPEM, const char *pchEnd, const char *pszBeginOrEnd, const char *pszExpectedType )
{
	// Eat any leading whitespace of any kind
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
		if ( !V_isspace( *pchPEM ) )
			break;
		++pchPEM;
	}

	// Require at least one dash, and each any number of them
	if ( pchPEM >= pchEnd || *pchPEM != '-' )
		return false;
	while ( *pchPEM == '-' )
	{
		++pchPEM;
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
	}

	// Eat tabs and spaces
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
		if ( *pchPEM != ' ' && *pchPEM != '\t' )
			break;
		++pchPEM;
	}

	// Require and eat the word "BEGIN" or "END"
	int l = V_strlen( pszBeginOrEnd );
	if ( pchPEM + l >= pchEnd || V_strnicmp( pchPEM, pszBeginOrEnd, l ) != 0 )
		return false;
	pchPEM += l;

	// Eat tabs and spaces
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
		if ( *pchPEM != ' ' && *pchPEM != '\t' )
			break;
		++pchPEM;
	}

	// Remember where the type field was
	const char *pszType = pchPEM;

	// Skip over the type, to the ending dashes.
	// Fail if we hit end of input or end of line before we find the dash
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' || *pchPEM == '\r' || *pchPEM == '\n' )
			return false;
		if ( *pchPEM == '-' )
			break;
		++pchPEM;
	}

	// Confirm the type is what they expected
	if ( pszExpectedType )
	{
		int cchExpectedType = V_strlen( pszExpectedType );
		if ( pszType + cchExpectedType > pchPEM || V_strnicmp( pszType, pszExpectedType, cchExpectedType ) != 0 )
			return false;
	}

	// Eat any remaining dashes
	while ( pchPEM < pchEnd && *pchPEM == '-' )
		++pchPEM;

	// Eat any trailing whitespace of any kind
	while ( pchPEM < pchEnd && V_isspace( *pchPEM ) )
		++pchPEM;

	// OK
	return true;
}

//-----------------------------------------------------------------------------
const char *CCrypto::LocatePEMBody( const char *pchPEM, uint32 *pcch, const char *pszExpectedType )
{
	if ( !pchPEM || !pcch || !*pcch )
		return nullptr;

	const char *pchEnd = pchPEM + *pcch;
	if ( !BParsePEMHeaderOrFooter( pchPEM, pchEnd, "BEGIN", pszExpectedType ) )
		return nullptr;

	const char *pchBody = pchPEM;

	// Scan until we hit a character that isn't a valid
	for (;;)
	{
		if ( pchPEM >= pchEnd )
			return nullptr;
		if ( *pchPEM == '-' )
			break;
		++pchPEM;
	}

	// Save size of body
	uint32 cchBody = pchPEM - pchBody;

	// Eat the footer
	if ( !BParsePEMHeaderOrFooter( pchPEM, pchEnd, "END", pszExpectedType ) )
		return nullptr;

	// Should we check for leftover garbage here?
	// let's just ignore it.

	// Success, return body location and size to caller
	*pcch = cchBody;
	return pchBody;
}

//-----------------------------------------------------------------------------
bool CCrypto::DecodeBase64ToBuf( const char *pszEncoded, uint32 cbEncoded, CUtlBuffer &buf )
{
	uint32 cubDecodeSize = cbEncoded * 3 / 4 + 1;
	buf.EnsureCapacity( cubDecodeSize );
	if ( !CCrypto::Base64Decode( pszEncoded, cbEncoded, (uint8*)buf.Base(), &cubDecodeSize ) )
		return false;
	buf.SeekPut( CUtlBuffer::SEEK_HEAD, cubDecodeSize );
	return true;
}

//-----------------------------------------------------------------------------
bool CCrypto::DecodePEMBody( const char *pszPem, uint32 cch, CUtlBuffer &buf, const char *pszExpectedType )
{
	const char *pszBody = CCrypto::LocatePEMBody( pszPem, &cch, pszExpectedType );
	if ( !pszBody )
		return false;
	return DecodeBase64ToBuf( pszBody, cch, buf );
}

