//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Implementation of (the most important subset of) the ICE protocol
//
// https://datatracker.ietf.org/doc/html/rfc8489

#include "steamnetworkingsockets_ice_client.h"
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#include "csteamnetworkingsockets.h"
#include <tier0/platform_sockets.h>
#include "crypto.h"
#include "steamnetworkingsockets_mock.h"

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

namespace {

static void ConvertNetAddr_tToSteamNetworkingIPAddr( const netadr_t& in, SteamNetworkingIPAddr *pOut );
static void ConvertSteamNetworkingIPAddrToNetAdr_t( const SteamNetworkingIPAddr& in, netadr_t *pOut );
static uint32 CRC32( const unsigned char *buf, int len );

// Returns true if remoteAddr falls on the same LAN as localAddr/nPrefixLen.
// Requires the local address to be in private/reserved IP space; this prevents
// two hosts with public datacenter IPs on a shared subnet from being mistakenly
// classified as a fast LAN hop.
static bool IsRemoteAddressOnLocalSubnet( const SteamNetworkingIPAddr &localAddr, int nPrefixLen, const SteamNetworkingIPAddr &remoteAddr )
{
    if ( nPrefixLen <= 0 )
        return false;

    if ( localAddr.IsIPv4() )
    {
        if ( !remoteAddr.IsIPv4() )
            return false;

        uint32 local = localAddr.GetIPv4();
        uint8 a = (uint8)( local >> 24 );
        uint8 b = (uint8)( local >> 16 );
        bool bPrivate = ( a == 127 )                           // loopback (includes mock network LANs)
            || ( a == 10 )                                     // RFC 1918 10/8
            || ( a == 172 && b >= 16 && b <= 31 )             // RFC 1918 172.16/12
            || ( a == 192 && b == 168 );                       // RFC 1918 192.168/16
        if ( !bPrivate )
            return false;

        uint32 mask = ( nPrefixLen >= 32 ) ? ~0u : ~( ~0u >> nPrefixLen );
        return ( local & mask ) == ( remoteAddr.GetIPv4() & mask );
    }
    else
    {
        if ( remoteAddr.IsIPv4() )
            return false;

        const uint8 *pLocal  = localAddr.m_ipv6;
        const uint8 *pRemote = remoteAddr.m_ipv6;

        // Only ULA (fc00::/7) is the IPv6 equivalent of private RFC 1918 space.
        if ( pLocal[0] != 0xfc && pLocal[0] != 0xfd )
            return false;

        int nFullBytes = nPrefixLen / 8;
        int nRemBits   = nPrefixLen % 8;
        if ( nFullBytes > 16 ) nFullBytes = 16;
        if ( memcmp( pLocal, pRemote, nFullBytes ) != 0 )
            return false;
        if ( nRemBits > 0 && nFullBytes < 16 )
        {
            uint8 mask = (uint8)( 0xFF << ( 8 - nRemBits ) );
            if ( ( pLocal[nFullBytes] & mask ) != ( pRemote[nFullBytes] & mask ) )
                return false;
        }
        return true;
    }
}



/* After the STUN header are zero or more attributes.  Each attribute
   MUST be TLV encoded, with a 16-bit type, 16-bit length, and value.
   Each STUN attribute MUST end on a 32-bit boundary.  As mentioned
   above, all fields in an attribute are transmitted most significant
   bit first.

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |         Type                  |            Length             |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                         Value (variable)                ....
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
static const uint32* DecodeSTUNAttribute( const uint32 *pData, const uint32 *pDataEnd, STUNAttribute* pAttribute )
{
    if ( pData == nullptr || pAttribute == nullptr )
        return nullptr;

    const uint32 nHeaderWord = ntohl( pData[0] );
    const uint32 nType = ( nHeaderWord >> 16 ) & 0xFFFF;
    const uint32 nLength = nHeaderWord & 0xFFFF;

    const uint32 *pDataNext = pData + 1 + (( nLength + 3 ) / 4 );
    if ( pDataNext > pDataEnd )
        return nullptr;

    pAttribute->m_nType = nType;
    pAttribute->m_nLength = nLength;
    pAttribute->m_pData = &pData[1];
    return pDataNext;
}

static uint32* WriteGenericSTUNAttribute( uint32 *pData, STUNAttribute* pAttribute )
{
    if ( pData == nullptr || pAttribute == nullptr )
        return pData;
    pData[0] = htonl( ( ( pAttribute->m_nType & 0xFFFF ) << 16 ) | ( pAttribute->m_nLength & 0xFFFF ) );
    ++pData;
    V_memcpy( pData, pAttribute->m_pData, pAttribute->m_nLength );
    byte* pDataByte = (byte*)pData;
    for( uint32 i = pAttribute->m_nLength; ( i & 3 ) != 0; ++i )
    {
        pDataByte[i] = 0;
    }
    return pData + ( pAttribute->m_nLength + 3 ) / 4;
}

static bool ReadMappedAddress( const STUNAttribute *pAttr, SteamNetworkingIPAddr* pAddr )
{
    if ( pAttr == nullptr || pAddr == nullptr )
        return false;
    if ( pAttr->m_nType != k_nSTUN_Attr_MappedAddress )
        return false;

    /*     The format of the MAPPED-ADDRESS attribute is:

       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |0 0 0 0 0 0 0 0|    Family     |           Port                |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                                                               |
      |                 Address (32 bits or 128 bits)                 |
      |                                                               |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    The address family can take on the following values:
        0x01:IPv4
        0x02:IPv6   */

    if ( pAttr->m_nLength != 8 && pAttr->m_nLength != 20 )
        return false;

    const uint32 nFamily = ( ( ntohl( pAttr->m_pData[0] ) >> 16 ) & 0xF );
    const uint32 nPort = (ntohl( pAttr->m_pData[0] ) & 0xFFFF ) ;
    if ( pAttr->m_nLength == 8 && nFamily == 0x1 )
    {
        const uint32 uIPv4 = ntohl( pAttr->m_pData[1] );
        pAddr->SetIPv4( uIPv4, nPort );
        return true;
    }
    else if ( pAttr->m_nLength == 20 && nFamily == 0x2 )
    {
        pAddr->SetIPv6( reinterpret_cast<const uint8 *>( &pAttr->m_pData[1] ), nPort );
        return true;
    }
    else
    {
        return false;
    }
}


static uint32* WriteMappedAddress( uint32* pBuffer, const SteamNetworkingIPAddr& localAddr, const uint32* pTransactionID )
{
    /*   The format of the MAPPED-ADDRESS is:

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |0 0 0 0 0 0 0 0|    Family     |         X-Port                |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                Address (Variable)
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    The address family can take on the following values:
        0x01:IPv4
        0x02:IPv6   */
    if ( localAddr.IsIPv4() )
    {
        pBuffer[0] = htonl( ( k_nSTUN_Attr_MappedAddress << 16 ) | 8 );
        pBuffer[1] = htonl( ( 0x01 << 16 ) | ((uint32)localAddr.m_port) );
        pBuffer[2] = htonl( localAddr.GetIPv4() );
        return &pBuffer[3];
    }
    else
    {
        pBuffer[0] = htonl( ( k_nSTUN_Attr_MappedAddress << 16 ) | 20 );
        pBuffer[1] = htonl( ( 0x02 << 16 ) | ((uint32)localAddr.m_port) );
        V_memcpy( &pBuffer[2], localAddr.m_ipv6, 16 ); // m_ipv6 is in network byte order.
        return &pBuffer[6];
    }
}

// Decode any XOR-encoded address attribute (XOR-MAPPED-ADDRESS, XOR-RELAYED-ADDRESS, etc.).
// Does NOT check the attribute type — callers are responsible for passing the right attr.
static bool ReadXORAddressAttribute( const STUNAttribute *pAttr, const STUNHeader *pHeader, SteamNetworkingIPAddr* pAddr )
{
    if ( pAttr == nullptr || pHeader == nullptr || pAddr == nullptr )
        return false;

    if ( pAttr->m_nLength != 8 && pAttr->m_nLength != 20 )
        return false;

    const uint32 nFamily = ( ( ntohl( pAttr->m_pData[0] ) >> 16 ) & 0xF );
    const uint32 nPort = ( ntohl( pAttr->m_pData[0] ) & 0xFFFF ) ^ ( k_nSTUN_CookieValue >> 16 );
    if ( pAttr->m_nLength == 8 && nFamily == 0x1 )
    {
        const uint32 uIPv4 = ntohl( pAttr->m_pData[1] ) ^ k_nSTUN_CookieValue;
        pAddr->SetIPv4( uIPv4, nPort );
        return true;
    }
    else if ( pAttr->m_nLength == 20 && nFamily == 0x2 )
    {
        uint32 uXORBuffer[] = {
            pAttr->m_pData[1] ^ htonl( k_nSTUN_CookieValue ),
            pAttr->m_pData[2] ^ pHeader->m_nTransactionID[0],
            pAttr->m_pData[3] ^ pHeader->m_nTransactionID[1],
            pAttr->m_pData[4] ^ pHeader->m_nTransactionID[2] };
        pAddr->SetIPv6( reinterpret_cast<const uint8 *>( uXORBuffer ), nPort );
        return true;
    }
    else
    {
        return false;
    }
}

static bool ReadXORMappedAddress( const STUNAttribute *pAttr, const STUNHeader *pHeader, SteamNetworkingIPAddr* pAddr )
{
    if ( pAttr == nullptr || pHeader == nullptr || pAddr == nullptr )
        return false;
    if ( pAttr->m_nType != k_nSTUN_Attr_XORMappedAddress )
        return false;

    /*   The format of the XOR-MAPPED-ADDRESS is:

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |0 0 0 0 0 0 0 0|    Family     |         X-Port                |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                X-Address (Variable)
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    The address family can take on the following values:
        0x01:IPv4
        0x02:IPv6   */

    if ( pAttr->m_nLength != 8 && pAttr->m_nLength != 20 )
        return false;

    const uint32 nFamily = ( ( ntohl( pAttr->m_pData[0] ) >> 16 ) & 0xF );
    const uint32 nPort = ( ntohl( pAttr->m_pData[0] ) & 0xFFFF ) ^ ( k_nSTUN_CookieValue >> 16 );
    if ( pAttr->m_nLength == 8 && nFamily == 0x1 )
    {
        const uint32 uIPv4 = ntohl( pAttr->m_pData[1] ) ^ k_nSTUN_CookieValue;
        pAddr->SetIPv4( uIPv4, nPort );
        return true;
    }
    else if ( pAttr->m_nLength == 20 && nFamily == 0x2 )
    {
        uint32 uXORBuffer[] = {
            pAttr->m_pData[1] ^ htonl( k_nSTUN_CookieValue ),
            pAttr->m_pData[2] ^ pHeader->m_nTransactionID[0],
            pAttr->m_pData[3] ^ pHeader->m_nTransactionID[1],
            pAttr->m_pData[4] ^ pHeader->m_nTransactionID[2] };
        pAddr->SetIPv6( reinterpret_cast<const uint8 *>( uXORBuffer ), nPort );
        return true;
    }
    else
    {
        return false;
    }
}

static uint32* WriteXORMappedAddress( uint32* pBuffer, const SteamNetworkingIPAddr& localAddr, const uint32* pTransactionID )
{
    /*   The format of the XOR-MAPPED-ADDRESS is:

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |0 0 0 0 0 0 0 0|    Family     |         X-Port                |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                X-Address (Variable)
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    The address family can take on the following values:
        0x01:IPv4
        0x02:IPv6   */
    const uint32 nXORPort = ((uint32)localAddr.m_port) ^ ( k_nSTUN_CookieValue >> 16 );
    if ( localAddr.IsIPv4() )
    {
        pBuffer[0] = htonl( ( k_nSTUN_Attr_XORMappedAddress << 16 ) | 8 );
        pBuffer[1] = htonl( ( 0x01 << 16 ) | nXORPort );
        pBuffer[2] = htonl( localAddr.GetIPv4() ^ k_nSTUN_CookieValue );
        return &pBuffer[3];
    }
    else
    {
        pBuffer[0] = htonl( ( k_nSTUN_Attr_XORMappedAddress << 16 ) | 20 );
        pBuffer[1] = htonl( ( 0x02 << 16 ) | nXORPort );
        V_memcpy( &pBuffer[2], localAddr.m_ipv6, 16 ); // m_ipv6 is in network byte order.
        pBuffer[2] ^= htonl( k_nSTUN_CookieValue );
        pBuffer[3] ^= pTransactionID[0];    // TransactionID is just treated as opqaue bits in network order.
        pBuffer[4] ^= pTransactionID[1];
        pBuffer[5] ^= pTransactionID[2];
        return &pBuffer[6];
    }
}

static bool ReadAnyMappedAddress( const STUNAttribute *pAttrs, uint32 nAttributes, const STUNHeader *pHeader, SteamNetworkingIPAddr* pAddr )
{
    if ( pAddr == nullptr || pAttrs == nullptr || nAttributes == 0 )
        return false;

    bool bResult = false;
    for ( uint32 i = 0; i < nAttributes; i++ )
    {
        if ( pAttrs[i].m_nType == k_nSTUN_Attr_MappedAddress )
        {
            bResult = ReadMappedAddress( &pAttrs[i], pAddr );
        }
        else if ( pAttrs[i].m_nType == k_nSTUN_Attr_XORMappedAddress )
        {
            bResult = ReadXORMappedAddress( &pAttrs[i], pHeader, pAddr );
        }
    }
    return bResult;
}

static const STUNAttribute* FindAttributeOfType( const STUNAttribute *pAttrs, uint32 nAttributes, const uint32 nType )
{
    if ( pAttrs == nullptr || nAttributes == 0 )
        return nullptr;

    for ( uint32 i = 0; i < nAttributes; i++ )
    {
        if ( pAttrs[i].m_nType == nType )
            return &pAttrs[i];
    }

    return nullptr;
}


static uint32* ReserveFingerprintAttribute( uint32 *pBuffer )
{
    pBuffer[0] = htonl( ( k_nSTUN_Attr_Fingerprint << 16 ) | 4 );
    return pBuffer + 2;
}

static uint32* WriteFingerprintAttribute( uint32 *pBuffer, uint32 *pMessageStart )
{
    pBuffer[0] = htonl( ( k_nSTUN_Attr_Fingerprint << 16 ) | 4 );
    pBuffer[1] = htonl( 0x5354554e ^ CRC32( reinterpret_cast<unsigned char*>( pMessageStart ), uint32( pBuffer - pMessageStart ) * 4 ) );
    return &pBuffer[2];
}


static uint32* ReserveMessageIntegritySHA256Attribute( uint32 *pBuffer )
{
    pBuffer[0] = htonl( ( k_nSTUN_Attr_MessageIntegrity_SHA256 << 16 ) | k_cubSHA256Hash );
    return pBuffer + 1 + ( k_cubSHA256Hash / 4 );
}

static uint32* WriteMessageIntegritySHA256Attribute( uint32 *pBuffer, uint32 *pMessageStart, const uint8 *pubKey, uint32 cubKey )
{
    SHA256Digest_t digest;
  	CCrypto::GenerateHMAC256( reinterpret_cast<const uint8 *>( pMessageStart ), 4 * (pBuffer - pMessageStart ), pubKey, cubKey, &digest );

    pBuffer[0] = htonl( ( k_nSTUN_Attr_MessageIntegrity_SHA256 << 16 ) | k_cubSHA256Hash );
    V_memcpy( &pBuffer[1], digest, k_cubSHA256Hash );
    return pBuffer + 1 + ( k_cubSHA256Hash / 4 );
}


static uint32* ReserveMessageIntegrityAttribute( uint32 *pBuffer )
{
    pBuffer[0] = htonl( ( k_nSTUN_Attr_MessageIntegrity << 16 ) | k_cubSHA1Hash );
    return pBuffer + 1 + ( k_cubSHA1Hash / 4 );
}

static uint32* WriteMessageIntegrityAttribute( uint32 *pBuffer, uint32 *pMessageStart, const uint8 *pubKey, uint32 cubKey )
{
    Assert( pubKey != nullptr );
    Assert( cubKey != 0 );

    SHADigest_t digest;
  	CCrypto::GenerateHMAC( reinterpret_cast<const uint8 *>( pMessageStart ), 4 * (pBuffer - pMessageStart ), pubKey, cubKey, &digest );

    pBuffer[0] = htonl( ( k_nSTUN_Attr_MessageIntegrity << 16 ) | k_cubSHA1Hash );
    V_memcpy( &pBuffer[1], digest, k_cubSHA1Hash );
    return pBuffer + 1 + ( k_cubSHA1Hash / 4 );
}

// Parse STUN attributes and verify message integrity/fingerprint.
// pubKey/cubKey may be null/0 to skip integrity verification.
static bool ParseSTUNAttributes( const RecvPktInfo_t &info, const uint8 *pubKey, uint32 cubKey, CUtlVector< STUNAttribute >* pVecAttrs )
{
    const uint32 * const pMessage = reinterpret_cast< const uint32* >( info.m_pPkt );
    const uint32 * const pMessageEnd = pMessage + info.m_cbPkt / 4;
    const uint32 *pAttrPtr = &pMessage[5];
    while ( pAttrPtr < pMessageEnd )
    {
        STUNAttribute attr;
        const uint32 * const pThisAttrPtr = pAttrPtr;
        pAttrPtr = DecodeSTUNAttribute( pAttrPtr, pMessageEnd, &attr );
        if ( pAttrPtr == nullptr )
            break;
        if ( pVecAttrs != nullptr )
            pVecAttrs->AddToTail( attr );
        switch ( attr.m_nType )
        {
            case k_nSTUN_Attr_Fingerprint:
            {
                if ( attr.m_nLength != 4 )
                    return false;
                const uint32 uPacketCRC = ntohl( attr.m_pData[0] ) ^ 0x5354554e;
                const uint32 uDataCRC = CRC32( reinterpret_cast<const unsigned char*>( pMessage ), uint32( pThisAttrPtr - pMessage ) * 4 );
                if ( uPacketCRC != uDataCRC )
                    return false;
                break;
            }

            case k_nSTUN_Attr_MessageIntegrity_SHA256:
            {
                if ( attr.m_nLength != k_cubSHA256Hash )
                    return false;
                if ( cubKey == 0 )
                {
                    //SpewWarningRateLimited( SteamNetworkingSockets_GetLocalTimestamp(), "[%s] Received STUN packet with MessageIntegrity-SHA256 but no key to verify it\n", CUtlNetAdrRender( info.m_adrFrom ).String() );
                    return false;
                }
                const uint32 uOriginalStartWordRaw256 = *pMessage;
                const uint32 uAdjustedLength256 = 4*( pThisAttrPtr - &pMessage[5] ) + 4 + attr.m_nLength;
                *(uint32*)pMessage = htonl( ( ntohl( uOriginalStartWordRaw256 ) & 0xFFFF0000ul ) | uAdjustedLength256 );
                SHA256Digest_t digest256;
                CCrypto::GenerateHMAC256( reinterpret_cast<const uint8 *>( pMessage ), 4 * ( pThisAttrPtr - pMessage ), pubKey, cubKey, &digest256 );
                *(uint32*)pMessage = uOriginalStartWordRaw256;
                if ( V_memcmp( attr.m_pData, &digest256, k_cubSHA256Hash ) != 0 )
                    return false;
                break;
            }

            case k_nSTUN_Attr_MessageIntegrity:
            {
                if ( attr.m_nLength != k_cubSHA1Hash )
                    return false;
                if ( cubKey == 0 )
                {
                    //SpewWarningRateLimited( SteamNetworkingSockets_GetLocalTimestamp(), "[%s] Received STUN packet with MessageIntegrity but no key to verify it\n", CUtlNetAdrRender( info.m_adrFrom ).String() );
                    return false;
                }
                const uint32 uOriginalStartWordRaw1 = *pMessage;
                const uint32 uAdjustedLength1 = 4*( pThisAttrPtr - &pMessage[5] ) + 4 + attr.m_nLength;
                *(uint32*)pMessage = htonl( ( ntohl( uOriginalStartWordRaw1 ) & 0xFFFF0000ul ) | uAdjustedLength1 );
                SHADigest_t digest1;
                CCrypto::GenerateHMAC( reinterpret_cast<const uint8 *>( pMessage ), 4 * ( pThisAttrPtr - pMessage ), pubKey, cubKey, &digest1 );
                *(uint32*)pMessage = uOriginalStartWordRaw1;
                if ( V_memcmp( attr.m_pData, &digest1, k_cubSHA1Hash ) != 0 )
                    return false;
                break;
            }

            default:
                break;
        }
    }

    return true;
}

static uint32 EncodeSTUNPacket( uint32* messageBuffer, uint16 nMessageType, int nEncoding, uint32* pTransactionID, const SteamNetworkingIPAddr& toAddr, const uint8 *pubKey, uint32 cubKey, STUNAttribute* pAttrs, int nAttrs  )
{
    {   // 20 bytes of header, 20 bytes of address, 36 bytes of SHA256, 8 bytes of fingerprint.
        int nFixedContent = 20 + 20 + 36 + 8;
        int nTotalAttrSize = 0;
        for ( int i = 0; i < nAttrs; ++i )
        {
            nTotalAttrSize += 4 + pAttrs[i].m_nLength;
        }
        if ( nFixedContent + nTotalAttrSize > k_nSTUN_MaxPacketSize_Bytes )
            return 0;
    }

    /*  All STUN messages comprise a 20-byte header followed by zero or more
        attributes.  The STUN header contains a STUN message type, message
        length, magic cookie, and transaction ID.

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |0 0|     STUN Message Type     |         Message Length        |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                         Magic Cookie                          |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |                                                               |
     |                     Transaction ID (96 bits)                  |
     |                                                               |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

                  Figure 2: Format of STUN Message Header    */

    // Cookie value and 96 bit Transaction ID here ( fills messageBuffer[2,3,4] )
    messageBuffer[1] = htonl( k_nSTUN_CookieValue );
    messageBuffer[2] = pTransactionID[0];
    messageBuffer[3] = pTransactionID[1];
    messageBuffer[4] = pTransactionID[2];

    // Write attributes first, so we can know the length...
    uint32 *pAttributePtr = &messageBuffer[5];
    if ( ( nEncoding & kSTUNPacketEncodingFlags_NoMappedAddress ) != kSTUNPacketEncodingFlags_NoMappedAddress )
    {
        if ( ( nEncoding & kSTUNPacketEncodingFlags_MappedAddress ) == kSTUNPacketEncodingFlags_MappedAddress )
        {
            pAttributePtr = WriteMappedAddress( pAttributePtr, toAddr, pTransactionID );
        }
        else
        {
            pAttributePtr = WriteXORMappedAddress( pAttributePtr, toAddr, pTransactionID );
        }
    }

    for ( int i = 0; i < nAttrs; ++i )
    {
        pAttributePtr = WriteGenericSTUNAttribute( pAttributePtr, &pAttrs[i] );
    }

    uint32 * pIntegrityPtr = nullptr;
    if ( pubKey != nullptr && cubKey > 0 )
    {
        pIntegrityPtr = pAttributePtr;
        if ( nEncoding & kSTUNPacketEncodingFlags_MessageIntegrity )
            pAttributePtr = ReserveMessageIntegrityAttribute( pIntegrityPtr );
        else
            pAttributePtr = ReserveMessageIntegritySHA256Attribute( pIntegrityPtr );
    }

    // Write the first header word of type and length.
    uint32 uAttributeLength = ( pAttributePtr - &messageBuffer[5] ) * 4;
    messageBuffer[0] = htonl( ( nMessageType & 0xFFFF ) << 16 | ( ( uAttributeLength + 3 ) & 0xFFFC ) );

    // And now the header is correct, so compute message integrity
    if ( pIntegrityPtr != nullptr )
    {
        if ( nEncoding & kSTUNPacketEncodingFlags_MessageIntegrity )
            WriteMessageIntegrityAttribute( pIntegrityPtr, messageBuffer, pubKey, cubKey );
        else
            WriteMessageIntegritySHA256Attribute( pIntegrityPtr, messageBuffer, pubKey, cubKey );
    }

    uint32 * const pFingerprintPtr = pAttributePtr;
    if ( ( nEncoding & kSTUNPacketEncodingFlags_NoFingerprint ) == 0 )
        pAttributePtr = ReserveFingerprintAttribute( pFingerprintPtr );

    // Now we know the total, final attribute size, so write the first header word of type and length.
    uAttributeLength = ( pAttributePtr - &messageBuffer[5] ) * 4;
    messageBuffer[0] = htonl( ( nMessageType & 0xFFFF ) << 16 | ( ( uAttributeLength + 3 ) & 0xFFFC ) );

    // And now the header is correct, so fingerprint..
    if ( ( nEncoding & kSTUNPacketEncodingFlags_NoFingerprint ) == 0 )
        WriteFingerprintAttribute( pFingerprintPtr, messageBuffer );

    uAttributeLength = ( pAttributePtr - &messageBuffer[5] ) * 4;
    messageBuffer[0] = htonl( ( nMessageType & 0xFFFF ) << 16 | ( ( uAttributeLength + 3 ) & 0xFFFC ) );

    return ( pAttributePtr - messageBuffer ) * 4;
}


static void ConvertNetAddr_tToSteamNetworkingIPAddr( const netadr_t& in, SteamNetworkingIPAddr *pOut )
{
    if ( pOut == nullptr )
        return;

    if ( in.GetType() == k_EIPTypeV4 )
    {
        pOut->SetIPv4( in.GetIPv4(), in.GetPort() );
    }
    else if ( in.GetType() == k_EIPTypeV6 )
    {
        pOut->SetIPv6( in.GetIPV6Bytes(), in.GetPort() );
    }
}

static void ConvertSteamNetworkingIPAddrToNetAdr_t( const SteamNetworkingIPAddr& in, netadr_t *pOut )
{
    if ( pOut == nullptr )
        return;

    if ( in.IsIPv4() )
    {
        pOut->SetIPAndPort( in.GetIPv4(), in.m_port );
    }
    else
    {
        pOut->SetIPV6AndPort( in.m_ipv6, in.m_port );
    }
}

/* Reference implementation of CRC32, adapted from
    https://datatracker.ietf.org/doc/html/rfc1952#section-8
*/

 /* Table of CRCs of all 8-bit messages. */
uint32 crc_table[256];

/* Flag: has the table been computed? Initially false. */
int crc_table_computed = 0;

/* Make the table for a fast CRC. */
void make_crc_table(void)
{
    uint32 c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (uint32) n;
        for (k = 0; k < 8; k++) {
        if (c & 1) {
            c = 0xedb88320L ^ (c >> 1);
        } else {
            c = c >> 1;
        }
        }
        crc_table[n] = c;
    }
    crc_table_computed = 1;
}

/*   Update a running crc with the bytes buf[0..len-1] and return
the updated crc. The crc should be initialized to zero. Pre- and
post-conditioning (one's complement) is performed within this
function so it shouldn't be done by the caller. Usage example:

    unsigned long crc = 0L;

    while (read_buffer(buffer, length) != EOF) {
    crc = update_crc(crc, buffer, length);
    }
    if (crc != original_crc) error();
*/
uint32 update_crc( uint32 crc, const unsigned char *buf, int len)
{
    uint32 c = crc ^ 0xffffffffL;
    int n;

    if (!crc_table_computed)
        make_crc_table();
    for (n = 0; n < len; n++) {
        c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xffffffffL;
}

/* Return the CRC of the bytes buf[0..len-1]. */
static uint32 CRC32( const unsigned char *buf, int len )
{
    return update_crc( 0L, buf, len );
}

// Parse a candidate-attribute from https://datatracker.ietf.org/doc/html/rfc5245#section-15.1
// Ex: candidate:2442523459 0 udp 2122262784 2602:801:f001:1034:5078:221c:76b:a3d6 63368 typ host generation 0 ufrag WLM82 network-id 2
bool ParseRFC5245CandidateAttribute( const char *pszAttr, RFC5245CandidateAttr *pAttr )
{
    if ( pszAttr == nullptr || pAttr == nullptr )
        return false;

    {   // Check to make sure pszAttr is indeed null-terminated (max length of 8k)
        bool bOk = false;
        for ( int i = 0; i < ( 1024 * 8 ) ; ++i )
        {
            if ( pszAttr[ i ] == '\0' )
            {
                bOk = true;
                break;
            }
        }
        if ( !bOk )
            return false;
    }
    const char *pCh = pszAttr;

    // candidate:
    static const char szPrefixString[] = "candidate:";
    if ( V_strstr( pCh, szPrefixString ) != pCh )
        return false;
    pCh += ( V_ARRAYSIZE( szPrefixString ) - 1 );

    // foundation= 1*32ice-char
    const char *pFoundationBegin = pCh;
    while( *pCh != '\0' && *pCh != ' ' ) pCh++;
    const char *pFoundationEnd = pCh;

    // <SP>
    while ( *pCh == ' ' ) pCh++;

    // component= 1*5DIGIT
    const char *pComponentIDBegin = pCh;
    while ( *pCh != '\0' && *pCh != ' ' ) pCh++;
    const char *pComponentIDEnd = pCh;

    // <SP>
    while ( *pCh == ' ' ) pCh++;

    // transport= "UDP" / transport-extension
    const char *pTransportBegin = pCh;
    while ( *pCh != '\0' && *pCh != ' ' ) pCh++;
    const char *pTransportEnd = pCh;

    // <SP>
    while ( *pCh == ' ' ) pCh++;

    // priority= 1*10DIGIT
    const char *pPriorityBegin = pCh;
    while ( *pCh != '\0' && *pCh != ' ' ) pCh++;
    const char *pPriorityEnd = pCh;

    // <SP>
    while ( *pCh == ' ' ) pCh++;

    // connection-address= RFC4566  https://datatracker.ietf.org/doc/html/rfc4566
    const char *pConnectionAddressBegin = pCh;
    while ( *pCh != '\0' && *pCh != ' ') pCh++;
    const char *pConnectionAddressEnd = pCh;

    // <SP>
    while ( *pCh == ' ' ) pCh++;

    // port= RFC4566
    const char *pPortBegin = pCh;
    while ( *pCh != '\0' && *pCh != ' ') pCh++;
    const char *pPortEnd = pCh;

    // <SP>
    while ( *pCh == ' ' ) pCh++;

    // typ
    static const char szTypString[] = "typ";
    if ( V_strstr( pCh, szTypString ) != pCh )
        return false;
    pCh += ( V_ARRAYSIZE( szTypString ) - 1 );

    // <SP>
    while ( *pCh == ' ' ) pCh++;

    // "host" / "srflx" / "prflx" / "relay" / token
    const char *pCandidateTypeBegin = pCh;
    while ( *pCh != '\0' && *pCh != ' ') pCh++;
    const char *pCandidateTypeEnd = pCh;

    // Consume rel-addr and rel-port along with optional attributes
    CUtlVector< const char *> vAttrNameBegin;
    CUtlVector< const char *> vAttrNameEnd;
    CUtlVector< const char *> vAttrValueBegin;
    CUtlVector< const char *> vAttrValueEnd;
    while ( *pCh != '\0' )
    {
        // *(SP extension-att-name SP extension-att-value)
        // <SP>
        while ( *pCh == ' ' ) pCh++;

        vAttrNameBegin.AddToTail( pCh );
        while ( *pCh != '\0' && *pCh != ' ') pCh++;
        vAttrNameEnd.AddToTail( pCh );

        // <SP>
        while ( *pCh == ' ' ) pCh++;

        vAttrValueBegin.AddToTail( pCh );
        while ( *pCh != '\0' && *pCh != ' ') pCh++;
        vAttrValueEnd.AddToTail( pCh );
    }

    if ( pFoundationBegin == pFoundationEnd || pComponentIDBegin == pComponentIDEnd
        || pTransportBegin == pTransportEnd || pPriorityBegin == pPriorityEnd
        || pConnectionAddressBegin == pConnectionAddressEnd || pPortBegin == pPortEnd
        || pCandidateTypeBegin == pCandidateTypeEnd )
        return false;

    if ( vAttrNameBegin.Count() != vAttrNameEnd.Count() || vAttrNameBegin.Count() != vAttrValueBegin.Count() || vAttrNameBegin.Count() != vAttrValueEnd.Count() )
        return false;

    for ( int i = 0; i < vAttrNameBegin.Count(); ++i )
    {
        if ( vAttrNameBegin[i] == vAttrNameEnd[i] )
            return false;
        if ( vAttrValueBegin[i] == vAttrValueEnd[i] )
            return false;
    }
    {
        std::string foundation( pFoundationBegin, pFoundationEnd - pFoundationBegin );
        pAttr->sFoundation.swap( foundation );
    }
    pAttr->nComponent = atoi( pComponentIDBegin );

    {
        std::string transport( pTransportBegin, pTransportEnd - pTransportBegin );
        pAttr->sTransport.swap( transport );
    }
    pAttr->nPriority = atoi( pPriorityBegin );
    {
        std::string connectionAddr( pConnectionAddressBegin, pConnectionAddressEnd - pConnectionAddressBegin );
        if ( !pAttr->address.ParseString( connectionAddr.c_str() ) )
            return false;
    }
    pAttr->address.m_port = (uint16)atoi( pPortBegin );
    {
        std::string candidateType( pCandidateTypeBegin, pCandidateTypeEnd - pCandidateTypeBegin );
        pAttr->sType.swap( candidateType );
    }
    if ( pAttr->sType == "host" )
        pAttr->nType = ICECandidateKind::Host;
    else if ( pAttr->sType == "srflx" )
        pAttr->nType = ICECandidateKind::ServerReflexive;
    else if ( pAttr->sType == "prflx" )
        pAttr->nType = ICECandidateKind::PeerReflexive;
    else if ( pAttr->sType == "relay" )
        pAttr->nType = ICECandidateKind::Relayed;
    else
        pAttr->nType = ICECandidateKind::None;
    for ( int i = 0; i < vAttrNameBegin.Count(); ++i )
    {
        pAttr->vAttrs.AddToTail( std::pair<std::string,std::string>( std::string( vAttrNameBegin[i], vAttrNameEnd[i]-vAttrNameBegin[i] ), std::string( vAttrValueBegin[i], vAttrValueEnd[i]-vAttrValueBegin[i] ) ) );
    }
    return true;
}

} // namespace <anonymous>

// Compare IP addresses, ignoring the port.
// Should we promotet this to a more public header?  Or perhaps
// make it a member of SteamNetworkingIPAddr?
inline bool IPAddrEqualIgnoringPort( const SteamNetworkingIPAddr &a, const SteamNetworkingIPAddr &b )
{
    return memcmp( a.m_ipv6, b.m_ipv6, sizeof(a.m_ipv6) ) == 0;
}


/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingSocketsSTUNRequest
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingSocketsSTUNRequest::CSteamNetworkingSocketsSTUNRequest( ICESessionInterface *pInterface )
    : m_pInterface( pInterface )
    , m_addrRelay{}
{
    CCrypto::GenerateRandomBlock( m_nTransactionID, 12 );
}

CSteamNetworkingSocketsSTUNRequest::~CSteamNetworkingSocketsSTUNRequest()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
}

void CSteamNetworkingSocketsSTUNRequest::Queue( uint32 nMessageType, int nEncoding, SteamNetworkingIPAddr remoteAddr, RecvSTUNPacketCallback_t cb, STUNAttribute *pExtraAttrs, int nExtraAttrs )
{
    m_remoteAddr = remoteAddr;
    m_nRetryCount = 0;
    m_nMaxRetries = 7;
    m_callback = cb;
	m_usecLastSentTime = 0;
    m_cbPacketSize = EncodeSTUNPacket( m_packet, nMessageType, nEncoding, m_nTransactionID,
        m_pInterface->m_pSocket->m_boundAddr,
        (const uint8*)m_strPassword.c_str(), (uint32)m_strPassword.size(),
        pExtraAttrs, nExtraAttrs );

    // Schedule send.  We do it this way instead of sending imediately, in case we fail and need to Cancel.
    // That way, Cancel always only happens from one call stack, inside Think(). and we don't get tangled up.
    SetNextThinkTimeASAP();
}


void CSteamNetworkingSocketsSTUNRequest::Cancel()
{
    if ( m_callback )
    {
        RecvSTUNPktInfo_t subInfo;
        subInfo.m_pRequest = this;
        subInfo.m_pHeader = nullptr;
        subInfo.m_nAttributes = 0;
        subInfo.m_pAttributes = nullptr;
        subInfo.m_usecNow = SteamNetworkingSockets_GetLocalTimestamp();
        ( m_pInterface->m_session.*m_callback )( subInfo );
    }

    delete this;
}

void CSteamNetworkingSocketsSTUNRequest::Think( SteamNetworkingMicroseconds usecNow )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingSocketsSTUNRequest::Think" );

    if ( m_nRetryCount < m_nMaxRetries )
    {

        ++m_nRetryCount;
        SteamNetworkingMicroseconds retryTimeout = 500000 * ( 1 << m_nRetryCount ); // 2 ^ retryCount * 500ms
        if ( retryTimeout > 60000000 ) // Max timeout of 60s.
            retryTimeout = 60000000;

        iovec temp;
        temp.iov_base = m_packet;
        temp.iov_len = m_cbPacketSize;
        if ( m_pInterface->SendPacketGather( 1, &temp, m_cbPacketSize, m_remoteAddr, m_addrRelay ) )
        {
            m_usecLastSentTime = usecNow;
            SetNextThinkTime( usecNow + retryTimeout );
            return;
        }

        // Immediate failure to send is actually very common, e.g. unroutable between two different private subnets.
        m_usecLastSentTime = 0;
    }

    // Call the callback to notify that we've failed, and SELF DESTRUCT
    Cancel();

    // WARNING: We don't exist here!
}

void CSteamNetworkingSocketsSTUNRequest::ReplyPacketReceived( const RecvPktInfo_t &info, const STUNHeader &header )
{
    // Parse attributes.  Drop packet if there is a problem.
    CUtlVector< STUNAttribute > vecAttributes;
    if ( !ParseSTUNAttributes( info, (const byte*)m_strPassword.c_str(), (uint32)m_strPassword.size(), &vecAttributes ) )
        return;

    if ( m_callback )
    {
        RecvSTUNPktInfo_t subInfo;
        subInfo.m_pRequest = this;
        subInfo.m_usecNow = info.m_usecNow;
        subInfo.m_pHeader = &header;
        subInfo.m_nAttributes = vecAttributes.Count();
        subInfo.m_pAttributes = vecAttributes.Base();
        ( m_pInterface->m_session.*m_callback )( subInfo );
    }

    delete this;
}


/////////////////////////////////////////////////////////////////////////////
//
// ICESessionInterface
//
/////////////////////////////////////////////////////////////////////////////

void ICESessionInterface::QueueBindRequest( const SteamNetworkingIPAddr &addrSTUNServer, RecvSTUNPacketCallback_t cb, int nEncoding )
{
    Assert( !m_pPendingSTUNRequest );

    m_pPendingSTUNRequest = new CSteamNetworkingSocketsSTUNRequest( this );
    m_pPendingSTUNRequest->Queue( k_nSTUN_BindingRequest, nEncoding, addrSTUNServer, cb );
}

void ICESessionInterface::QueueAllocateRequest( const SteamNetworkingIPAddr &addrTURNServer, RecvSTUNPacketCallback_t cb, int nEncoding )
{
    Assert( !m_pPendingSTUNRequest );

    // REQUESTED-TRANSPORT: UDP (IANA protocol 17), protocol byte + 3 RFFU bytes
    uint32 uTransport = htonl( 17u << 24 );
    STUNAttribute reqTransport;
    reqTransport.m_nType   = k_nTURN_Attr_RequestedTransport;
    reqTransport.m_nLength = 4;
    reqTransport.m_pData   = &uTransport;

    m_pPendingSTUNRequest = new CSteamNetworkingSocketsSTUNRequest( this );
    m_pPendingSTUNRequest->Queue( k_nTURN_AllocateRequest, nEncoding | kSTUNPacketEncodingFlags_NoMappedAddress, addrTURNServer, cb, &reqTransport, 1 );
}

// Send a gathered packet to the pair, routing via TURN Send Indication if the
// local candidate is a relay.  For non-relay pairs this is a thin wrapper around
// BSendRawPacketGather.
bool ICESessionInterface::SendPacketGather( int nChunks, const iovec *pChunks, int cbPayload, const SteamNetworkingIPAddr &addrPeer, const SteamNetworkingIPAddr &addrRelay )
{
    if ( addrRelay.IsIPv6AllZeros() )
        return m_pSocket->BSendRawPacketGather( nChunks, pChunks, addrPeer );

    // Relay: wrap in a TURN Send Indication.
    if ( nChunks > 3 )
    {
        AssertMsg( false, "Too many chunks to send via TURN relay (max 3)" );
        return false;
    }

    // Build Send Indication: STUN header + XOR-PEER-ADDRESS + DATA.
    const int cbPad      = ( 4 - ( cbPayload & 3 ) ) & 3;
    const int cbPeerAttr = addrPeer.IsIPv4() ? 12 : 24;
    const int cbAttrs    = cbPeerAttr + 4 + cbPayload + cbPad;
    uint8 hdrBuf[ 20 + 24 + 8 ];  // header + max peer attr + DATA attr header
    uint32 *p = (uint32 *)hdrBuf;

    // STUN header
    p[0] = htonl( ( k_nTURN_SendIndication << 16 ) | (uint16)cbAttrs );
    p[1] = htonl( k_nSTUN_CookieValue );
    p[2] = p[3] = p[4] = 0;  // transaction ID = zeros; indications don't need one
    p += 5;

    // XOR-PEER-ADDRESS
    const uint32 nXORPort = (uint32)addrPeer.m_port ^ ( k_nSTUN_CookieValue >> 16 );
    if ( addrPeer.IsIPv4() )
    {
        p[0] = htonl( ( k_nTURN_Attr_XORPeerAddress << 16 ) | 8 );
        p[1] = htonl( ( 0x01u << 16 ) | nXORPort );
        p[2] = htonl( addrPeer.GetIPv4() ^ k_nSTUN_CookieValue );
        p += 3;
    }
    else
    {
        p[0] = htonl( ( k_nTURN_Attr_XORPeerAddress << 16 ) | 20 );
        p[1] = htonl( ( 0x02u << 16 ) | nXORPort );
        V_memcpy( &p[2], addrPeer.m_ipv6, 16 );
        p[2] ^= htonl( k_nSTUN_CookieValue );
        // Transaction ID is zeros so no additional XOR needed for IPv6
        p += 6;
    }

    // DATA attribute type+length  (payload and padding follow in separate iovecs)
    p[0] = htonl( ( k_nTURN_Attr_Data << 16 ) | (uint16)cbPayload );

    static const uint32 k_zeroPad = 0;
    iovec relayChunks[5];
    relayChunks[0].iov_base = hdrBuf;
    relayChunks[0].iov_len = (size_t)( (uint8*)(p+1) - hdrBuf );
    int nRelayChunks = 1;
    for ( int i = 0; i < nChunks; ++i )
        relayChunks[nRelayChunks++] = pChunks[i];
    if ( cbPad )
    {
        relayChunks[nRelayChunks].iov_base = (void*)&k_zeroPad;
        relayChunks[nRelayChunks].iov_len = cbPad;
        ++nRelayChunks;
    }

    return m_pSocket->BSendRawPacketGather( nRelayChunks, relayChunks, addrRelay );
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingICESession
//
/////////////////////////////////////////////////////////////////////////////
CSteamNetworkingICESession::CSteamNetworkingICESession( EICERole role, CSteamNetworkingICESessionCallbacks *pCallbacks, int nEncoding )
{
    m_nEncoding = nEncoding;
    m_pCallbacks = pCallbacks;
    m_bInterfaceListStale = true;
    m_nextKeepalive = 0;
    m_role = role;
    m_pSelectedCandidatePair = nullptr;
    m_vecInterfaces.reserve( 16 );
	m_nPermittedCandidateTypes = k_EICECandidate_Any;
}

CSteamNetworkingICESession::CSteamNetworkingICESession( const ICESessionConfig& cfg, CSteamNetworkingICESessionCallbacks *pCallbacks )
{
	m_nEncoding = kSTUNPacketEncodingFlags_MessageIntegrity;
	m_pCallbacks = pCallbacks;
	m_bInterfaceListStale = true;
    m_nextKeepalive = 0;
    m_role = cfg.m_eRole;
    m_pSelectedCandidatePair = nullptr;
    m_vecInterfaces.reserve( 16 );

	m_vecSTUNServers.reserve( cfg.m_nStunServers );

	{
		for ( int i = 0; i < cfg.m_nStunServers; ++i )
		{
			const char *pszHostname = cfg.m_pStunServers[i];
			if ( V_strnicmp( pszHostname, "stun:", 5 ) == 0 )
				pszHostname = pszHostname + 5;
			CUtlVector< SteamNetworkingIPAddr > stunServers;
			ResolveHostname( pszHostname, &stunServers );
			m_vecSTUNServers.reserve( m_vecSTUNServers.size() + stunServers.Count() );
			for ( const SteamNetworkingIPAddr &ip: stunServers )
				m_vecSTUNServers.push_back( ip );
		}
	}

	m_vecTURNServers.reserve( cfg.m_nTurnServers );
	for ( int i = 0; i < cfg.m_nTurnServers; ++i )
	{
		const char *pszHostname = cfg.m_pTurnServers[i].m_pszHost;
		if ( pszHostname == nullptr )
			continue;
		if ( V_strnicmp( pszHostname, "turn:", 5 ) == 0 )
			pszHostname = pszHostname + 5;
		CUtlVector< SteamNetworkingIPAddr > turnServers;
		ResolveHostname( pszHostname, &turnServers );
		m_vecTURNServers.reserve( m_vecTURNServers.size() + turnServers.Count() );
		for ( const SteamNetworkingIPAddr &ip: turnServers )
			m_vecTURNServers.push_back( ip );
	}

	m_nPermittedCandidateTypes = cfg.m_nCandidateTypes;
	m_strLocalUsernameFragment = cfg.m_pszLocalUserFrag;
	m_strLocalPassword = cfg.m_pszLocalPwd;
}


CSteamNetworkingICESession::~CSteamNetworkingICESession()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

    // Make sure we don't fire any more callbacks while doing any of this processing
    m_pCallbacks = nullptr;

    for ( const auto &pIntf : m_vecInterfaces )
    {
        if ( pIntf->m_pPendingSTUNRequest )
        {
            delete pIntf->m_pPendingSTUNRequest;
            pIntf->m_pPendingSTUNRequest = nullptr;
        }
    }

    for ( ICECandidatePair *pPair: m_vecCandidatePairs )
        InternalDeleteCandidatePair( pPair );
    m_vecCandidatePairs.clear();

    m_vecInterfaces.clear();
}

bool CSteamNetworkingICESession::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
    if ( !m_pSelectedCandidatePair )
        return false;
    return m_pSelectedCandidatePair->m_localCandidate.m_pInterface->SendPacketGather( nChunks, pChunks, cbSendTotal,
        m_pSelectedCandidatePair->m_remoteCandidate.m_addr, m_pSelectedCandidatePair->m_localCandidate.m_addrTURNServer );
}

void CSteamNetworkingICESession::SetRemoteUsername( const char *pszUsername )
{
    m_strRemoteUsernameFragment = pszUsername;
    m_strOutgoingUsername = m_strRemoteUsernameFragment + ':' + m_strLocalUsernameFragment;
    m_strIncomingUsername = m_strLocalUsernameFragment + ':' + m_strRemoteUsernameFragment;
}

void CSteamNetworkingICESession::SetRemotePassword( const char *pszPassword )
{
    m_strRemotePassword = pszPassword;

    // We might have been waiting for this before sending binding requests
    // to the peer (which cannot be authenticated until we have the remote password)
    SetNextThinkTimeASAP();
}

// Returns true if addr is a public IP that we should ask our TURN relay to permit.
// Excludes loopback, RFC-1918 private, and link-local addresses — forwarding from
// those would be meaningless (or expose internal topology).
static bool IsTURNPermissibleIP( const SteamNetworkingIPAddr &addr )
{
    if ( addr.IsIPv4() )
    {
        const uint8 *ip = addr.m_ipv4.m_ip;
        // Mock network: 127.0.100.x is the simulated public internet (third octet == 100).
        // All other 127.x.x.x addresses are private/LAN.
        if ( TEST_mocknetwork_active )
            return ip[0] == 127 && ip[1] == 0 && ip[2] == 100;
        if ( ip[0] == 127 ) return false;                                   // loopback
        if ( ip[0] == 10  ) return false;                                   // RFC 1918 10/8
        if ( ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31 ) return false;    // RFC 1918 172.16/12
        if ( ip[0] == 192 && ip[1] == 168 ) return false;                   // RFC 1918 192.168/16
        if ( ip[0] == 169 && ip[1] == 254 ) return false;                   // link-local
        return true;
    }
    else
    {
        const uint8 *ip = addr.m_ipv6;
        // Mock network: fd7f:0:100::x is the simulated public internet.
        if ( TEST_mocknetwork_active )
            return ip[0] == 0xfd && ip[1] == 0x7f && ip[2] == 0x00 && ip[3] == 0x00
                && ip[4] == 0x01 && ip[5] == 0x00;
        static const uint8 k_ipv6Loopback[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
        if ( memcmp( ip, k_ipv6Loopback, 16 ) == 0 ) return false;         // ::1
        if ( ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80 ) return false;       // fe80::/10 link-local
        if ( ip[0] == 0xfc || ip[0] == 0xfd ) return false;                 // fc00::/7 ULA
        return true;
    }
}

EICECandidateType CSteamNetworkingICESession::AddPeerCandidate( const RFC5245CandidateAttr& attr )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

    ICECandidateBase candidate( attr.nType, attr.address );
    candidate.m_nPriority = attr.nPriority;
    const char *pszFoundation = attr.sFoundation.c_str();

    EICECandidateType eCandidateType = CalcICECandidateType( attr.nType, attr.address );

    // Do we already have a candidate for this peer? If so, just update the foundation and move on.
	bool bNeedsNewEntry = true;
    for ( ICEPeerCandidate& c : m_vecPeerCandidates )
    {
        if ( c.m_addr == candidate.m_addr )
        {
			// If the foundation is the same, don't do anything - this is redundant.
			if ( V_strcmp( c.m_sFoundation.c_str(), pszFoundation ) == 0 )
				return eCandidateType;

            (ICECandidateBase&)c = candidate;
            c.m_sFoundation = pszFoundation;
			bNeedsNewEntry = false;

			// Propagate the updated candidate type to any existing pairs that have a stale copy.
			// Pairs are created from incoming binding requests before signaling arrives, giving
			// them peer-reflexive type.  When the signaling later confirms the address is a host
			// candidate, update the pair's copy so OnConnectionSelected sees the correct type.
			for ( ICECandidatePair *pPair : m_vecCandidatePairs )
			{
				if ( pPair->m_remoteCandidate.m_addr == candidate.m_addr )
					(ICECandidateBase&)pPair->m_remoteCandidate = candidate;
			}
            break; // fall through to update state and trigger a think
        }
    }
	if ( bNeedsNewEntry )
	{
		m_vecPeerCandidates.push_back( ICEPeerCandidate( candidate, pszFoundation ) );

		// If this is a public IP, register it for TURN CreatePermission so our relay
		// will forward traffic from it.  Dedup by IP (port is ignored for permissions).
		SteamNetworkingIPAddr permAddr = candidate.m_addr;
		permAddr.m_port = 0;
		if ( IsTURNPermissibleIP( permAddr ) )
		{
			std_vector<SteamNetworkingIPAddr> &vecPermitted = permAddr.IsIPv4() ? m_vecTURNPermittedIPv4 : m_vecTURNPermittedIPv6;
			int &nRevision = permAddr.IsIPv4() ? m_nTURNPermissionRevisionIPv4 : m_nTURNPermissionRevisionIPv6;
			bool bAlreadyPermitted = false;
			for ( const SteamNetworkingIPAddr &existing : vecPermitted )
			{
				if ( IPAddrEqualIgnoringPort( existing, permAddr ) )
				{
					bAlreadyPermitted = true;
					break;
				}
			}
			if ( !bAlreadyPermitted )
			{
				vecPermitted.push_back( permAddr );
				++nRevision;
			}
		}
	}
    m_bCandidatePairsNeedUpdate = true;
    SetNextThinkTimeASAP();
    return eCandidateType;
}

void CSteamNetworkingICESession::InvalidateInterfaceList()
{
    m_bInterfaceListStale = true;
}

void CSteamNetworkingICESession::SetSelectedCandidatePair( ICECandidatePair *pPair )
{
    SpewMsg( "\n\nSelected candidate %s -> %s.\n\n", SteamNetworkingIPAddrRender( pPair->m_localCandidate.m_pInterface->m_pSocket->m_boundAddr ).c_str(), SteamNetworkingIPAddrRender( pPair->m_remoteCandidate.m_addr ).c_str() );
    m_pSelectedCandidatePair = pPair;
    if ( m_pCallbacks )
        m_pCallbacks->OnConnectionSelected( pPair->m_localCandidate, pPair->m_remoteCandidate );
}

void CSteamNetworkingICESession::InternalDeleteCandidatePair( ICECandidatePair *pPair )
{
    if ( pPair == m_pSelectedCandidatePair )
    {
        m_pSelectedCandidatePair = nullptr;
    }

    if ( pPair->m_pPeerRequest != nullptr )
    {
        find_and_remove_element( m_vecPendingPeerRequests, pPair->m_pPeerRequest );
        delete pPair->m_pPeerRequest;
        pPair->m_pPeerRequest = nullptr;
    }

    find_and_remove_element( m_vecTriggeredCheckQueue, pPair );

    delete pPair;
}

int CSteamNetworkingICESession::GetPing() const
{
	if ( m_pSelectedCandidatePair == nullptr )
		return -1;
	return m_pSelectedCandidatePair->m_nLastRecordedPing;
}

void CSteamNetworkingICESession::StartSession()
{
    m_nextKeepalive = 0;
    m_pSelectedCandidatePair = nullptr;
    CCrypto::GenerateRandomBlock( &m_nRoleTiebreaker, sizeof( m_nRoleTiebreaker ) );
    SetNextThinkTimeASAP();
}

void CSteamNetworkingICESession::GatherInterfaces()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::GatherInterfaces" );

    CUtlVector<LocalAddress_t> vecAddrs;
    if ( !GetLocalAddresses( &vecAddrs ) )
        return;

    m_bInterfaceListStale = false;

    // First pass: scan existing interfaces against the new address list.
    // Remove any that are no longer present; consume matched addresses from
    // vecAddrs so only genuinely new ones remain after this pass.
    // Also track the lowest priority among survivors so new interfaces are
    // assigned priorities below all existing ones.
    uint32 uNextPriority = 65535;
    for ( int i = len( m_vecInterfaces ) - 1; i >= 0; --i )
    {
        ICESessionInterface *intf = m_vecInterfaces[i].get();

        bool bFound = false;
        for ( int j = 0; j < vecAddrs.Count(); ++j )
        {
            if ( IPAddrEqualIgnoringPort( vecAddrs[j].m_addr, intf->m_pSocket->m_boundAddr ) )
            {
                vecAddrs.Remove( j );
                bFound = true;
                break;
            }
        }

        if ( !bFound )
        {
            // ICESessionInterface disappeared!  Delete the socket and all candidates
            // and pairs that use it
            SpewMsg( "Local interface %s removed\n", SteamNetworkingIPAddrRender( intf->m_pSocket->m_boundAddr ).c_str() );

            for ( int j = len( m_vecCandidatePairs ) - 1; j >= 0; --j )
            {
                ICECandidatePair *pPair = m_vecCandidatePairs[j];
                if ( pPair->m_localCandidate.m_pInterface == intf )
                {
                    InternalDeleteCandidatePair( pPair );
                    erase_at( m_vecCandidatePairs, j );
                }
            }

            if ( intf->m_pPendingSTUNRequest )
            {
                delete intf->m_pPendingSTUNRequest;
                intf->m_pPendingSTUNRequest = nullptr;
            }

            erase_at( m_vecInterfaces, i );
            continue;
        }

        if ( intf->m_nPriority <= uNextPriority )
            uNextPriority = intf->m_nPriority-1;
    }

    // Second pass: add genuinely new interfaces.  Assign priorities counting
    // down from just below the lowest surviving priority (or from 65535 if
    // the list was empty).
    for ( const LocalAddress_t &addr: vecAddrs )
    {
        std::unique_ptr<ICESessionInterface> pIntf = std::make_unique<ICESessionInterface>( *this, uNextPriority, addr.m_nPrefixLen );
        SteamDatagramErrMsg errMsg;
        SteamNetworkingIPAddr bindAddr = addr.m_addr;
        pIntf->m_pSocket = OpenRawUDPSocket( CRecvPacketCallback( CSteamNetworkingICESession::StaticPacketReceived, pIntf.get() ), errMsg, &bindAddr, nullptr );
        if ( pIntf->m_pSocket == nullptr )
        {
            SpewWarning( "Could not bind to %s, skipping interface.  %s\n", SteamNetworkingIPAddrRender( addr.m_addr ).c_str(), errMsg );
            continue;
        }

        m_vecInterfaces.emplace_back( std::move( pIntf ) );
        if ( uNextPriority > 0 )
            --uNextPriority;

        ICESessionInterface *pNewIntf = m_vecInterfaces.back().get();
        pNewIntf->NotifyLocalCandidateDiscovered( ICECandidateKind::Host, pNewIntf->m_pSocket->m_boundAddr );
        m_bCandidatePairsNeedUpdate = true;
    }
}

void CSteamNetworkingICESession::OnPacketReceived( const RecvPktInfo_t &info, ICESessionInterface *pInterface, SteamNetworkingIPAddr *pAddrRelay )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::OnPacketReceived" );

    //
    // Quick check if packet might be a STUN packet, and unpacket the header
    //
    //  0                   1                   2                   3
    //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |0 0|     STUN Message Type     |         Message Length        |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                         Magic Cookie                          |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                     Transaction ID (96 bits)                  |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    if ( info.m_cbPkt < 20 )
    {
not_stun:
        if ( m_pCallbacks != nullptr )
            m_pCallbacks->OnPacketReceived( info );
        return;
    }

    STUNHeader header;
    {

        const uint32 * const pWords = reinterpret_cast<const uint32 *>( info.m_pPkt );
        const uint32 nFirstWord = ntohl( pWords[0] );
        if (
            ( nFirstWord & 0xc000FFFF ) + 20 != (uint32)info.m_cbPkt      // top 2 bits zero and length field matches
            || pWords[1] != htonl( k_nSTUN_CookieValue )     // magic cookie
        ) {
            goto not_stun;
        }

        header.m_nMessageType   = ( nFirstWord >> 16 ) & 0x3FFF;
        header.m_nMessageLength = ( nFirstWord & 0xFFFF );
        header.m_nTransactionID[0] = pWords[2]; // treat as opaque bits, no byte-swap
        header.m_nTransactionID[1] = pWords[3];
        header.m_nTransactionID[2] = pWords[4];
    }

    // TURN Data Indications are the most common STUN-framed packet once a relay is active.
    // Handle them first.  They are server-initiated (no matching transaction ID) so the
    // normal response-routing path below would just drop them.
    if ( !pAddrRelay && header.m_nMessageType == k_nTURN_DataIndication )
    {
        // Must originate from the TURN server we allocated with; discard anything else.
        SteamNetworkingIPAddr fromAddr;
        ConvertNetAddr_tToSteamNetworkingIPAddr( info.m_adrFrom, &fromAddr );
        if ( !( fromAddr == pInterface->m_addrTURNServer ) )
            return;

        // TODO: avoid heap allocation per packet
        CUtlVector<STUNAttribute> vecAttrs;
        ParseSTUNAttributes( info, nullptr, 0, &vecAttrs );

        const STUNAttribute *pPeerAttr = FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nTURN_Attr_XORPeerAddress );
        const STUNAttribute *pDataAttr = FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nTURN_Attr_Data );
        if ( pPeerAttr == nullptr || pDataAttr == nullptr )
            return;

        SteamNetworkingIPAddr peerAddr;
        peerAddr.Clear();
        if ( !ReadXORAddressAttribute( pPeerAttr, &header, &peerAddr ) )
            return;

        // Re-enter with the inner payload, as if it arrived directly from the peer.
        RecvPktInfo_t innerInfo;
        innerInfo.m_pPkt    = reinterpret_cast<const uint8 *>( pDataAttr->m_pData );
        innerInfo.m_cbPkt   = (int)pDataAttr->m_nLength;
        innerInfo.m_usecNow = info.m_usecNow;
        innerInfo.m_pSock   = info.m_pSock;
        ConvertSteamNetworkingIPAddrToNetAdr_t( peerAddr, &innerInfo.m_adrFrom );
        OnPacketReceived( innerInfo, pInterface, &pInterface->m_addrTURNServer );
        return;
    }

    // STUN responses: route to the matching in-flight request by transaction ID.
    if ( header.m_nMessageType != k_nSTUN_BindingRequest )
    {
        // Fast path: check the interface's own server-reflexive request first (O(1)).
        CSteamNetworkingSocketsSTUNRequest *pRequest = pInterface->m_pPendingSTUNRequest;
        if ( pRequest == nullptr
            || pRequest->m_nTransactionID[0] != header.m_nTransactionID[0]
            || pRequest->m_nTransactionID[1] != header.m_nTransactionID[1]
            || pRequest->m_nTransactionID[2] != header.m_nTransactionID[2] )
        {
            pRequest = nullptr;
            for ( CSteamNetworkingSocketsSTUNRequest *p : m_vecPendingPeerRequests )
            {
                if ( p->m_pInterface == pInterface
                    && p->m_nTransactionID[0] == header.m_nTransactionID[0]
                    && p->m_nTransactionID[1] == header.m_nTransactionID[1]
                    && p->m_nTransactionID[2] == header.m_nTransactionID[2] )
                {
                    pRequest = p;
                    break;
                }
            }
        }
        if ( pRequest != nullptr )
            pRequest->ReplyPacketReceived( info, header );

        // Unknown or stale response — drop silently.
        return;
    }

    //
    // Incoming binding request
    //

    // Parse attributes and verify message integrity.
    CUtlVector< STUNAttribute > vecAttrs;
    if ( !ParseSTUNAttributes( info, (const uint8*)m_strLocalPassword.c_str(), (uint32)m_strLocalPassword.size(), &vecAttrs ) )
        return;

    const STUNAttribute *pUsernameAttr = FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nSTUN_Attr_UserName );
    if ( pUsernameAttr != nullptr )
    {
        if ( pUsernameAttr->m_nLength < (uint32)m_strIncomingUsername.size() )
        {
            SpewMsg( "Incorrect username length; at least %d expected, got %d.", (int)m_strIncomingUsername.size(), pUsernameAttr->m_nLength );
            return;
        }
        if ( m_strIncomingUsername.size() == 0 )
        {
            const char* pCh = (const char*)( pUsernameAttr->m_pData );
            size_t nLen = 0;
            for( size_t i = 0; i < pUsernameAttr->m_nLength; ++i )
            {
                if ( pCh[i] == ':' )
                {
                    nLen = i;
                    break;
                }
            }
            if ( nLen == 0 )
            {
                SpewMsg( "Invalid username; no : found in %s", std::string( (const char*)( pUsernameAttr->m_pData ),pUsernameAttr->m_nLength ).c_str() );
                return;
            }

            std::string discoveredRemoteName( (char*)pUsernameAttr->m_pData + nLen + 1, pUsernameAttr->m_nLength - nLen - 1 );
            SetRemoteUsername( discoveredRemoteName.c_str() );
        }
        else if ( V_memcmp( pUsernameAttr->m_pData, m_strIncomingUsername.c_str(), m_strIncomingUsername.size() ) != 0 )
        {
            std::string remoteName( (char*)pUsernameAttr->m_pData, pUsernameAttr->m_nLength );
            SpewMsg( "Incorrect username: got '%s' expected '%s'.", remoteName.c_str(), m_strIncomingUsername.c_str() );
            return;
        }
    }

    SteamNetworkingIPAddr fromAddr;
    ConvertNetAddr_tToSteamNetworkingIPAddr( info.m_adrFrom, &fromAddr );
    CUtlVector< STUNAttribute > outAttrs;

    //
    // Find the candidate pair for this binding request, if any
    //

    SpewMsg( "Incoming binding request from %s to %s.\n\n", SteamNetworkingIPAddrRender( fromAddr ).c_str(), SteamNetworkingIPAddrRender( pInterface->m_pSocket->m_boundAddr ).c_str() );

    ICELocalCandidate localCandidate{ pInterface, pAddrRelay ? *pAddrRelay : SteamNetworkingIPAddr{} };

    ICECandidatePair *pThisPair = nullptr;
    for ( ICECandidatePair *pPair : m_vecCandidatePairs )
    {
        if ( pPair->m_remoteCandidate.m_addr == fromAddr
            && pPair->m_localCandidate.m_pInterface == localCandidate.m_pInterface
            && pPair->m_localCandidate.m_addrTURNServer == localCandidate.m_addrTURNServer
        ) {
            pThisPair = pPair;
            break;
        }
    }

    // Stale request on a pair we're not using? Ignore.
    if ( m_pSelectedCandidatePair != nullptr && m_pSelectedCandidatePair != pThisPair )
    {
        return;
    }

    //
    // Didn't find a pair?  Then maybe we can create one
    //
    if ( pThisPair == nullptr )
    {

        ICEPeerCandidate *pRemoteCandidate = nullptr;
        for ( ICEPeerCandidate &c : m_vecPeerCandidates )
        {
            if ( c.m_addr == fromAddr )
            {
                pRemoteCandidate = &c;
                break;
            }
        }
        if ( pRemoteCandidate == nullptr )
        {
            ICECandidateBase newRemoteCandidate( ICECandidateKind::PeerReflexive, fromAddr );
            const STUNAttribute *pPriorityAttr = FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nSTUN_Attr_Priority );
            if ( pPriorityAttr != nullptr )
            {
                newRemoteCandidate.m_nPriority = ntohl( pPriorityAttr->m_pData[0] );
            }
            pRemoteCandidate = push_back_get_ptr( m_vecPeerCandidates, ICEPeerCandidate( newRemoteCandidate, SteamNetworkingIPAddrRender( fromAddr ).c_str() ) );
        }
        pThisPair = new ICECandidatePair( localCandidate, *pRemoteCandidate, m_role );
        m_vecCandidatePairs.push_back( pThisPair );
    }

    // RFC 8445 sec 7.3.1.4: queue a triggered check when a binding request arrives.
    // An incoming request proves the remote side can reach us, so retry immediately
    // rather than waiting for the normal schedule or a retransmit timeout.
    // Skip when USE-CANDIDATE is set — that path handles its own triggered check below.
    if ( !FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nSTUN_Attr_UseCandidate )
        && pThisPair->m_nState != kICECandidatePairState_Succeeded )
    {
        if ( pThisPair->m_pPeerRequest != nullptr )
        {
            // InProgress: cancel the existing request so we don't have two in flight.
            pThisPair->m_pPeerRequest->Cancel();
            pThisPair->m_pPeerRequest = nullptr;
        }
        pThisPair->m_nState = kICECandidatePairState_Waiting;
        if ( !has_element( m_vecTriggeredCheckQueue, pThisPair ) )
            m_vecTriggeredCheckQueue.push_back( pThisPair );
    }

    if ( FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nSTUN_Attr_UseCandidate ) )
    {
        if ( pThisPair->m_nState == kICECandidatePairState_Succeeded
                && ( m_pSelectedCandidatePair == nullptr || m_pSelectedCandidatePair == pThisPair ) )
        {
            SetSelectedCandidatePair( pThisPair );
        }
        else if ( m_pSelectedCandidatePair == nullptr )
        {
            bool bAlreadyHaveANomination = ( m_pSelectedCandidatePair != nullptr );
            for ( ICECandidatePair *pOtherPair : m_vecCandidatePairs )
            {
                if ( pOtherPair->m_bNominated == true
                    && ( pOtherPair->m_nState == kICECandidatePairState_InProgress || pOtherPair->m_nState == kICECandidatePairState_Waiting ) )
                    bAlreadyHaveANomination = true;
            }

            // Do we already have a valid triggered check in flight?
            if ( pThisPair->m_pPeerRequest != nullptr )
            {
                pThisPair->m_pPeerRequest->Cancel();
                pThisPair->m_pPeerRequest = nullptr;
                pThisPair->m_nState = kICECandidatePairState_Waiting;
            }

            if ( !bAlreadyHaveANomination )
            {
                pThisPair->m_nState = kICECandidatePairState_Waiting;
                pThisPair->m_bNominated = true;
                m_vecTriggeredCheckQueue.push_back( pThisPair );
            }
        }
    }

    if ( m_strIncomingUsername.size() > 0 )
    {
        STUNAttribute attrUsername;
        attrUsername.m_nType = k_nSTUN_Attr_UserName;
        attrUsername.m_nLength = (int)m_strIncomingUsername.size();
        attrUsername.m_pData = reinterpret_cast<const uint32 *>( m_strIncomingUsername.c_str() );
        outAttrs.AddToTail( attrUsername );
    }

    {
        uint32 responseBuffer[ k_nSTUN_MaxPacketSize_Bytes / 4 ];
        const int nByteCount = EncodeSTUNPacket( responseBuffer, k_nSTUN_BindingResponse, m_nEncoding, header.m_nTransactionID, fromAddr,
            (const uint8*)m_strLocalPassword.c_str(), (uint32)m_strLocalPassword.size(), outAttrs.Base(), outAttrs.Count() );
        if ( nByteCount > 0 )
        {
            SpewMsg( "Sending a STUN response to %s from %s.", SteamNetworkingIPAddrRender( fromAddr, true ).c_str(), SteamNetworkingIPAddrRender( pInterface->m_pSocket->m_boundAddr, true ).c_str() );
            iovec iov{ responseBuffer, (size_t)nByteCount };
            pInterface->SendPacketGather( 1, &iov, nByteCount, fromAddr, localCandidate.m_addrTURNServer );
        }
    }
}

void CSteamNetworkingICESession::StaticPacketReceived( const RecvPktInfo_t &info, ICESessionInterface *pContext )
{
    pContext->m_session.OnPacketReceived( info, pContext, nullptr );
}

void CSteamNetworkingICESession::Think( SteamNetworkingMicroseconds usecNow )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::Think" );

    SetNextThinkTime( usecNow + SteamNetworkingMicroseconds( 50000 ) ); // 50ms think rate

    if ( m_bInterfaceListStale )
    {
        GatherInterfaces();
        // We tried to update interfaces but failed. Try again later.
        if ( m_bInterfaceListStale )
            return;
    }

    Think_KeepAliveOnCandidates( usecNow );
    Think_DiscoverServerReflexiveCandidates();
    Think_DiscoverRelayCandidate();
    Think_TURNCreatePermissions();

    // Don't start checks before we have peer candidates and the remote password --
    // we'd send unauthenticated requests and couldn't verify the response integrity.
    if ( !m_vecPeerCandidates.empty() && !m_strRemotePassword.empty() )
        Think_TestPeerConnectivity();
}

void CSteamNetworkingICESession::Think_DiscoverServerReflexiveCandidates()
{
    if ( m_vecSTUNServers.empty() )
        return;

    for ( const std::unique_ptr<ICESessionInterface> &pIntf : m_vecInterfaces )
    {
        // Skip if discovery is done or a request is already in flight.
        if ( !pIntf->m_addrSTUNServer.IsIPv6AllZeros() || pIntf->m_pPendingSTUNRequest != nullptr )
            continue;

        // Find the first STUN server matching this interface's address family.
        for ( const SteamNetworkingIPAddr &srv : m_vecSTUNServers )
        {
            if ( srv.IsIPv4() == pIntf->m_pSocket->m_boundAddr.IsIPv4() )
            {
                pIntf->QueueBindRequest( srv, &CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveCandidate, m_nEncoding | kSTUNPacketEncodingFlags_MappedAddress );
                break;
            }
        }
    }
}



void CSteamNetworkingICESession::Think_DiscoverRelayCandidate()
{
    if ( m_vecTURNServers.empty() )
        return;

    for ( const std::unique_ptr<ICESessionInterface> &pIntf : m_vecInterfaces )
    {
        // Skip if relay discovery is done or a request is already in flight.
        if ( !pIntf->m_addrTURNServer.IsIPv6AllZeros() || pIntf->m_pPendingSTUNRequest != nullptr )
            continue;

        // Find the first TURN server matching this interface's address family.
        for ( const SteamNetworkingIPAddr &srv : m_vecTURNServers )
        {
            if ( srv.IsIPv4() == pIntf->m_pSocket->m_boundAddr.IsIPv4() )
            {
                pIntf->QueueAllocateRequest( srv, &CSteamNetworkingICESession::STUNRequestCallback_AllocateRelay, m_nEncoding );
                break;
            }
        }
    }
}

void CSteamNetworkingICESession::STUNRequestCallback_AllocateRelay( const RecvSTUNPktInfo_t &info )
{
    ICESessionInterface * const pIntf = info.m_pRequest->m_pInterface;
    pIntf->m_pPendingSTUNRequest = nullptr;

    // If we already have a result for this interface, ignore duplicates.
    if ( !pIntf->m_addrTURNServer.IsIPv6AllZeros() )
        return;

    if ( info.m_pHeader != nullptr )
    {
        // Got a response — check for XOR-RELAYED-ADDRESS.
        const STUNAttribute *pRelayAttr = FindAttributeOfType( info.m_pAttributes, info.m_nAttributes, k_nTURN_Attr_XORRelayedAddress );
        if ( pRelayAttr != nullptr )
        {
            SteamNetworkingIPAddr addrRelayed;
            addrRelayed.Clear();
            if ( ReadXORAddressAttribute( pRelayAttr, info.m_pHeader, &addrRelayed ) )
            {
                pIntf->m_addrTURNServer = info.m_pRequest->m_remoteAddr;
                pIntf->m_addrRelayed = addrRelayed;
                pIntf->m_bRelayFailed = false;
                pIntf->NotifyLocalCandidateDiscovered( ICECandidateKind::Relayed, addrRelayed );
                return;
            }
        }
        // Response received but no usable relay address — error response.
        pIntf->m_addrTURNServer = info.m_pRequest->m_remoteAddr;
        pIntf->m_bRelayFailed = true;
        return;
    }

    // Timed out — try the next TURN server if available.
    const int nTURNServerIdx = index_of( m_vecTURNServers, info.m_pRequest->m_remoteAddr );
    const int nNextTURNServerIdx = nTURNServerIdx + 1;
    if ( nTURNServerIdx < 0 || nNextTURNServerIdx >= len( m_vecTURNServers ) )
    {
        // Exhausted all TURN servers. Mark failed.
        pIntf->m_addrTURNServer = info.m_pRequest->m_remoteAddr;
        pIntf->m_bRelayFailed = true;
        return;
    }

    // Try the next TURN server.
    pIntf->QueueAllocateRequest( m_vecTURNServers[nNextTURNServerIdx], &CSteamNetworkingICESession::STUNRequestCallback_AllocateRelay, m_nEncoding );
}

void CSteamNetworkingICESession::Think_TURNCreatePermissions()
{
    for ( const std::unique_ptr<ICESessionInterface> &pIntf : m_vecInterfaces )
    {
        // Must have a completed relay allocation, not a busy slot, and no in-flight request.
        if ( pIntf->m_addrTURNServer.IsIPv6AllZeros() || pIntf->m_bRelayFailed )
            continue;
        if ( pIntf->m_pPendingSTUNRequest != nullptr )
            continue;

        bool bIsIPv4 = pIntf->m_pSocket->m_boundAddr.IsIPv4();
        const std_vector<SteamNetworkingIPAddr> &vecPermitted = bIsIPv4 ? m_vecTURNPermittedIPv4 : m_vecTURNPermittedIPv6;
        int nSessionRevision = bIsIPv4 ? m_nTURNPermissionRevisionIPv4 : m_nTURNPermissionRevisionIPv6;

        if ( pIntf->m_nTURNPermissionRevision >= nSessionRevision || vecPermitted.empty() )
            continue;

        // Build one XOR-PEER-ADDRESS attribute per permitted IP.
        // STUNAttribute is a non-owning view; back the data in stack buffers.
        // IPv4 value: 8 bytes (2 uint32s); IPv6 value: 20 bytes (5 uint32s).
        const int k_nMaxPeers = 10;
        STUNAttribute peerAttrs[ k_nMaxPeers ];
        uint32 peerAttrData[ k_nMaxPeers ][ 5 ];

        // Construct the request first so m_nTransactionID is set before we XOR IPv6 addrs.
        auto *pRequest = new CSteamNetworkingSocketsSTUNRequest( pIntf.get() );

        int nPeerAttrs = 0;
        for ( const SteamNetworkingIPAddr &addr : vecPermitted )
        {
            if ( nPeerAttrs >= k_nMaxPeers )
                break;
            if ( addr.IsIPv4() )
            {
                // Port is zero; XOR-port = 0 ^ (magic >> 16) = magic >> 16.
                peerAttrData[nPeerAttrs][0] = htonl( (0x0001u << 16) | (k_nSTUN_CookieValue >> 16) );
                peerAttrData[nPeerAttrs][1] = htonl( addr.GetIPv4() ^ k_nSTUN_CookieValue );
                peerAttrs[nPeerAttrs].m_nType   = k_nTURN_Attr_XORPeerAddress;
                peerAttrs[nPeerAttrs].m_nLength = 8;
                peerAttrs[nPeerAttrs].m_pData   = peerAttrData[nPeerAttrs];
            }
            else
            {
                peerAttrData[nPeerAttrs][0] = htonl( (0x0002u << 16) | (k_nSTUN_CookieValue >> 16) );
                V_memcpy( &peerAttrData[nPeerAttrs][1], addr.m_ipv6, 16 );
                peerAttrData[nPeerAttrs][1] ^= htonl( k_nSTUN_CookieValue );
                peerAttrData[nPeerAttrs][2] ^= pRequest->m_nTransactionID[0];
                peerAttrData[nPeerAttrs][3] ^= pRequest->m_nTransactionID[1];
                peerAttrData[nPeerAttrs][4] ^= pRequest->m_nTransactionID[2];
                peerAttrs[nPeerAttrs].m_nType   = k_nTURN_Attr_XORPeerAddress;
                peerAttrs[nPeerAttrs].m_nLength = 20;
                peerAttrs[nPeerAttrs].m_pData   = peerAttrData[nPeerAttrs];
            }
            ++nPeerAttrs;
        }

        pRequest->m_nTURNPermissionRevision = nSessionRevision;
        pIntf->m_pPendingSTUNRequest = pRequest;
        pRequest->Queue( k_nTURN_CreatePermissionRequest, m_nEncoding | kSTUNPacketEncodingFlags_NoMappedAddress,
            pIntf->m_addrTURNServer, &CSteamNetworkingICESession::STUNRequestCallback_CreatePermission,
            peerAttrs, nPeerAttrs );
    }
}

void CSteamNetworkingICESession::STUNRequestCallback_CreatePermission( const RecvSTUNPktInfo_t &info )
{
    ICESessionInterface * const pIntf = info.m_pRequest->m_pInterface;
    pIntf->m_pPendingSTUNRequest = nullptr;

    if ( info.m_pHeader == nullptr )
    {
        // Timed out.  m_nTURNPermissionRevision is still stale, so Think_TURNCreatePermissions
        // will retry on the next Think() sweep.
        return;
    }

    // Advance to the revision that was current when we sent this request.
    // If more IPs arrived while the request was in flight, the revision will still be
    // behind the session's current revision, and we'll send another CreatePermission.
    pIntf->m_nTURNPermissionRevision = info.m_pRequest->m_nTURNPermissionRevision;
}

void CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveCandidate( const RecvSTUNPktInfo_t &info )
{
    ICESessionInterface * const pIntf = info.m_pRequest->m_pInterface;
    pIntf->m_pPendingSTUNRequest = nullptr;

    // If we already have a real SR address for this interface, ignore duplicate responses.
    // (A previous failed-placeholder is overwriteable — that means we set bServerReflexiveFailed
    // earlier but a late response arrived; accept it.)
    if ( !pIntf->m_addrServerReflexive.IsIPv6AllZeros() )
        return;

    SteamNetworkingIPAddr bindResult;
    bindResult.Clear();
    if ( ReadAnyMappedAddress( info.m_pAttributes, info.m_nAttributes, info.m_pHeader, &bindResult ) )
    {
        // Got a response.  If mapped address == local address we're not behind a NAT:
        // record the STUN server so discovery is marked done, but don't advertise.
        pIntf->m_addrSTUNServer = info.m_pRequest->m_remoteAddr;
        if ( bindResult == pIntf->m_pSocket->m_boundAddr )
            bindResult.Clear();
        pIntf->m_addrServerReflexive = bindResult;
        pIntf->m_bServerReflexiveFailed = false;
        if ( !pIntf->m_addrServerReflexive.IsIPv6AllZeros() )
            pIntf->NotifyLocalCandidateDiscovered( ICECandidateKind::ServerReflexive, pIntf->m_addrServerReflexive );
        return;
    }

    // Timed out to this STUN server — try the next one if available.
    const int nSTUNServerIdx = index_of( m_vecSTUNServers, info.m_pRequest->m_remoteAddr );
    const int nNextSTUNServerIdx = nSTUNServerIdx + 1;
    if ( nSTUNServerIdx < 0 || nNextSTUNServerIdx >= len( m_vecSTUNServers ) )
    {
        // Exhausted all STUN servers.  Mark failed so Think_DiscoverServerReflexiveCandidates
        // does not retry this interface indefinitely.
        pIntf->m_addrSTUNServer = info.m_pRequest->m_remoteAddr;
        pIntf->m_bServerReflexiveFailed = true;
        return;
    }

    // Try the next server
    pIntf->QueueBindRequest( m_vecSTUNServers[nNextSTUNServerIdx], &CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveCandidate, m_nEncoding );
}

void CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveKeepAlive( const RecvSTUNPktInfo_t &info )
{
    ICESessionInterface * const pIntf = info.m_pRequest->m_pInterface;
    pIntf->m_pPendingSTUNRequest = nullptr;

    SteamNetworkingIPAddr bindResult;
    bindResult.Clear();
    if ( ReadAnyMappedAddress( info.m_pAttributes, info.m_nAttributes, info.m_pHeader, &bindResult ) )
    {
        if ( !( pIntf->m_addrSTUNServer == info.m_pRequest->m_remoteAddr ) )
            pIntf->m_addrSTUNServer = info.m_pRequest->m_remoteAddr;
        if ( !( pIntf->m_addrServerReflexive == bindResult ) )
            /*STUN server gave us a new address - what should we do here?*/
            SpewError( "Mismatching address in STUN response: got %s expected %s.", SteamNetworkingIPAddrRender( bindResult, true ).c_str(), SteamNetworkingIPAddrRender( pIntf->m_addrServerReflexive, true ).c_str() );
        return;
    }

    // Timed out — try the next STUN server (cycling through the list).
    if ( m_vecSTUNServers.empty() )
        return;

    const int nSTUNServerIdx = std::max( 0, index_of( m_vecSTUNServers, info.m_pRequest->m_remoteAddr ) );
    const int nNextSTUNServerIdx = ( nSTUNServerIdx + 1 ) % len( m_vecSTUNServers );
    pIntf->QueueBindRequest( m_vecSTUNServers[ nNextSTUNServerIdx ], &CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveKeepAlive, m_nEncoding );
}

void CSteamNetworkingICESession::UpdateKeepalive( ICESessionInterface *pIntf )
{
    if ( pIntf->m_addrServerReflexive.IsIPv6AllZeros() )
        return;
    if ( pIntf->m_pPendingSTUNRequest != nullptr )
        return;

    pIntf->QueueBindRequest( pIntf->m_addrSTUNServer, &CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveKeepAlive, m_nEncoding );
}

void CSteamNetworkingICESession::Think_KeepAliveOnCandidates( SteamNetworkingMicroseconds usecNow )
{
    if ( usecNow < m_nextKeepalive )
        return;

    m_nextKeepalive = usecNow + SteamNetworkingMicroseconds( 15 * 1000 * 1000 );

    if ( m_pSelectedCandidatePair != nullptr )
    {
        UpdateKeepalive( m_pSelectedCandidatePair->m_localCandidate.m_pInterface );
    }
    else
    {
        for ( const std::unique_ptr<ICESessionInterface> &pIntf : m_vecInterfaces )
            UpdateKeepalive( pIntf.get() );
    }
}

void CSteamNetworkingICESession::Think_TestPeerConnectivity()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::Think_TestPeerConnectivity" );

    if ( m_bCandidatePairsNeedUpdate )
    {
        m_bCandidatePairsNeedUpdate = false;

        // For every local candidate (host + relay if available), for every peer candidate,
        // make sure the pair is present.
        for ( const std::unique_ptr<ICESessionInterface> &pIntf : m_vecInterfaces )
        {
            ICELocalCandidate localCandidates[2];
            int nLocalCandidates = 0;

            // Host candidate — always present.
            localCandidates[nLocalCandidates++] = { pIntf.get(), {} };

            // Relay candidate — only when an allocation has succeeded.
            if ( !pIntf->m_addrTURNServer.IsIPv6AllZeros() && !pIntf->m_bRelayFailed )
                localCandidates[nLocalCandidates++] = { pIntf.get(), pIntf->m_addrTURNServer };

            for ( int iLocal = 0; iLocal < nLocalCandidates; ++iLocal )
            {
                const ICELocalCandidate &localCand = localCandidates[iLocal];
                for ( ICEPeerCandidate &remoteCandidate : m_vecPeerCandidates )
                {
                    if ( localCand.m_pInterface->m_pSocket->m_boundAddr.IsIPv4() != remoteCandidate.m_addr.IsIPv4() )
                        continue;
                    bool bFound = false;
                    for ( ICECandidatePair *pPair : m_vecCandidatePairs )
                    {
                        if ( pPair->m_localCandidate.m_pInterface == localCand.m_pInterface
                            && pPair->m_localCandidate.m_addrTURNServer == localCand.m_addrTURNServer
                            && pPair->m_remoteCandidate.m_addr == remoteCandidate.m_addr )
                        {
                            bFound = true;
                            break;
                        }
                    }
                    if ( !bFound )
                    {
                        ICECandidatePair *pNewCandidatePair = new ICECandidatePair( localCand, remoteCandidate, m_role );
                        m_vecCandidatePairs.push_back( pNewCandidatePair );
                    }
                }
            }
        }

        std::sort( m_vecCandidatePairs.begin(), m_vecCandidatePairs.end(), []( const ICECandidatePair *pA, const ICECandidatePair *pB ) { return pA->m_nPriority > pB->m_nPriority; } );
    }

    ICECandidatePair *pPairToCheck = nullptr;

    if ( !m_vecTriggeredCheckQueue.empty() )
    {
        // RFC 8445 sec 5.1.3.2: triggered checks are processed in FIFO order.
        pPairToCheck = m_vecTriggeredCheckQueue[ 0 ];
        erase_at( m_vecTriggeredCheckQueue, 0 );
    }

    if ( pPairToCheck == nullptr )
    {
        for ( ICECandidatePair *pCandidatePair : m_vecCandidatePairs )
        {
            if ( pCandidatePair->m_nState == kICECandidatePairState_Waiting )
            {
                pPairToCheck = pCandidatePair;
                break;
            }
        }
    }

    if ( pPairToCheck == nullptr )
    {
        std_vector< const char * > vecFoundationsUsed;
        for ( ICECandidatePair *pCandidatePair : m_vecCandidatePairs )
        {
			const char *pszFoundation = pCandidatePair->m_remoteCandidate.m_sFoundation.c_str();
            if ( pCandidatePair->m_nState == kICECandidatePairState_InProgress )
            {
                vecFoundationsUsed.push_back( pszFoundation );
                continue;
            }
            if ( pCandidatePair->m_nState != kICECandidatePairState_Frozen )
                continue;

			bool bFound = false;
			for ( const char *pszUsed: vecFoundationsUsed )
			{
				if ( V_stricmp( pszUsed, pszFoundation ) == 0 )
				{
					bFound = true;
					break;
				}
			}
			if ( bFound )
				continue;

            vecFoundationsUsed.push_back( pszFoundation );
            pCandidatePair->m_nState = kICECandidatePairState_Waiting;
            if ( pPairToCheck == nullptr )
                pPairToCheck = pCandidatePair;
        }
    }

    if ( pPairToCheck != nullptr )
    {
        // Trigger the connectivity check here...
        ICESessionInterface * const pIntf = pPairToCheck->m_localCandidate.m_pInterface;
        pPairToCheck->m_nState = kICECandidatePairState_InProgress;
        pPairToCheck->m_pPeerRequest = new CSteamNetworkingSocketsSTUNRequest( pIntf );

        // Build all extra attributes on the stack; Queue() serializes them into the stored packet.
        STUNAttribute extraAttrs[5];
        int nExtraAttrs = 0;
        uint32 uUsernameBuf[ k_nSTUN_MaxPacketSize_Bytes / 4 ];
        uint32 uPriority;
        uint32 uRoleBuf[2];

        if ( m_strOutgoingUsername.size() > 0 )
        {
            const int nUsernameLength = (int)m_strOutgoingUsername.size();
            V_memcpy( uUsernameBuf, m_strOutgoingUsername.c_str(), nUsernameLength );
            extraAttrs[nExtraAttrs].m_nType   = k_nSTUN_Attr_UserName;
            extraAttrs[nExtraAttrs].m_nLength = nUsernameLength;
            extraAttrs[nExtraAttrs].m_pData   = uUsernameBuf;
            ++nExtraAttrs;
        }

        {
            // RFC 8445 section 7.2.2: priority attr uses peer-reflexive type preference (110).
            uPriority = htonl( ( 110u << 24 ) | ( ( pPairToCheck->m_localCandidate.m_pInterface->m_nPriority & 0xFFFF ) << 8 ) | 255u );
            extraAttrs[nExtraAttrs].m_nType   = k_nSTUN_Attr_Priority;
            extraAttrs[nExtraAttrs].m_nLength = 4;
            extraAttrs[nExtraAttrs].m_pData   = &uPriority;
            ++nExtraAttrs;
        }

        if ( m_role == k_EICERole_Controlling )
        {
            *(uint64*)uRoleBuf = m_nRoleTiebreaker;
            uRoleBuf[0] = htonl( uRoleBuf[0] );
            uRoleBuf[1] = htonl( uRoleBuf[1] );
            extraAttrs[nExtraAttrs].m_nType   = k_nSTUN_Attr_ICEControlling;
            extraAttrs[nExtraAttrs].m_nLength = 8;
            extraAttrs[nExtraAttrs].m_pData   = uRoleBuf;
            ++nExtraAttrs;

			if ( pPairToCheck->m_bNominated )
			{
				extraAttrs[nExtraAttrs].m_nType   = k_nSTUN_Attr_UseCandidate;
				extraAttrs[nExtraAttrs].m_nLength = 0;
				extraAttrs[nExtraAttrs].m_pData   = nullptr;
				++nExtraAttrs;
			}
        }
        else if ( m_role == k_EICERole_Controlled )
        {
            *(uint64*)uRoleBuf = m_nRoleTiebreaker;
            uRoleBuf[0] = htonl( uRoleBuf[0] );
            uRoleBuf[1] = htonl( uRoleBuf[1] );
            extraAttrs[nExtraAttrs].m_nType   = k_nSTUN_Attr_ICEControlled;
            extraAttrs[nExtraAttrs].m_nLength = 8;
            extraAttrs[nExtraAttrs].m_pData   = uRoleBuf;
            ++nExtraAttrs;
        }

        pPairToCheck->m_pPeerRequest->m_strPassword = m_strRemotePassword;

        pPairToCheck->m_pPeerRequest->Queue( k_nSTUN_BindingRequest, m_nEncoding | kSTUNPacketEncodingFlags_NoMappedAddress, pPairToCheck->m_remoteCandidate.m_addr, &CSteamNetworkingICESession::STUNRequestCallback_PeerConnectivityCheck, extraAttrs, nExtraAttrs );
        pPairToCheck->m_pPeerRequest->m_addrRelay = pPairToCheck->m_localCandidate.m_addrTURNServer;
        m_vecPendingPeerRequests.push_back( pPairToCheck->m_pPeerRequest );
    }
}

void CSteamNetworkingICESession::STUNRequestCallback_PeerConnectivityCheck( const RecvSTUNPktInfo_t &info )
{
    find_and_remove_element( m_vecPendingPeerRequests, info.m_pRequest );
    ICECandidatePair *pPair = nullptr;
    for ( ICECandidatePair *pCandidatePair : m_vecCandidatePairs )
    {
		if ( pCandidatePair->m_pPeerRequest == info.m_pRequest )
		{
			pCandidatePair->m_pPeerRequest = nullptr;
			pPair = pCandidatePair;
			break;
		}
    }

	if ( pPair == nullptr )
		return;

    const SteamNetworkingMicroseconds usPing = Max( SteamNetworkingMicroseconds( 1 ), info.m_usecNow - info.m_pRequest->m_usecLastSentTime );
    pPair->m_nLastRecordedPing = Max( 1, (int)( usPing / 1000 ) );

    // Stale request on a pair we're not using? Ignore.
    if ( m_pSelectedCandidatePair != nullptr && m_pSelectedCandidatePair != pPair )
    {
        return;
    }

    if ( info.m_pHeader == nullptr )
    {
        pPair->m_nState = kICECandidatePairState_Failed;
        return;
    }
    pPair->m_nState = kICECandidatePairState_Succeeded;
    if ( pPair->m_bNominated )
    {
        SetSelectedCandidatePair( pPair );
    }
	else if ( m_role == k_EICERole_Controlling )
    {
		// Once we have a selected pair, or any nominated pair (including ones queued
		// but not yet sent), don't nominate more.
		bool bAlreadyHaveANomination = ( m_pSelectedCandidatePair != nullptr );
        for ( ICECandidatePair *pOtherPair : m_vecCandidatePairs )
        {
            if ( pOtherPair->m_bNominated )
                bAlreadyHaveANomination = true;
        }
		if ( !bAlreadyHaveANomination )
		{
			pPair->m_bNominated = true;
			m_vecTriggeredCheckQueue.push_back( pPair );
		}

    }
}


/////////////////////////////////////////////////////////////////////////////
//
// ICESessionInterface / CSteamNetworkingICESession::ICECandidateBase
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingICESession::ICECandidateBase::ICECandidateBase()
{
    m_type = ICECandidateKind::None;
    m_addr.Clear();
    m_nPriority = 0;
}

CSteamNetworkingICESession::ICECandidateBase::ICECandidateBase( ICECandidateKind t, const SteamNetworkingIPAddr& addr )
{
    m_type = t;
    m_addr = addr;
    m_nPriority = 0;
}

// Compute the RFC 5245 candidate-attribute string, determine the family-specific
// EICECandidateType, and dispatch OnLocalCandidateDiscovered to the session callbacks.
// Ex: candidate:2442523459 0 udp 2122262784 2602:801:f001:1034:5078:221c:76b:a3d6 63368 typ host generation 0 ufrag WLM82 network-id 2
void ICESessionInterface::NotifyLocalCandidateDiscovered( ICECandidateKind kind, const SteamNetworkingIPAddr& addr )
{
    CSteamNetworkingICESessionCallbacks *pCallbacks = m_session.m_pCallbacks;
    if ( pCallbacks == nullptr )
        return;

    const SteamNetworkingIPAddr &base = m_pSocket->m_boundAddr;

    // priority = (2^24)*(type preference) + (2^8)*(local preference) + (2^0)*(256 - component ID)
    uint32 nTypePreference = 0;
    switch ( kind )
    {
        case ICECandidateKind::Host:            nTypePreference = 126; break;
        case ICECandidateKind::ServerReflexive: nTypePreference = 100; break;
        case ICECandidateKind::Relayed:         nTypePreference =   0; break;
        case ICECandidateKind::PeerReflexive:   nTypePreference = 110; break;
        default:                                nTypePreference =   0; break;
    }
    const uint32 nPriority = ( nTypePreference << 24 ) + ( ( m_nPriority & 0xFFFF ) << 8 ) + 255u;

    /* <foundation>:  is composed of 1 to 32 <ice-char>s.  It is an
      identifier that is equivalent for two candidates that are of the
      same type, share the same base, and come from the same STUN
      server.*/
    uint32 nFoundation = 0;
    {
        uint16 uCounter = 0;
        for ( int i = 0; i < 16; ++i )
        {
            uCounter += base.m_ipv6[i];
            uCounter += m_addrSTUNServer.m_ipv6[i];
        }
        nFoundation = ( base.m_port + m_addrSTUNServer.m_port ) + ( uCounter << 15 ) + (int)kind;
    }

    char connectionAddr[ SteamNetworkingIPAddr::k_cchMaxString];
    addr.ToString( connectionAddr, V_ARRAYSIZE( connectionAddr ), false );
    const char *pszType = "";
    switch ( kind )
    {
        case ICECandidateKind::Host:            pszType = "host";  break;
        case ICECandidateKind::ServerReflexive: pszType = "srflx"; break;
        case ICECandidateKind::Relayed:         pszType = "relay"; break;
        case ICECandidateKind::PeerReflexive:   pszType = "prflx"; break;
        default: break;
    }
    // TODO: relay candidates should include rel-addr/rel-port (RFC 5245 §15.1)
    char szCandidate[128];
    V_snprintf( szCandidate, sizeof(szCandidate), "candidate:%u 0 udp %u %s %d typ %s", nFoundation, nPriority, connectionAddr, addr.m_port, pszType );

    EICECandidateType eType = CalcICECandidateType( kind, addr );
    pCallbacks->OnLocalCandidateDiscovered( eType, szCandidate );
}

static bool IsPrivateIPv4( const uint8 m_ip[ 4 ] )
{
	/*	Class A: 10.0. 0.0 to 10.255. 255.255.
		Class B: 172.16. 0.0 to 172.31. 255.255.
		Class C: 192.168. 0.0 to 192.168. 255.255. */
	if ( m_ip[0] == 10 )
		return true;
	if ( m_ip[0] == 172 && m_ip[1] >= 16 && m_ip[1] <= 31 )
		return true;
	if ( m_ip[0] == 192 && m_ip[1] == 168 )
		return true;
	return false;
}

EICECandidateType CalcICECandidateType( ICECandidateKind kind, const SteamNetworkingIPAddr& addr )
{
	switch ( kind )
	{
	case ICECandidateKind::Host:
		if ( addr.IsIPv4() )
		{
			if ( IsPrivateIPv4( addr.m_ipv4.m_ip ) )
				return k_EICECandidate_IPv4_HostPrivate;
			else
				return k_EICECandidate_IPv4_HostPublic;
		}
		else
		{
			return k_EICECandidate_IPv6_HostPublic;
		}
		break;
	case ICECandidateKind::ServerReflexive:
	case ICECandidateKind::PeerReflexive:
		if ( addr.IsIPv4() )
			return k_EICECandidate_IPv4_Reflexive;
		else
			return k_EICECandidate_IPv6_Reflexive;
		break;

	case ICECandidateKind::Relayed:
		if ( addr.IsIPv4() )
			return k_EICECandidate_IPv4_Relay;
		else
			return k_EICECandidate_IPv6_Relay;
		break;
	default:
		break;
	}

	return k_EICECandidate_Invalid;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingICESession::ICECandidatePair
//
/////////////////////////////////////////////////////////////////////////////
CSteamNetworkingICESession::ICECandidatePair::ICECandidatePair( const ICELocalCandidate& localCandidate, const ICEPeerCandidate& remoteCandidate, EICERole role )
    : m_localCandidate( localCandidate ),
      m_remoteCandidate( remoteCandidate ),
      m_nState( kICECandidatePairState_Frozen ),
      m_bNominated( false )
{
    // RFC 8445 §5.1.2 type preference: host=126, srflx=100, relay=0.
    const uint32 nTypePreference = localCandidate.IsRelay() ? 0u : 126u;
    const uint32 nLocalPriority = ( nTypePreference << 24 ) + ( ( localCandidate.m_pInterface->m_nPriority & 0xFFFF ) << 8 ) + 255u;
    const uint64 D = ( role == k_EICERole_Controlling ) ? nLocalPriority : remoteCandidate.m_nPriority;
    const uint64 G = ( role == k_EICERole_Controlling ) ? remoteCandidate.m_nPriority : nLocalPriority;
    m_nPriority = ( 1ull << 32 ) * MIN( G, D ) + 2 * MAX( G, D ) + ( G > D ? 1 : 0 );
    m_pPeerRequest = nullptr;
	m_nLastRecordedPing = -1;
}

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportP2PICE_Valve
//
/////////////////////////////////////////////////////////////////////////////
CConnectionTransportP2PICE_Valve::CConnectionTransportP2PICE_Valve( CSteamNetworkConnectionP2P &connection )
    : CConnectionTransportP2PICE( connection )
{
    m_pICESession = nullptr;
}

void CConnectionTransportP2PICE_Valve::Init( const ICESessionConfig& cfg )
{
	AssertLocksHeldByCurrentThread( "CConnectionTransportP2PICE_Valve::Init" );

    Assert( m_pICESession == nullptr );
	m_pICESession = new CSteamNetworkingICESession( cfg, this );
    m_pICESession->StartSession();
}

void CConnectionTransportP2PICE_Valve::TransportFreeResources()
{
	CConnectionTransport::TransportFreeResources();

    if ( m_pICESession != nullptr )
    {
        delete m_pICESession;
        m_pICESession = nullptr;
    }
}

bool CConnectionTransportP2PICE_Valve::BCanSendEndToEndData() const
{
    return m_pICESession->BCanSendEndToEnd();
}

void CConnectionTransportP2PICE_Valve::RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	AssertLocksHeldByCurrentThread( "CConnectionTransportP2PICE_Valve::RecvRendezvous" );

    if ( msg.has_auth() && msg.auth().has_pwd_frag() )
    {
        m_pICESession->SetRemoteUsername( Base64EncodeLower30Bits( ConnectionIDRemote() ).c_str() );
        m_pICESession->SetRemotePassword( msg.auth().pwd_frag().c_str() );
    }

    if ( msg.has_add_candidate() )
    {
        // candidate-attribute from https://datatracker.ietf.org/doc/html/rfc5245#section-15.1
        const std::string& s = msg.add_candidate().candidate();
        RFC5245CandidateAttr attr;
        if ( !ParseRFC5245CandidateAttribute( s.c_str(), &attr ) )
        {
            SpewMsg( "[%s] Failed to parse remote candidate \'%s\'\n",
                Connection().GetDescription(), s.c_str() );
        }
        else
        {
            SpewMsg( "[%s] Got remote candidate \'%s\'\n",
                Connection().GetDescription(), s.c_str() );
            EICECandidateType nType = m_pICESession->AddPeerCandidate( attr );
            Connection().m_msgICESessionSummary.set_remote_candidate_types(
                Connection().m_msgICESessionSummary.remote_candidate_types() | nType );
        }
    }
}

bool CConnectionTransportP2PICE_Valve::SendPacket( const void *pkt, int cbPkt )
{
	iovec temp;
	temp.iov_base = const_cast<void*>( pkt );
	temp.iov_len = cbPkt;
	return m_pICESession->SendPacketGather( 1, &temp, cbPkt );
}

bool CConnectionTransportP2PICE_Valve::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
	return m_pICESession->SendPacketGather( nChunks,pChunks, cbSendTotal );
}

void CConnectionTransportP2PICE_Valve::OnLocalCandidateDiscovered( EICECandidateType type, const char *pszCandidateStr )
{
    ConnectionScopeLock lock( Connection(), "OnLocalCandidateDiscovered");
    CMsgICECandidate c;
    c.set_candidate( pszCandidateStr );
    LocalCandidateGathered( type, std::move( c ) );
}

void CConnectionTransportP2PICE_Valve::OnConnectionSelected( const ICELocalCandidate& localCandidate, const CSteamNetworkingICESession::ICECandidateBase& remoteCandidate )
{
    ConnectionScopeLock lock( Connection(), "CConnectionTransportP2PICE_Valve::OnConnectionSelected");

    m_currentRouteRemoteAddress = remoteCandidate.m_addr;
    if ( localCandidate.IsRelay() || remoteCandidate.m_type == ICECandidateKind::Relayed )
        m_eCurrentRouteKind = k_ESteamNetTransport_TURN;
    else if ( IsRemoteAddressOnLocalSubnet( localCandidate.m_pInterface->m_pSocket->m_boundAddr, localCandidate.m_pInterface->m_nPrefixLen, remoteCandidate.m_addr ) )
        m_eCurrentRouteKind = k_ESteamNetTransport_UDPProbablyLocal;
    else
        m_eCurrentRouteKind = k_ESteamNetTransport_UDP;
    m_pingEndToEnd.Reset();
    m_pingEndToEnd.ReceivedPing( m_pICESession->GetPing(), SteamNetworkingSockets_GetLocalTimestamp() );
    Connection().TransportEndToEndConnectivityChanged( this, SteamNetworkingSockets_GetLocalTimestamp() );
}

void CConnectionTransportP2PICE_Valve::OnPacketReceived( const RecvPktInfo_t &info )
{
    ConnectionScopeLock lock( Connection(), "CConnectionTransportP2PICE_Valve::OnPacketReceived");
    ProcessPacket( (const uint8_t*)info.m_pPkt, info.m_cbPkt, info.m_usecNow );
}

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
