/// Standalone plain C utilities for parsing and printing IPv6 addresses
#pragma once

#include <stdint.h>
#include <stdbool.h>

/// Max length of an IPv6 string, with scope, WITHOUT port number, including \0':
/// 0123:4567:89ab:cdef:0123:4567:89ab:cdef%4294967295
const int k_ncchMaxIPV6AddrStringWithoutPort = 51;

/// Max number of bytes output by IPv6AddrToString, including '\0':
/// [0123:4567:89ab:cdef:0123:4567:89ab:cdef%4294967295]:12345
/// There are other strings that are acceptable to ParseIPv6Addr
/// that are longer than this, but this is the longest canonical
/// string.
const int k_ncchMaxIPV6AddrStringWithPort = 59;

#ifdef __cplusplus
extern "C" {
#endif

/// Format an IPv6 address to the canonical form according to RFC5952.
/// The address should be 16 bytes (e.g. same as in6_addr::s6_addr).
/// Your buffer MUST be at least k_ncchMaxIPV6AddrStringWithoutPort bytes.
extern void IPv6IPToString( char *pszOutText, const unsigned char *ip );

/// Format IPv6 IP and port to string.  This uses the recommended
/// bracket notation, eg [1234::1]:12345.  Your buffer must be
/// at least k_ncchMaxIPV6AddrStringWithPort bytes.
extern void IPv6AddrToString( char *pszOutText, const unsigned char *ip, uint16_t port, uint32_t scope );

/// Parse IPv6 address string.  Returns true if parsed OK.  Returns false
/// if input cannot be parsed, or if input specifies a port but pOutPort is NULL.
/// If input does not specify a port, and pOutPort is non-NULL, then *pOutPort is
/// set to -1.
///
/// Parsing is tolerant of any unambiguous IPv6 representation, the input
/// need not be the canonical RFC5952 representation.
///
/// IPv6 zones are not supported.
///
/// Leading and trailing whitespace is OK around the entire string,
/// but not internal whitespace.  The different methods for separating the
/// port in RFC5952 are supported section 6, except the ambiguous case
/// of a colon to separate the port, when the IP contains a double-colon.
/// Brackets around an IP are OK, even if there is no port.
///
/// Address must point to a 16-byte buffer (e.g. same as in6_addr::s6_addr)
/// Port is returned in host byte order.
extern bool ParseIPv6Addr( const char *pszText, unsigned char *pOutIP, int *pOutPort, uint32_t *pOutScope );

#ifdef __cplusplus
}
#endif
