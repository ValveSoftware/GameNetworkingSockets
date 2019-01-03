#include <stdio.h>
#include <string.h>
#include "ipv6text.h"

#ifdef _WIN32
#ifndef snprintf
#define snprintf _snprintf
#endif
#endif

void IPv6IPToString( char *pszOutText, const unsigned char *ip )
{

	// Find the longest run of consecutive zero quads.
	// If there's a tie, we want the leftmost one.
	int idxLongestRunStart = -1;
	int nLongestRun = 1; // It must be at least 2 quads in a row, a single 0 must not be compressed
	int nCurrentRun = 0;
	int idxQuad;
	for ( idxQuad = 0 ; idxQuad < 8 ; ++idxQuad )
	{
		// Zero
		if ( ip[idxQuad*2] || ip[idxQuad*2 + 1] )
		{
			// Terminate run
			nCurrentRun = 0;
		}
		else
		{
			// Extend (or begin) run
			++nCurrentRun;

			// Longer than previously found run?
			if ( nCurrentRun > nLongestRun )
			{
				nLongestRun = nCurrentRun;
				idxLongestRunStart = idxQuad - nCurrentRun + 1;
			}
		}
	}

	// Print the quads
	char *p = pszOutText;
	idxQuad = 0;
	bool bNeedColon = false;
	while ( idxQuad < 8 )
	{
		// Run of compressed zeros?
		if ( idxQuad == idxLongestRunStart )
		{
			*(p++) = ':';
			*(p++) = ':';
			bNeedColon = false;
			idxQuad += nLongestRun;
		}
		else
		{
			// Colon to separate from previous, unless
			// we are first or immediately follow compressed zero "::"
			if ( bNeedColon )
				*(p++) = ':';

			// Next quad should should print a separator
			bNeedColon = true;

			// Assemble 16-bit quad value from the two bytes
			unsigned quad = ( (unsigned)ip[idxQuad*2] << 8U ) | ip[idxQuad*2 + 1];

			// Manually do the hex number formatting.
			// Lowercase hex digits, with leading zeros omitted
			static const char hexdigits[] = "0123456789abcdef";
			if ( quad >= 0x0010 )
			{
				if ( quad >= 0x0100 )
				{
					if ( quad >= 0x1000 )
						*(p++) = hexdigits[ quad >> 12U ];
					*(p++) = hexdigits[ ( quad >> 8U ) & 0xf ];
				}
				*(p++) = hexdigits[ ( quad >> 4U ) & 0xf ];
			}

			// Least significant digit, which is always printed
			*(p++) = hexdigits[ quad & 0xf ];

			// On to the next one
			++idxQuad;
		}
	}

	// String terminator
	*p = '\0';
}

void IPv6AddrToString( char *pszOutText, const unsigned char *ip, uint16_t port, uint32_t scope )
{
	char *p = pszOutText;

	// Open bracket
	*(p++) = '[';

	// Print in the IP
	IPv6IPToString( p, ip );

	// Find the end of the string
	while (*p)
		++p;

	if ( scope )
	{
		// And now the scope.  Max 32-digit scope number is 10 digits
		snprintf( p, 12, "%%%d", scope );

		// Find the end of the string
		while (*p)
			++p;
	}

	// And now the rest.  Max 16-digit port number is 6 digits
	snprintf( p, 8, "]:%u", (unsigned int)port );
}

static inline int ParseIPv6Addr_HexDigitVal( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - ('a' - 0xa);
	if ( c >= 'A' && c <= 'F' ) return c - ('A' - 0xa);
	return -1;
}
static inline int ParseIPv6Addr_DecimalDigitVal( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	return -1;
}
bool ParseIPv6Addr_IsSpace( char c )
{
	// Newlines don't count, intentionally
	return c == ' ' || c == '\t';
}
bool ParseIPv6Addr( const char *pszText, unsigned char *pOutIP, int *pOutPort, uint32_t *pOutScope )
{
	while ( ParseIPv6Addr_IsSpace( *pszText ) )
		++pszText;
	const char *s = pszText;

	// Skip opening bracket, if present
	if ( *s == '[' )
	{
		++s;
		while ( ParseIPv6Addr_IsSpace( *s ) )
			++s;
	}

	// Special case for leading "::"
	bool bQuadMustFollow = true;
	unsigned char *d = pOutIP;
	unsigned char *pZeroFill = NULL;
	unsigned char *pEndIP = pOutIP + 16;
	if ( s[0] == ':' && s[1] == ':' )
	{
		pZeroFill = d;
		s += 2;
		bQuadMustFollow = false;
	}

	// Parse quads until we get to the end
	for (;;)
	{
		// Next thing must be a quad, or end of input.  Is it a quad?
		int quadDigit = ParseIPv6Addr_HexDigitVal( *s );
		if ( quadDigit < 0 )
		{
			if ( bQuadMustFollow )
				return false;
			break;
		}

		// No room for more quads?
		if ( d >= pEndIP )
			return false;

		++s;
		int quad = quadDigit;

		// Now parse up to three additional characters
		quadDigit = ParseIPv6Addr_HexDigitVal( *s );
		if ( quadDigit >= 0 )
		{
			quad = ( quad << 4 ) | quadDigit;
			++s;

			quadDigit = ParseIPv6Addr_HexDigitVal( *s );
			if ( quadDigit >= 0 )
			{
				quad = ( quad << 4 ) | quadDigit;
				++s;

				quadDigit = ParseIPv6Addr_HexDigitVal( *s );
				if ( quadDigit >= 0 )
				{
					quad = ( quad << 4 ) | quadDigit;
					++s;
				}
			}
		}

		// Stash it in the next slot, ignoring for now the issue
		// of compressed zeros
		*(d++) = (unsigned char)( quad >> 8 );
		*(d++) = (unsigned char)quad;

		// Only valid character for the IP portion is a colon.
		// Anything else ends the IP portion
		if ( *s != ':' )
			break;

		// Compressed zeros?
		if ( s[1] == ':' )
		{

			// Eat '::'
			s += 2;

			// Can only have one range of compressed zeros
			if ( pZeroFill )
				return false;

			// Remember where to insert the compressed zeros
			pZeroFill = d;

			// An IP can end with '::'
			bQuadMustFollow = false;
		}
		else
		{
			// If they have filed the entire IP with no compressed zeros,
			// then this is unambiguously a port number.  That's not
			// necessarily the best style, but it *is* unambiguous
			// what it should mean, so let's allow it.  If there
			// are compressed zeros, then this is ambiguous, and we will
			// always interpret it as a quad.
			if ( !pZeroFill && d >= pEndIP )
				break; // leave ':' as next character, for below

			// Eat ':'
			++s;

			// A single colon must be followed by another quad
			bQuadMustFollow = true;
		}
	}

	// End of the IP.  Do we have compressed zeros?
	if ( pZeroFill )
	{
		// How many zeros do we need to fill?
		intptr_t nZeros = pEndIP - d;
		if ( nZeros <= 0 )
			return false;

		// Shift the quads after the bytes to the end
		memmove( pZeroFill+nZeros, pZeroFill, d-pZeroFill );

		// And now fill the zeros
		memset( pZeroFill, 0, nZeros );
	}
	else
	{
		// No compressed zeros.  Just make sure we filled the IP exactly
		if ( d != pEndIP )
			return false;
	}

	if ( *s == '%' )
	{
		++s;
        
		// Parse scope number
		uint32_t unScope = 0;
		int nScopeDigit = ParseIPv6Addr_DecimalDigitVal( *s );
		if ( nScopeDigit < 0 )
			return false;
		unScope = (uint32_t)nScopeDigit;
		for (;;)
		{
			++s;
			if ( *s == '\0' || *s == ']' || ParseIPv6Addr_IsSpace( *s ) )
				break;
			nScopeDigit = ParseIPv6Addr_DecimalDigitVal( *s );
			if ( nScopeDigit < 0 )
				return false;
			unScope = unScope * 10 + nScopeDigit;
		}

		if ( pOutScope )
			*pOutScope = unScope;
	}
	else
	{
		if ( pOutScope )
			*pOutScope = 0;
	}

	// If we started with a bracket, then the next character MUST be a bracket.
	// (And this is the only circumstance in which a closing bracket would be legal)
	if ( *pszText == '[' )
	{
		while ( ParseIPv6Addr_IsSpace( *s ) )
			++s;
		if ( *s != ']' )
			return false;
		++s;
	}

	// Now we are definitely at the end of the IP.  Do we have a port?
	// We support all of the syntaxes mentioned in RFC5952 section 6 other
	// than the ambiguous case
	if ( *s == ':' || *s == '#' || *s == '.' || *s == 'p' || *s == 'P' )
	{
		++s;
	}
	else
	{
		while ( ParseIPv6Addr_IsSpace( *s ) )
			++s;
		if ( *s == '\0' )
		{
			// Parsed IP without port OK
			if ( pOutPort )
				*pOutPort = -1;
			return true;
		}

		if ( strncmp( s, "port", 4 ) == 0 )
		{
			s += 4;
			while ( ParseIPv6Addr_IsSpace( *s ) )
				++s;
		}
		else
		{
			// Extra stuff after the IP which isn't whitespace or a port
			return false;
		}
	}

	// We have a port.  If they didn't ask for it, that's considered a parse failure.
	if ( !pOutPort )
		return false;

	// Parse port number
	int nPort = ParseIPv6Addr_DecimalDigitVal( *s );
	if ( nPort < 0 )
		return false;
	for (;;)
	{
		++s;
		if ( *s == '\0' || ParseIPv6Addr_IsSpace( *s ) )
			break;
		int portDigit = ParseIPv6Addr_DecimalDigitVal( *s );
		if ( portDigit < 0 )
			return false;
		nPort = nPort * 10 + portDigit;
		if ( nPort > 0xffff )
			return false;
	}

	// Consume trailing whitespace; confirm nothing else in the input
	while ( ParseIPv6Addr_IsSpace( *s ) )
		++s;
	if ( *s != '\0' )
		return false;

	*pOutPort = nPort;
	return true;
}

