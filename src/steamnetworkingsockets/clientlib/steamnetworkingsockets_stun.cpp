//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Implementaiton of (the most important subset of) the ICE protocol
//
// https://datatracker.ietf.org/doc/html/rfc8489

#include "steamnetworkingsockets_stun.h"
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#include "csteamnetworkingsockets.h"
#include <tier0/platform_sockets.h>
#include "crypto.h"

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

namespace {
    
const uint32 k_nSTUN_MaxPacketSize_Bytes = 576;

static void ConvertNetAddr_tToSteamNetworkingIPAddr( const netadr_t& in, SteamNetworkingIPAddr *pOut );
static void ConvertSteamNetworkingIPAddrToNetAdr_t( const SteamNetworkingIPAddr& in, netadr_t *pOut );
static uint32 CRC32( const unsigned char *buf, int len );

static void UnpackSTUNHeader( const uint32 *pHeader, STUNHeader* pUnpackedHeader )
{
    if ( pHeader == nullptr || pUnpackedHeader == nullptr )
        return;

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

                  Figure 2: Format of STUN Message Header
    */          
    const uint32 nHeaderWord = ntohl( pHeader[0] );
    pUnpackedHeader->m_nZeroPad = ( nHeaderWord >> 30 ) & 3;
    pUnpackedHeader->m_nMessageType = ( nHeaderWord >> 16 ) & 0x3FFF;
    pUnpackedHeader->m_nMessageLength = ( nHeaderWord & 0xFFFF );
    pUnpackedHeader->m_nCookie = ntohl( pHeader[1] );
    pUnpackedHeader->m_nTransactionID[0] = pHeader[2]; // Treat transaction ID as opaque bits.
    pUnpackedHeader->m_nTransactionID[1] = pHeader[3];
    pUnpackedHeader->m_nTransactionID[2] = pHeader[4];
}

bool IsValidSTUNHeader( STUNHeader* pHeader, uint32 uPacketSize, uint32* pTransactionID )
{
    if ( pHeader == nullptr )
        return false;

    /*  The most significant 2 bits of every STUN message MUST be zeroes.
        This can be used to differentiate STUN packets from other protocols
        when STUN is multiplexed with other protocols on the same port. */
    if ( pHeader->m_nZeroPad != 0 )
        return false;

    /*  The message length MUST contain the size of the message in bytes, not
        including the 20-byte STUN header.  Since all STUN attributes are
        padded to a multiple of 4 bytes, the last 2 bits of this field are
        always zero.  This provides another way to distinguish STUN packets
        from packets of other protocols. */
    // if ( ( pHeader->nMessageLength & 3 ) != 0 )
    //     return false;
    if ( ( ( pHeader->m_nMessageLength + 20 ) != uPacketSize ) )
        return false;

    /*  The Magic Cookie field MUST contain the fixed value 0x2112A442 in
        network byte order. */
    if ( pHeader->m_nCookie != k_nSTUN_CookieValue )
        return false;

    /*  Verify transaction ID */
    if ( pTransactionID != nullptr )
    {
        if ( pTransactionID[0] != pHeader->m_nTransactionID[0] 
            || pTransactionID[1] != pHeader->m_nTransactionID[1] 
            || pTransactionID[2] != pHeader->m_nTransactionID[2] )
            return false;
    }
    return true;
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

static bool ReadFingerprintAttribute( const STUNAttribute *pAttr, const uint32* pMessageStart, const uint32* pAttributeStart )
{
    if ( pAttr == nullptr || pMessageStart == nullptr || pAttributeStart == nullptr || pAttributeStart < pMessageStart )
        return false;
    if ( pAttr->m_nType != k_nSTUN_Attr_Fingerprint )
        return false;
    if ( pAttr->m_nLength != 4 )
        return false;
    const uint32 uPacketCRCValue = ntohl( pAttr->m_pData[0] ) ^ 0x5354554e;
    const uint32 uDataCRCValue = CRC32( reinterpret_cast<const unsigned char*>( pMessageStart ), uint32( pAttributeStart - pMessageStart ) * 4 );
    
    if ( uPacketCRCValue != uDataCRCValue )
    {
        SpewMsg( "Fingerprint check failed: %x vs. %x", uPacketCRCValue, uDataCRCValue );
        return false;
    }

    return true;
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

static bool ReadMessageIntegritySHA256Attribute( const STUNAttribute *pAttr, const uint32* pMessageStart, const uint32* pAttributeStart, const uint8 *pubKey, uint32 cubKey )
{
    if ( pAttr == nullptr || pMessageStart == nullptr || pAttributeStart == nullptr || pAttributeStart < pMessageStart )
        return false;
    if ( pAttr->m_nType != k_nSTUN_Attr_MessageIntegrity_SHA256 )
        return false;
    if ( pAttr->m_nLength != k_cubSHA256Hash )
        return false;       

    const uint32 uOriginalMessageStartWordRaw = *pMessageStart;
    const uint32 uOriginalMessageStartWord = ntohl( uOriginalMessageStartWordRaw );
    const uint32 uAdjustedMessageLength = 4*( pAttributeStart - &pMessageStart[5] ) + 4+pAttr->m_nLength;
    uint32 uMessageStartHighWord = uOriginalMessageStartWord & 0xFFFF0000ul;
    uint32 uAdjustedMessageStartWord = uMessageStartHighWord | uAdjustedMessageLength;
    const uint32 uTruncatedStartWord = htonl( uAdjustedMessageStartWord );
    *(uint32*)( pMessageStart ) = uTruncatedStartWord;
    SHA256Digest_t digest;
  	CCrypto::GenerateHMAC256( reinterpret_cast<const uint8 *>( pMessageStart ), 4 * ( pAttributeStart - pMessageStart ), pubKey, cubKey, &digest );
    *(uint32*)( pMessageStart ) = uOriginalMessageStartWordRaw;
    if ( V_memcmp( pAttr->m_pData, &digest, k_cubSHA256Hash ) != 0 )
        return false;
    return true;
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

static bool ReadMessageIntegrityAttribute( const STUNAttribute *pAttr, const uint32* pMessageStart, const uint32* pAttributeStart, const uint8 *pubKey, uint32 cubKey )
{
    if ( pAttr == nullptr || pMessageStart == nullptr || pAttributeStart == nullptr || pAttributeStart < pMessageStart )
        return false;
    if ( pAttr->m_nType != k_nSTUN_Attr_MessageIntegrity )
        return false;
    if ( pAttr->m_nLength != k_cubSHA1Hash )
        return false;       

    const uint32 uOriginalMessageStartWordRaw = *pMessageStart;
    const uint32 uOriginalMessageStartWord = ntohl( uOriginalMessageStartWordRaw );
    const uint32 uAdjustedMessageLength = 4*( pAttributeStart - &pMessageStart[5] ) + 4+pAttr->m_nLength;
    uint32 uMessageStartHighWord = uOriginalMessageStartWord & 0xFFFF0000ul;
    uint32 uAdjustedMessageStartWord = uMessageStartHighWord | uAdjustedMessageLength;
    const uint32 uTruncatedStartWord = htonl( uAdjustedMessageStartWord );
    *(uint32*)( pMessageStart ) = uTruncatedStartWord;
    SHADigest_t digest;
  	CCrypto::GenerateHMAC( reinterpret_cast<const uint8 *>( pMessageStart ), 4*( pAttributeStart - pMessageStart ), pubKey, cubKey, &digest );
    *(uint32*)( pMessageStart ) = uOriginalMessageStartWordRaw;

    if ( V_memcmp( pAttr->m_pData, &digest, k_cubSHA1Hash ) != 0 )
    {
        const unsigned char* pszAttr = (const unsigned char*)pAttr->m_pData;
        const unsigned char* pszDigest = (const unsigned char*)(digest);
        SpewMsg( "Got %s expected %s\n", pszAttr, pszDigest );
        return false;
    }
    return true;
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

static bool DecodeSTUNPacket( const void *pPkt, uint32 cbPkt, uint32* nTransactionID, const uint8 *pubKey, uint32 cubKey, STUNHeader *pHeader, CUtlVector< STUNAttribute >* pVecAttrs )
{
    // Always require at least the 20 byte header.
    if ( pPkt == nullptr || cbPkt < 20 )
        return false;
 
    const uint32 * const pMessage = reinterpret_cast< const uint32* >( pPkt );
    UnpackSTUNHeader( pMessage, pHeader );
    if ( !IsValidSTUNHeader( pHeader, cbPkt, nTransactionID ) )
        return false;

    const uint32 * const pMessageEnd = reinterpret_cast< const uint32* >( pPkt ) + cbPkt / 4;
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
                // Failed fingerprint means this isn't actually a STUN message, so just bail.
                if ( !ReadFingerprintAttribute( &attr, pMessage, pThisAttrPtr ) )
                    return false;
                break;
            }

            case k_nSTUN_Attr_MessageIntegrity_SHA256:
            {
                // Failed Message Integrity means this is a malformed STUN message, so just bail.                
                if ( !ReadMessageIntegritySHA256Attribute( &attr, pMessage, pThisAttrPtr, pubKey, cubKey ) )
                    return false;
                break;
            }

            case k_nSTUN_Attr_MessageIntegrity:
            {
                // Failed Message Integrity means this is a malformed STUN message, so just bail.                
                if ( !ReadMessageIntegrityAttribute( &attr, pMessage, pThisAttrPtr, pubKey, cubKey ) )
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

static bool SendSTUNResponsePacket( IRawUDPSocket* pSocket, int nEncoding, uint32 *pTransactionID, const SteamNetworkingIPAddr& toAddr, const uint8 *pubKey, uint32 cubKey, STUNAttribute* pAttrs, int nAttrs )
{
    uint32 messageBuffer[ k_nSTUN_MaxPacketSize_Bytes / 4 ];
    const int nByteCount = EncodeSTUNPacket( messageBuffer, k_nSTUN_BindingResponse, nEncoding, pTransactionID, toAddr, pubKey, cubKey, pAttrs, nAttrs );
	for ( int i = 0; i < nAttrs; ++i )
		delete []( pAttrs[i].m_pData );
    if ( nByteCount == 0 )
        return false;

    SpewMsg( "Sending a STUN response to %s from %s.", SteamNetworkingIPAddrRender( toAddr, true ).c_str(), SteamNetworkingIPAddrRender( pSocket->m_boundAddr, true ).c_str() );
    {        
        netadr_t netadr_t_toAdr;
        ConvertSteamNetworkingIPAddrToNetAdr_t( toAddr, &netadr_t_toAdr );
        return pSocket->BSendRawPacket( messageBuffer, nByteCount, netadr_t_toAdr );
    }
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
struct RFC5245CandidateAttr {
    std::string sFoundation;
    int nComponent;
    std::string sTransport;
    int nPriority;
    std::string sAddress;
    int nPort;
    std::string sType;
    CSteamNetworkingICESession::ICECandidateType nType;
    CUtlVector< std::pair< std::string, std::string > > vAttrs;
};
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
        pAttr->sAddress.swap( connectionAddr );
    }
    pAttr->nPort = atoi( pPortBegin );
    {
        std::string candidateType( pCandidateTypeBegin, pCandidateTypeEnd - pCandidateTypeBegin );
        pAttr->sType.swap( candidateType );
    }
    if ( pAttr->sType == "host" )
        pAttr->nType = CSteamNetworkingICESession::kICECandidateType_Host;
    else if ( pAttr->sType == "srflx" )
        pAttr->nType = CSteamNetworkingICESession::kICECandidateType_ServerReflexive;
    else if ( pAttr->sType == "prflx" )
        pAttr->nType = CSteamNetworkingICESession::kICECandidateType_PeerReflexive;
    else if ( pAttr->sType == "relay" )
        pAttr->nType = CSteamNetworkingICESession::kICECandidateType_None; // CSteamNetworkingICESession::kICECandidateType_Relayed
    else
        pAttr->nType = CSteamNetworkingICESession::kICECandidateType_None;
    for ( int i = 0; i < vAttrNameBegin.Count(); ++i )
    {
        pAttr->vAttrs.AddToTail( std::pair<std::string,std::string>( std::string( vAttrNameBegin[i], vAttrNameEnd[i]-vAttrNameBegin[i] ), std::string( vAttrValueBegin[i], vAttrValueEnd[i]-vAttrValueBegin[i] ) ) );
    }
    return true;
}

} // namespace <anonymous>


/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingSocketsSTUNRequest
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingSocketsSTUNRequest::CSteamNetworkingSocketsSTUNRequest()
{
}

CSteamNetworkingSocketsSTUNRequest::~CSteamNetworkingSocketsSTUNRequest()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

    if ( m_pSocket != nullptr )
    {
        m_pSocket->Close();
        m_pSocket = nullptr;
    }
    for ( STUNAttribute &a : m_vecExtraAttrs )
    {
        if ( a.m_pData != nullptr )
            delete []( a.m_pData );
        a.m_pData = nullptr;
    }
    m_vecExtraAttrs.RemoveAll();
}

void CSteamNetworkingSocketsSTUNRequest::Send( SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb )
{
    m_remoteAddr = remoteAddr;
    m_nRetryCount = 0;
    m_nMaxRetries = 7;
    m_callback = cb;
	m_usecLastSentTime = 0;
    CCrypto::GenerateRandomBlock( m_nTransactionID, 12 );
    SetNextThinkTimeASAP();
}

CSteamNetworkingSocketsSTUNRequest *CSteamNetworkingSocketsSTUNRequest::SendBindRequest( IBoundUDPSocket *pBoundSock, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding ) 
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingSocketsSTUNRequest::SendBindRequest" );

    if ( pBoundSock == nullptr || pBoundSock->GetRawSock() == nullptr )
        return nullptr;

    CSteamNetworkingSocketsSTUNRequest * pRequest = new CSteamNetworkingSocketsSTUNRequest;
    netadr_t remoteNetAddr;
    ConvertSteamNetworkingIPAddrToNetAdr_t( remoteAddr, &remoteNetAddr );
    pRequest->m_pSocket = pBoundSock;
    pRequest->m_localAddr = pBoundSock->GetRawSock()->m_boundAddr;
    pRequest->m_nEncoding = nEncoding;
    pRequest->Send( remoteAddr, cb );
    return pRequest;
}

CSteamNetworkingSocketsSTUNRequest *CSteamNetworkingSocketsSTUNRequest::SendBindRequest( CSharedSocket *pSharedSock, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding ) 
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingSocketsSTUNRequest::SendBindRequest" );

    if ( pSharedSock == nullptr )
        return nullptr;

    const SteamNetworkingIPAddr *pLocalAddr = pSharedSock->GetBoundAddr();
    if ( pLocalAddr == nullptr )
        return nullptr;

    CSteamNetworkingSocketsSTUNRequest * pRequest = new CSteamNetworkingSocketsSTUNRequest;
    {
        pRequest->m_localAddr = *pLocalAddr;
        pRequest->m_nEncoding = nEncoding;
        netadr_t remoteNetAddr;
        ConvertSteamNetworkingIPAddrToNetAdr_t( remoteAddr, &remoteNetAddr );
        pRequest->m_pSocket = pSharedSock->AddRemoteHost( remoteNetAddr, CRecvPacketCallback( StaticPacketReceived, pRequest ) );
		if ( pRequest->m_pSocket == nullptr )
		{
			delete pRequest;
			return nullptr;
		}
    }
    pRequest->Send( remoteAddr, cb );
    return pRequest;
}

CSteamNetworkingSocketsSTUNRequest *CSteamNetworkingSocketsSTUNRequest::CreatePeerConnectivityCheckRequest( CSharedSocket *pSharedSock, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding ) 
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingSocketsSTUNRequest::CreatePeerConnectivityCheckRequest" );

    if ( pSharedSock == nullptr )
        return nullptr;

    const SteamNetworkingIPAddr *pLocalAddr = pSharedSock->GetBoundAddr();
    if ( pLocalAddr == nullptr )
        return nullptr;

    CSteamNetworkingSocketsSTUNRequest * pRequest = new CSteamNetworkingSocketsSTUNRequest;
    {
        pRequest->m_localAddr = *pLocalAddr;
        pRequest->m_nEncoding = nEncoding | kSTUNPacketEncodingFlags_NoMappedAddress;
        netadr_t remoteNetAddr;
        ConvertSteamNetworkingIPAddrToNetAdr_t( remoteAddr, &remoteNetAddr );
        pRequest->m_pSocket = pSharedSock->AddRemoteHost( remoteNetAddr, CRecvPacketCallback( StaticPacketReceived, pRequest ) );
		if ( pRequest->m_pSocket == nullptr )
		{
			delete pRequest;
			return nullptr;
		}
    }
    return pRequest;
}

void CSteamNetworkingSocketsSTUNRequest::Cancel()
{
	if ( m_pSocket != nullptr )
	{
		m_pSocket->Close();
	}
    m_pSocket = nullptr;

    RecvSTUNPktInfo_t subInfo;
    subInfo.m_pRequest = this;
    subInfo.m_pHeader = nullptr;
    subInfo.m_nAttributes = 0;
    subInfo.m_pAttributes = nullptr;
	subInfo.m_usecNow = SteamNetworkingSockets_GetLocalTimestamp();
    m_callback( subInfo );

    delete this;
}

void CSteamNetworkingSocketsSTUNRequest::Think( SteamNetworkingMicroseconds usecNow )
{        
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingSocketsSTUNRequest::Think" );

    if ( m_nRetryCount == m_nMaxRetries )
    {   // Call the callback to notify that we've timed out.
        Cancel();
        return;
    }

    ++m_nRetryCount;
    SteamNetworkingMicroseconds retryTimeout = 500000 * ( 1 << m_nRetryCount ); // 2 ^ retryCount * 500ms    
    if ( retryTimeout > 60000000 ) // Max timeout of 60s.
        retryTimeout = 60000000;

    SetNextThinkTime( usecNow + retryTimeout );
    
    uint32 messageBuffer[ k_nSTUN_MaxPacketSize_Bytes / 4 ];
    const int nByteCount = EncodeSTUNPacket( messageBuffer, k_nSTUN_BindingRequest, m_nEncoding, m_nTransactionID, m_pSocket->GetRawSock()->m_boundAddr, (const uint8*)m_strPassword.c_str(), (uint32)m_strPassword.size(), m_vecExtraAttrs.Base(), m_vecExtraAttrs.Count() );
    if ( !m_pSocket->BSendRawPacket( messageBuffer, nByteCount ) )
    {        
		m_usecLastSentTime = 0;
        Cancel();
    }
	else
	{
		m_usecLastSentTime = usecNow;
	}
}

void CSteamNetworkingSocketsSTUNRequest::StaticPacketReceived( const RecvPktInfo_t &info, CSteamNetworkingSocketsSTUNRequest *pContext )
{
    if ( pContext != nullptr )
        pContext->OnPacketReceived( info );
}

bool CSteamNetworkingSocketsSTUNRequest::OnPacketReceived( const RecvPktInfo_t &info )
{
    STUNHeader header;  
    CUtlVector< STUNAttribute > vecAttributes;
    if ( !DecodeSTUNPacket( info.m_pPkt, info.m_cbPkt, m_nTransactionID, (const byte*)m_strPassword.c_str(), (uint32)m_strPassword.size(), &header, &vecAttributes ) )
        return kPacketNotProcessed; 

    RecvSTUNPktInfo_t subInfo;
    subInfo.m_pRequest = this;
	subInfo.m_usecNow = info.m_usecNow;
    subInfo.m_pHeader = &header;
    subInfo.m_nAttributes = vecAttributes.Count();
    subInfo.m_pAttributes = vecAttributes.Base();

	if ( m_pSocket != nullptr )
		m_pSocket->Close();
    m_pSocket = nullptr;
    m_callback( subInfo );

    delete this;
    return kPacketProcessed;
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
    m_sessionState = kICESessionState_Idle;
    m_nextKeepalive = 0;
    m_role = role;
    m_pSelectedCandidatePair = nullptr;
    m_pSelectedSocket = nullptr;
    m_vecInterfaces.reserve( 16 );
	m_nPermittedCandidateTypes = k_EICECandidate_Any;
}

CSteamNetworkingICESession::CSteamNetworkingICESession( const ICESessionConfig& cfg, CSteamNetworkingICESessionCallbacks *pCallbacks )
{
	m_nEncoding = kSTUNPacketEncodingFlags_MessageIntegrity;
	m_pCallbacks = pCallbacks;
	m_bInterfaceListStale = true;
    m_sessionState = kICESessionState_Idle;
    m_nextKeepalive = 0;
    m_role = cfg.m_eRole;
    m_pSelectedCandidatePair = nullptr;
    m_pSelectedSocket = nullptr;
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
    
	m_nPermittedCandidateTypes = cfg.m_nCandidateTypes;
	m_strLocalUsernameFragment = cfg.m_pszLocalUserFrag;
	m_strLocalPassword = cfg.m_pszLocalPwd;
}


CSteamNetworkingICESession::~CSteamNetworkingICESession()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

    m_sessionState = kICESessionState_Idle;
    for ( int i = len( m_vecPendingServerReflexiveRequests ) - 1; i >= 0; --i )
    {
        m_vecPendingServerReflexiveRequests[i]->Cancel();
    }
    for ( int i = len( m_vecPendingServerReflexiveKeepAliveRequests ) - 1; i >= 0; --i )
    {
        m_vecPendingServerReflexiveKeepAliveRequests[i]->Cancel();
    }
    
    for ( int i = len( m_vecPendingPeerRequests ) - 1; i >= 0; --i )
    {
        m_vecPendingPeerRequests[i]->Cancel();
    }

    for ( ICECandidatePair *pPair: m_vecCandidatePairs )
        delete pPair;
    m_vecCandidatePairs.clear();

	for ( CSharedSocket *pSock: m_vecSharedSockets )
		delete pSock;
	m_vecSharedSockets.clear();
}

CSteamNetworkingICESession::ICESessionState CSteamNetworkingICESession::GetSessionState()
{
    return m_sessionState;
}

SteamNetworkingIPAddr CSteamNetworkingICESession::GetSelectedDestination()
{
    if ( m_pSelectedCandidatePair == nullptr )
    {
        SteamNetworkingIPAddr result;
        result.Clear();
        return result;
    }
    return m_pSelectedCandidatePair->m_remoteCandidate.m_addr;
}

bool CSteamNetworkingICESession::GetCandidates( CUtlVector< ICECandidate > *pOutVecCandidates )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

    if ( pOutVecCandidates == nullptr )
        return false;

    pOutVecCandidates->RemoveAll();
    if ( m_bInterfaceListStale )
        return false;

    pOutVecCandidates->EnsureCapacity( len( m_vecCandidates ) );
    pOutVecCandidates->AddMultipleToTail( len( m_vecCandidates ), m_vecCandidates.data() );
    return true;
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
}

void CSteamNetworkingICESession::AddPeerCandidate( const ICECandidate& candidate, const char* pszFoundation )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

    // Do we already have a candidate for this peer? If so, just update the foundation and move on.
	bool bNeedsNewEntry = true;
    for ( ICEPeerCandidate& c : m_vecPeerCandidates )
    {
        if ( c.m_addr == candidate.m_addr )
        {
			// If the foundation is the same, don't do anything - this is redundant.
			if ( V_strcmp( c.m_sFoundation.c_str(), pszFoundation ) == 0 )
				return;

            (ICECandidate&)c = candidate;
            c.m_sFoundation = pszFoundation;
			bNeedsNewEntry = false;
            return;
        }
    }
	if ( bNeedsNewEntry )
	{
		m_vecPeerCandidates.push_back( ICEPeerCandidate( candidate, pszFoundation ) );
	}
    m_bCandidatePairsNeedUpdate = true;
    if ( m_sessionState == kICESessionState_Idle || m_sessionState == kICESessionState_GatheringCandidates )
        m_sessionState = kICESessionState_TestingPeerConnectivity;
    SetNextThinkTimeASAP();
}

void CSteamNetworkingICESession::InvalidateInterfaceList()
{
    m_bInterfaceListStale = true;
}

void CSteamNetworkingICESession::SetSelectedCandidatePair( ICECandidatePair *pPair )
{
    SpewMsg( "\n\nSelected candidate %s -> %s.\n\n", SteamNetworkingIPAddrRender( pPair->m_localCandidate.m_base ).c_str(), SteamNetworkingIPAddrRender( pPair->m_remoteCandidate.m_addr ).c_str() );
    m_pSelectedCandidatePair = pPair;
    m_pSelectedSocket = FindSharedSocketForCandidate( pPair->m_localCandidate.m_base );
    if ( m_pCallbacks )
        m_pCallbacks->OnConnectionSelected( pPair->m_localCandidate, pPair->m_remoteCandidate );
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
    m_pSelectedSocket = nullptr;
    CCrypto::GenerateRandomBlock( &m_nRoleTiebreaker, sizeof( m_nRoleTiebreaker ) );
    SetNextThinkTimeASAP();
}

void CSteamNetworkingICESession::GatherInterfaces()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::GatherInterfaces" );

    m_vecInterfaces.clear();
    CUtlVector< SteamNetworkingIPAddr > vecAddrs;
    if ( !GetLocalAddresses( &vecAddrs ) )
        return;

    uint32 uPriority = 65535;
    m_bInterfaceListStale = false;

    m_vecInterfaces.reserve( vecAddrs.Count() );
    for ( int i = 0; i < vecAddrs.Count(); ++i )
    {
        m_vecInterfaces.push_back( Interface( vecAddrs[i], uPriority ) );
        --uPriority;
    }
}

CSharedSocket* CSteamNetworkingICESession::FindSharedSocketForCandidate( const SteamNetworkingIPAddr& addr )
{
    for ( CSharedSocket *p : m_vecSharedSockets )
    {
        if ( addr == *p->GetBoundAddr() )
            return p;
    }
    return nullptr; 
}

void CSteamNetworkingICESession::OnPacketReceived( const RecvPktInfo_t &info )
{   
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::OnPacketReceived" );

    STUNHeader header;
    CUtlVector< STUNAttribute > vecAttrs;
    if ( !DecodeSTUNPacket( info.m_pPkt, info.m_cbPkt, nullptr, (const uint8*)m_strLocalPassword.c_str(), (uint32)m_strLocalPassword.size(), &header, &vecAttrs ) )
    {
        if ( m_pCallbacks != nullptr )
            m_pCallbacks->OnPacketReceived( info );
        return;
    }

    if ( header.m_nMessageType == k_nSTUN_BindingRequest )
    {
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

        // Role conflict resolution?
        SteamNetworkingIPAddr fromAddr;
        ConvertNetAddr_tToSteamNetworkingIPAddr( info.m_adrFrom, &fromAddr );
        CUtlVector< STUNAttribute > outAttrs;

        {
            const SteamNetworkingIPAddr localAddr = info.m_pSock->m_boundAddr;
            SpewMsg( "Incoming binding request from %s to %s.\n\n", SteamNetworkingIPAddrRender( fromAddr ).c_str(),  SteamNetworkingIPAddrRender( localAddr ).c_str() );
            
            ICECandidatePair *pThisPair = nullptr;
            for ( ICECandidatePair *pPair : m_vecCandidatePairs )
            {
                if ( pPair->m_remoteCandidate.m_addr == fromAddr 
                    && pPair->m_localCandidate.m_base == localAddr )
                {
                    pThisPair = pPair;
                    break;
                }
            }

            // Stale request on a pair we're not using? Ignore.
            if ( m_pSelectedCandidatePair != nullptr && m_pSelectedCandidatePair != pThisPair )
            {
                return;
            }

            if ( pThisPair == nullptr )
            {   // Find the local candidate
                ICECandidate *pLocalCandidate = nullptr;
                for ( ICECandidate &c : m_vecCandidates )
                {
                    if ( c.m_base == localAddr )
                    {
                        pLocalCandidate = &c;
                        break;
                    }
                }
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
                    ICECandidate newRemoteCandidate( kICECandidateType_PeerReflexive, fromAddr, fromAddr );
                    const STUNAttribute *pPriorityAttr = FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nSTUN_Attr_Priority );
                    if ( pPriorityAttr != nullptr )
                    {
                        newRemoteCandidate.m_nPriority = ntohl( pPriorityAttr->m_pData[0] );
                    }
                    pRemoteCandidate = push_back_get_ptr( m_vecPeerCandidates, ICEPeerCandidate( newRemoteCandidate, SteamNetworkingIPAddrRender( fromAddr ).c_str() ) );
                }
                pThisPair = new ICECandidatePair( *pLocalCandidate, *pRemoteCandidate, m_role );
                m_vecCandidatePairs.push_back( pThisPair );
            }

            if ( pThisPair != nullptr )
            {          
                if ( FindAttributeOfType( vecAttrs.Base(), vecAttrs.Count(), k_nSTUN_Attr_UseCandidate ) )
                {
                    SpewMsg( "UseCandidate was set!" );
                    if ( pThisPair->m_nState == kICECandidatePairState_Succeeded )
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
            }
            
            if ( m_strIncomingUsername.size() > 0 )
            {
                STUNAttribute attrUsername;
                attrUsername.m_nType = k_nSTUN_Attr_UserName;
                int nUsernameLength = (int)m_strIncomingUsername.size();
                attrUsername.m_nLength = nUsernameLength;
                attrUsername.m_pData = new uint32[ (nUsernameLength + 3) / 4 ];
                V_memcpy( (void*)attrUsername.m_pData, m_strIncomingUsername.c_str(), m_strIncomingUsername.size() );
                outAttrs.AddToTail( attrUsername );
            }
        }
        
        SendSTUNResponsePacket( info.m_pSock, m_nEncoding, header.m_nTransactionID, fromAddr, (const uint8*)m_strLocalPassword.c_str(), (uint32)m_strLocalPassword.size(), outAttrs.Base(), outAttrs.Count() );
    }
}

void CSteamNetworkingICESession::StaticPacketReceived( const RecvPktInfo_t &info, CSteamNetworkingICESession *pContext )
{
    if ( pContext != nullptr )
        pContext->OnPacketReceived( info );
}

void CSteamNetworkingICESession::Think( SteamNetworkingMicroseconds usecNow )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::Think" );

    SetNextThinkTime( usecNow + SteamNetworkingMicroseconds( 50000 ) ); // 50ms think rate

    if ( m_bInterfaceListStale )
    {
		if ( m_sessionState == kICESessionState_Idle )
			m_sessionState = kICESessionState_GatheringCandidates;
        GatherInterfaces();
        // We tried to update interfaces but failed. Try again later.
        if ( m_bInterfaceListStale )
            return;
        
        UpdateHostCandidates();
    }

    Think_KeepAliveOnCandidates( usecNow );

    if ( m_sessionState == kICESessionState_GatheringCandidates 
        || m_sessionState == kICESessionState_TestingPeerConnectivity )
    {
        Think_DiscoverServerReflexiveCandidates();
        if ( m_sessionState == kICESessionState_GatheringCandidates && m_vecPendingServerReflexiveRequests.empty() && m_vecPeerCandidates.empty() )
        {
            m_sessionState = kICESessionState_Idle;
            return;
        }
    }

    if ( m_sessionState == kICESessionState_TestingPeerConnectivity )
    {
        Think_TestPeerConnectivity();
        if ( !m_vecPendingPeerRequests.empty() )
            return;
        m_sessionState = kICESessionState_Idle;
    }
}

void CSteamNetworkingICESession::Think_DiscoverServerReflexiveCandidates()
{
    if ( m_vecSTUNServers.empty() )
        return;

    // Send a STUN request to check for a kICECandidateType_ServerReflexive candidate.
    // This search is O(n^2) over the number of candidates. We assume this number is a pretty small
    // integer such that basically all of m_vecCandidates ends up in L1 cache.
    // If it gets large, we'll want to manage these requests using queues or something.
    for ( const ICECandidate& c : m_vecCandidates )
    {
        if ( c.m_type != kICECandidateType_Host )
            continue;
        if ( !c.m_base.IsIPv4() )
            continue;
        // Do we have a server-reflexive candidate for this host already?
        bool bFound = false;
        if ( !bFound )
        {
            for ( const ICECandidate& c2 : m_vecCandidates )
            {
                if ( c2.m_type == kICECandidateType_ServerReflexive && c2.m_base == c.m_base )
                {
                    bFound = true;
                    break;
                }
            }
        }
        if ( !bFound )
        {   // Is there a STUN request pending?
            for ( CSteamNetworkingSocketsSTUNRequest* pReq : m_vecPendingServerReflexiveRequests )
            {
                if ( c.m_base == pReq->m_localAddr )
                {
                    bFound = true;
                    break;
                }
            }
        }
        if ( bFound )
            continue;

        CSharedSocket * const pSocket = FindSharedSocketForCandidate( c.m_base );
        // No socket for this candidate?
        if ( pSocket == nullptr )
            continue;

        CSteamNetworkingSocketsSTUNRequest *pNewRequest = CSteamNetworkingSocketsSTUNRequest::SendBindRequest( pSocket, m_vecSTUNServers[0], CRecvSTUNPktCallback( StaticSTUNRequestCallback_ServerReflexiveCandidate, this ), m_nEncoding | kSTUNPacketEncodingFlags_MappedAddress );
        if ( pNewRequest != nullptr )
        {
            m_vecPendingServerReflexiveRequests.push_back( pNewRequest );
            return;
        }
    }
}

void CSteamNetworkingICESession::UpdateHostCandidates()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::UpdateHostCandidates" );

    std_vector<ICECandidate> vecPreviousCandidates;
    std::swap( vecPreviousCandidates, m_vecCandidates );

    for ( const Interface& intf: m_vecInterfaces )
    {
        SteamNetworkingIPAddr hostCandidateAddr = intf.m_localaddr;
		hostCandidateAddr.m_port = 0;

        const uint32 nLocalPriority = intf.m_nPriority;
        bool bSawPrevCandidate = false;
		ICECandidate *pAddedCandidate = nullptr;
        for( const ICECandidate& prevCandidate : vecPreviousCandidates )
        {
            if ( prevCandidate.m_base == hostCandidateAddr )
            {
                bSawPrevCandidate = true;
                pAddedCandidate = push_back_get_ptr( m_vecCandidates, prevCandidate );
            }
        }
        if ( !bSawPrevCandidate )
        {
            CSharedSocket *pSock = new CSharedSocket;
            SteamDatagramErrMsg errMsg;
            if ( pSock->BInit( hostCandidateAddr, CRecvPacketCallback( CSteamNetworkingICESession::StaticPacketReceived, this ), errMsg ) )
            {
				if ( hostCandidateAddr.m_port == 0 )
					hostCandidateAddr.m_port = pSock->GetBoundAddr()->m_port;
                m_vecSharedSockets.push_back( pSock );
                pAddedCandidate = push_back_get_ptr( m_vecCandidates, ICECandidate( kICECandidateType_Host, hostCandidateAddr, hostCandidateAddr ) );
            }
            else
            {
                SpewError( "Could not bind to %s.  %s\n", SteamNetworkingIPAddrRender( hostCandidateAddr ).c_str(), errMsg );
				delete pSock;
                continue;
            }
        }
        pAddedCandidate->m_nPriority = pAddedCandidate->CalcPriority( nLocalPriority );
        if ( m_pCallbacks != nullptr )
            m_pCallbacks->OnLocalCandidateDiscovered( *pAddedCandidate );
    }

    // Cancel all pending STUN requests that refer to interfaces that no longer exist.
    for ( int i = len( m_vecPendingServerReflexiveRequests ) - 1; i >= 0; )
    {
        SteamNetworkingIPAddr ifAddr = m_vecPendingServerReflexiveRequests[i]->m_localAddr;
        ifAddr.m_port = 0;
        bool bFound = false;
		for ( const Interface& intf: m_vecInterfaces )
        {
            if ( intf.m_localaddr == ifAddr )
            {
                bFound = true;
                break;
            }
        }
        if ( bFound )
        {
            --i;
            continue;
        }

        m_vecPendingServerReflexiveRequests[i]->Cancel();
        erase_at( m_vecPendingServerReflexiveRequests, i );
    }
    
    // Close all shared sockets that refer to interfaces that no longer exist.
    for ( int i = len( m_vecSharedSockets ) - 1; i >= 0; )
    {
        SteamNetworkingIPAddr ifAddr = *m_vecSharedSockets[i]->GetBoundAddr();
        ifAddr.m_port = 0;
        bool bFound = false;
		for ( const Interface& intf: m_vecInterfaces )
        {
            if ( intf.m_localaddr == ifAddr )
            {
                bFound = true;
                break;
            }
        }
        if ( bFound )
        {
            --i;
            continue;
        }
        delete m_vecSharedSockets[i];
        erase_at( m_vecSharedSockets, i );
    }
}

bool CSteamNetworkingICESession::IsCandidatePermitted( const ICECandidate& localCandidate)
{
	int nCandidateType = localCandidate.CalcType();
	return ( m_nPermittedCandidateTypes & nCandidateType ) == nCandidateType;
}


void CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveCandidate( const RecvSTUNPktInfo_t &info )
{
    find_and_remove_element( m_vecPendingServerReflexiveRequests, info.m_pRequest );
    // It's possible this is a late return.
    if ( m_sessionState != kICESessionState_GatheringCandidates )
        return;

    const SteamNetworkingIPAddr localAddr = info.m_pRequest->m_localAddr;
    bool bFound = false;
    for ( const ICECandidate& c : m_vecCandidates )
    {
        if ( c.m_type == kICECandidateType_ServerReflexive && c.m_base == localAddr )
        {
            bFound = true;
            break;
        }
    }

    uint32 uLocalPriority = 0;
    for ( const Interface& i : m_vecInterfaces )
    {
        if ( i.m_localaddr == localAddr )
        {
            uLocalPriority = i.m_nPriority;
            break;
        }
    }

    // Another response for a candidate we already have? Just drop it.
    if ( bFound )
        return;

    SteamNetworkingIPAddr bindResult;        
    bindResult.Clear();
    if ( ReadAnyMappedAddress( info.m_pAttributes, info.m_nAttributes, info.m_pHeader, &bindResult ) )
    {   // Got a response... is it redundant (this happens when we get a STUN response but we're not behind a NAT)
        if ( bindResult == localAddr )
            bindResult.Clear();
        ICECandidate *pCand = push_back_get_ptr( m_vecCandidates, ICECandidate( kICECandidateType_ServerReflexive, bindResult, localAddr, info.m_pRequest->m_remoteAddr ) );
        pCand->m_nPriority = pCand->CalcPriority( uLocalPriority );
        if ( m_pCallbacks != nullptr && !bindResult.IsIPv6AllZeros() )
            m_pCallbacks->OnLocalCandidateDiscovered( *pCand );
        return;
    }
        
    // So we timed out to this STUN server
    const int nSTUNServerIdx = index_of( m_vecSTUNServers, info.m_pRequest->m_remoteAddr );
    CSharedSocket *pSharedSock = FindSharedSocketForCandidate( localAddr );
    if ( pSharedSock == nullptr || nSTUNServerIdx < 0 )
    {   // Just store an IPv6 all zeros to flag an invalid server reflexive candidate.
        bindResult.Clear();    
        ICECandidate *pCand = push_back_get_ptr( m_vecCandidates, ICECandidate( kICECandidateType_ServerReflexive, bindResult, localAddr, info.m_pRequest->m_remoteAddr ) );
        pCand->m_nPriority = 0;
        return;        
    }

    // Try the next server
    CSteamNetworkingSocketsSTUNRequest *pNewRequest = CSteamNetworkingSocketsSTUNRequest::SendBindRequest( pSharedSock, m_vecSTUNServers[nSTUNServerIdx+1], CRecvSTUNPktCallback( StaticSTUNRequestCallback_ServerReflexiveCandidate, this ), m_nEncoding );
    if ( pNewRequest != nullptr )
    {
        m_vecPendingServerReflexiveRequests.push_back( pNewRequest );
    }
}

void CSteamNetworkingICESession::StaticSTUNRequestCallback_ServerReflexiveCandidate( const RecvSTUNPktInfo_t &info, CSteamNetworkingICESession* pContext )
{
    if ( pContext != nullptr )
        pContext->STUNRequestCallback_ServerReflexiveCandidate( info );
}


void CSteamNetworkingICESession::STUNRequestCallback_ServerReflexiveKeepAlive( const RecvSTUNPktInfo_t &info )
{
    find_and_remove_element( m_vecPendingServerReflexiveKeepAliveRequests, info.m_pRequest );

    const SteamNetworkingIPAddr localAddr = info.m_pRequest->m_localAddr;
    ICECandidate *pCandidate = nullptr;
    for ( ICECandidate& c : m_vecCandidates )
    {
        if ( c.m_type == kICECandidateType_ServerReflexive && c.m_base == localAddr )
        {
            pCandidate = &c;
            break;
        }
    }

    SteamNetworkingIPAddr bindResult;        
    bindResult.Clear();
    if ( ReadAnyMappedAddress( info.m_pAttributes, info.m_nAttributes, info.m_pHeader, &bindResult ) )
    {   
        // Update the STUN info for keepalive and we're done.
        if ( !( pCandidate->m_stunServer == info.m_pRequest->m_remoteAddr ) )
            pCandidate->m_stunServer = info.m_pRequest->m_remoteAddr;
        if ( !( pCandidate->m_addr == bindResult ) )
            /*STUN server gave us a new address - what should we do here?*/
            SpewError( "Mismatching address in STUN response: got %s expected %s.", SteamNetworkingIPAddrRender( bindResult, true ).c_str(), SteamNetworkingIPAddrRender( pCandidate->m_addr, true ).c_str());      

        return;
    }

    // So we timed out to this STUN server, so try the next one if we have any.
    if ( m_vecSTUNServers.empty() )
        return;

    const int nSTUNServerIdx = std::max( 0, index_of( m_vecSTUNServers, info.m_pRequest->m_remoteAddr ) );
    const int nNextSTUNServerIdx = ( nSTUNServerIdx + 1 ) % len( m_vecSTUNServers );
    CSteamNetworkingSocketsSTUNRequest *pNewRequest = CSteamNetworkingSocketsSTUNRequest::SendBindRequest( info.m_pRequest->m_pSocket, m_vecSTUNServers[ nNextSTUNServerIdx ], CRecvSTUNPktCallback( StaticSTUNRequestCallback_ServerReflexiveKeepAlive, this ), m_nEncoding );
    if ( pNewRequest != nullptr )
    {
        m_vecPendingServerReflexiveRequests.push_back( pNewRequest );
    }
}

void CSteamNetworkingICESession::StaticSTUNRequestCallback_ServerReflexiveKeepAlive( const RecvSTUNPktInfo_t &info, CSteamNetworkingICESession* pContext )
{
    if ( pContext != nullptr )
        pContext->STUNRequestCallback_ServerReflexiveKeepAlive( info );
}

void CSteamNetworkingICESession::UpdateKeepalive( const ICECandidate& c )
{
    if ( c.m_type != kICECandidateType_ServerReflexive )
        return;
    if ( c.m_addr.IsIPv6AllZeros() )
        return;
    
    CSharedSocket * const pSocket = FindSharedSocketForCandidate( c.m_base );
    if ( pSocket == nullptr )
        return;

    bool bFoundPendingKeepalive = false;
    for ( const CSteamNetworkingSocketsSTUNRequest *pRequest : m_vecPendingServerReflexiveRequests )
    {
        if ( pRequest->m_localAddr == c.m_base )
        {
            bFoundPendingKeepalive = true;
            break;
        }
    }
    if ( bFoundPendingKeepalive )
        return;

    CSteamNetworkingSocketsSTUNRequest *pNewRequest = CSteamNetworkingSocketsSTUNRequest::SendBindRequest( pSocket, c.m_stunServer, CRecvSTUNPktCallback( StaticSTUNRequestCallback_ServerReflexiveKeepAlive, this ), m_nEncoding );
    if ( pNewRequest != nullptr )
    {
        m_vecPendingServerReflexiveKeepAliveRequests.push_back( pNewRequest );
    }
}

void CSteamNetworkingICESession::Think_KeepAliveOnCandidates( SteamNetworkingMicroseconds usecNow )
{
    if ( usecNow < m_nextKeepalive )
        return;

    m_nextKeepalive = usecNow + SteamNetworkingMicroseconds( 15 * 1000 * 1000 );

    if ( m_pSelectedCandidatePair != nullptr )
    {
        UpdateKeepalive( m_pSelectedCandidatePair->m_localCandidate );
    }
    else
    {    
        for ( const ICECandidate& c : m_vecCandidates )
        {
            UpdateKeepalive( c ) ;
        }
    }
}

void CSteamNetworkingICESession::Think_TestPeerConnectivity()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingICESession::Think_TestPeerConnectivity" );

    if ( m_bCandidatePairsNeedUpdate )
    {
        m_bCandidatePairsNeedUpdate = false;

        // For every peer, for every local candidate, make sure the pair is present in the pairs list...
        for ( ICECandidate &localCandidate : m_vecCandidates )
        {
			if ( !IsCandidatePermitted( localCandidate ) )
				continue;

            for ( ICEPeerCandidate &remoteCandidate : m_vecPeerCandidates )
            {
                bool bFound = false;
                for ( ICECandidatePair *pPair : m_vecCandidatePairs )
                {
                    if ( pPair->m_localCandidate.m_addr == localCandidate.m_addr && pPair->m_remoteCandidate.m_addr == remoteCandidate.m_addr )
                    {
                        bFound = true;
                        break;
                    }
                }
                if ( bFound )
                    continue;
                if ( localCandidate.m_base.IsIPv4() != remoteCandidate.m_addr.IsIPv4() )
                    continue;

                if ( !bFound )
                {
                    ICECandidatePair *pNewCandidatePair = new ICECandidatePair( localCandidate, remoteCandidate, m_role );
                    m_vecCandidatePairs.push_back( pNewCandidatePair );
                }
            }
        }

        std::sort( m_vecCandidatePairs.begin(), m_vecCandidatePairs.end(), []( const ICECandidatePair *pA, const ICECandidatePair *pB ) { return pA->m_nPriority > pB->m_nPriority; } );
    }

    ICECandidatePair *pPairToCheck = nullptr;

    if ( !m_vecTriggeredCheckQueue.empty() )
    {
		int i = len( m_vecTriggeredCheckQueue ) - 1;
        pPairToCheck = m_vecTriggeredCheckQueue[ i ];
        erase_at( m_vecTriggeredCheckQueue, i );
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
        pPairToCheck->m_nState = kICECandidatePairState_InProgress;
        CSharedSocket * const pSocket = FindSharedSocketForCandidate( pPairToCheck->m_localCandidate.m_base );
        // No socket for this candidate?
        if ( pSocket == nullptr )
        {
            pPairToCheck->m_nState = kICECandidatePairState_Failed;
            return;
        }

        pPairToCheck->m_pPeerRequest = CSteamNetworkingSocketsSTUNRequest::CreatePeerConnectivityCheckRequest( pSocket, pPairToCheck->m_remoteCandidate.m_addr, CRecvSTUNPktCallback( StaticSTUNRequestCallback_PeerConnectivityCheck, this ), m_nEncoding );
        if ( pPairToCheck->m_pPeerRequest == nullptr )
        {
            pPairToCheck->m_nState = kICECandidatePairState_Failed;
            return;
        }

        if ( m_strOutgoingUsername.size() > 0 )
        {
            STUNAttribute attrUsername;
            attrUsername.m_nType = k_nSTUN_Attr_UserName;
            int nUsernameLength = (int)m_strOutgoingUsername.size();
            attrUsername.m_nLength = nUsernameLength;
            attrUsername.m_pData = new uint32[ (nUsernameLength + 3) / 4 ];
            V_memcpy( (void*)attrUsername.m_pData, m_strOutgoingUsername.c_str(), m_strOutgoingUsername.size() );
            pPairToCheck->m_pPeerRequest->m_vecExtraAttrs.AddToTail( attrUsername );
        }

        {
            STUNAttribute attrPriority;
            attrPriority.m_nType = k_nSTUN_Attr_Priority;
            attrPriority.m_nLength = 4;
            attrPriority.m_pData = new uint32[1];
            uint32 uPriority = pPairToCheck->m_localCandidate.m_nPriority;
            // Adjust priority to be peer-reflexity type preference.
            uPriority = ( uPriority & 0xFFFFFF ) | ( 110 << 24ul );
            *const_cast<uint32*>(attrPriority.m_pData) = htonl( uPriority );
            pPairToCheck->m_pPeerRequest->m_vecExtraAttrs.AddToTail( attrPriority );
        }

        if ( m_role == k_EICERole_Controlling )
        {
            STUNAttribute attrControlling;
            attrControlling.m_nType = k_nSTUN_Attr_ICEControlling;
            attrControlling.m_nLength = 8;
            uint32* pBuf = new uint32[2];
            attrControlling.m_pData = pBuf;
            *(uint64*)pBuf = m_nRoleTiebreaker;
            pBuf[0] = htonl( pBuf[0] );
            pBuf[1] = htonl( pBuf[1] );
            pPairToCheck->m_pPeerRequest->m_vecExtraAttrs.AddToTail( attrControlling );
            
			if ( pPairToCheck->m_bNominated )
			{
				STUNAttribute attrUseCandidate;
				attrUseCandidate.m_nType = k_nSTUN_Attr_UseCandidate;
				attrUseCandidate.m_nLength = 0;
				attrUseCandidate.m_pData = nullptr;
				pPairToCheck->m_pPeerRequest->m_vecExtraAttrs.AddToTail( attrUseCandidate );
			}
        }
        else if ( m_role == k_EICERole_Controlled )
        {
            STUNAttribute attrControlled;
            attrControlled.m_nType = k_nSTUN_Attr_ICEControlled;
            attrControlled.m_nLength = 8;
            uint32* pBuf = new uint32[2];
            attrControlled.m_pData = pBuf;
            *(uint64*)pBuf = m_nRoleTiebreaker;
            pBuf[0] = htonl( pBuf[0] );
            pBuf[1] = htonl( pBuf[1] );
            pPairToCheck->m_pPeerRequest->m_vecExtraAttrs.AddToTail( attrControlled );
        }

        pPairToCheck->m_pPeerRequest->m_strPassword = m_strRemotePassword;
        pPairToCheck->m_pPeerRequest->Send( pPairToCheck->m_remoteCandidate.m_addr, CRecvSTUNPktCallback( StaticSTUNRequestCallback_PeerConnectivityCheck, this ) );
        m_vecPendingPeerRequests.push_back( pPairToCheck->m_pPeerRequest );
    }        
}
        
void CSteamNetworkingICESession::STUNRequestCallback_PeerConnectivityCheck( const RecvSTUNPktInfo_t &info )
{
    find_and_remove_element( m_vecPendingPeerRequests, info.m_pRequest );
    ICECandidatePair *pPair = nullptr;
    for ( ICECandidatePair *pCandidatePair : m_vecCandidatePairs )
    {
        if ( pCandidatePair->m_nState != kICECandidatePairState_InProgress )
            continue;
        if ( !( pCandidatePair->m_localCandidate.m_base == info.m_pRequest->m_localAddr ) )
            continue;
        if ( !( pCandidatePair->m_remoteCandidate.m_addr == info.m_pRequest->m_remoteAddr ) )
            continue;
        pPair = pCandidatePair;
        break;
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
    pPair->m_pPeerRequest = nullptr;   
    pPair->m_nState = kICECandidatePairState_Succeeded;
    if ( pPair->m_bNominated )
    {
        SetSelectedCandidatePair( pPair );
    }
	else if ( m_role == k_EICERole_Controlling )
    {
		bool bAlreadyHaveANomination = false;
        for ( ICECandidatePair *pOtherPair : m_vecCandidatePairs )
        {
            if ( pOtherPair->m_bNominated == true 
                && ( pOtherPair->m_nState == kICECandidatePairState_InProgress || pOtherPair->m_nState == kICECandidatePairState_Waiting ) )
                bAlreadyHaveANomination = true;
        }
		if ( !bAlreadyHaveANomination )
		{
			pPair->m_bNominated = true;
			m_vecTriggeredCheckQueue.push_back( pPair );
		}
        
    }
}

void CSteamNetworkingICESession::StaticSTUNRequestCallback_PeerConnectivityCheck( const RecvSTUNPktInfo_t &info, CSteamNetworkingICESession* pContext )
{
    if ( pContext != nullptr )
	{
        pContext->STUNRequestCallback_PeerConnectivityCheck( info );
	}
}


/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingICESession::ICECandidate
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingICESession::ICECandidate::ICECandidate()
{
    m_type = kICECandidateType_None;
    m_addr.Clear();
    m_base.Clear();
    m_stunServer.Clear();
    m_nPriority = 0;
}

CSteamNetworkingICESession::ICECandidate::ICECandidate( ICECandidateType t, const SteamNetworkingIPAddr& addr, const SteamNetworkingIPAddr& base )
{
    m_type = t;
    m_addr = addr;
    m_base = base;
    m_stunServer.Clear();
    m_nPriority = 0;
}

CSteamNetworkingICESession::ICECandidate::ICECandidate( ICECandidateType t, const SteamNetworkingIPAddr& addr, const SteamNetworkingIPAddr& base, const SteamNetworkingIPAddr& stunServer ) 
{
    m_type = t;
    m_addr = addr;
    m_base = base;
    m_stunServer = stunServer;
    m_nPriority = 0;
}

uint32 CSteamNetworkingICESession::ICECandidate::CalcPriority( uint32 nLocalPreference )
{
    /*priority = (2^24)*(type preference) +
              (2^8)*(local preference) +
              (2^0)*(256 - component ID) */

    if ( m_type == kICECandidateType_None )
        return 0;
    if ( m_addr.IsIPv6AllZeros() )
        return 0;
    
    uint32 nTypePreference = 0;
    /*  The RECOMMENDED values for type preferences are 126 for host
        candidates, 110 for peer-reflexive candidates, 100 for server-
        reflexive candidates, and 0 for relayed candidates. */
    switch ( m_type )
    {
        case kICECandidateType_Host: nTypePreference = 126; break;
        case kICECandidateType_ServerReflexive: nTypePreference = 100; break;
        case kICECandidateType_PeerReflexive: nTypePreference = 110; break;
        case kICECandidateType_None: default: nTypePreference = 0; break;
    }

    uint32 nComponentID = 1;
    return (( nTypePreference & 0xFF ) << 24 ) + (( nLocalPreference & 0xFFFF ) << 8 ) + ( 256 - ( nComponentID & 0xFF ) );
}

// Compute a candidate-attribute from https://datatracker.ietf.org/doc/html/rfc5245#section-15.1
// Ex: candidate:2442523459 0 udp 2122262784 2602:801:f001:1034:5078:221c:76b:a3d6 63368 typ host generation 0 ufrag WLM82 network-id 2
void CSteamNetworkingICESession::ICECandidate::CalcCandidateAttribute( char *pszBuffer, size_t nBufferSize ) const
{
    /* <foundation>:  is composed of 1 to 32 <ice-char>s.  It is an
      identifier that is equivalent for two candidates that are of the
      same type, share the same base, and come from the same STUN
      server.*/
    uint32 nFoundation = 0;
    {
        uint16 uCounter = 0;
        for( int i = 0; i < 16; ++i )
        {
            uCounter += m_base.m_ipv6[i];
            uCounter += m_stunServer.m_ipv6[i];
        }    
        nFoundation = ( m_base.m_port + m_stunServer.m_port ) + ( uCounter << 15 ) + (int)m_type;
    }
    char connectionAddr[ SteamNetworkingIPAddr::k_cchMaxString];
    m_addr.ToString( connectionAddr, V_ARRAYSIZE( connectionAddr ), false );
    const char *pszType = "";
    switch ( m_type )
    {
        case kICECandidateType_Host: pszType = "host"; break;
        case kICECandidateType_ServerReflexive: pszType = "srflx"; break;
        //case  kICECandidateType_Relayed: pszType = "relay"; break;
        case  kICECandidateType_PeerReflexive: pszType = "prflx"; break;
        default: break;
    }
    /*If relayed, add these too: 
    rel-addr              = "raddr" SP connection-address
    rel-port              = "rport" SP port*/
    V_snprintf( pszBuffer, nBufferSize, "candidate:%u 0 udp %u %s %d typ %s", nFoundation, m_nPriority, connectionAddr, m_addr.m_port, pszType );
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

EICECandidateType CSteamNetworkingICESession::ICECandidate::CalcType() const
{
	switch ( m_type )
	{
	case kICECandidateType_Host:
		if ( m_base.IsIPv4() )
		{
			if ( IsPrivateIPv4( m_base.m_ipv4.m_ip ) )
				return k_EICECandidate_IPv4_HostPrivate;
			else
				return k_EICECandidate_IPv4_HostPublic;
		}
		else
		{
			return k_EICECandidate_IPv6_HostPublic;
		}
		break;
	case kICECandidateType_ServerReflexive:
	case kICECandidateType_PeerReflexive:
		if ( m_base.IsIPv4() )
			return k_EICECandidate_IPv4_Reflexive;
		else
			return k_EICECandidate_IPv6_Reflexive;
		break;

	/* case kICECandidateType_Relayed:
		if ( localCandidate.m_base.IsIPv4() )
			nCandidateType = k_EICECandidate_IPv4_Relay;
		else
			nCandidateType = k_EICECandidate_IPv6_Relay;
		break;
	*/
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
CSteamNetworkingICESession::ICECandidatePair::ICECandidatePair( const ICECandidate& localCandidate, const ICEPeerCandidate& remoteCandidate, EICERole role )
    : m_localCandidate( localCandidate ),
      m_remoteCandidate( remoteCandidate ),
      m_nState( kICECandidatePairState_Frozen ),
      m_bNominated( false )
{
    const uint64 D = ( role == k_EICERole_Controlling ) ? localCandidate.m_nPriority : remoteCandidate.m_nPriority;
    const uint64 G = ( role == k_EICERole_Controlling ) ? remoteCandidate.m_nPriority : localCandidate.m_nPriority;
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
    return ( m_pICESession->GetSelectedSocket() != nullptr );
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
        SpewMsg( "Got remote candidate \'%s\'\n", s.c_str() );
        RFC5245CandidateAttr attr;
        if ( ParseRFC5245CandidateAttribute( s.c_str(), &attr ) )
        {
            SteamNetworkingIPAddr candidateAddr;
            if ( !candidateAddr.ParseString( attr.sAddress.c_str() ) )
            {
                SpewMsg( "Failed to parse address \'%s\' as an IP address.", attr.sAddress.c_str() );
                return;
            }            
            candidateAddr.m_port = attr.nPort;

            SpewMsg( "Got a rendezvous candidate at \"%s\"\n", SteamNetworkingIPAddrRender( candidateAddr ).c_str() );
            CSteamNetworkingICESession::ICECandidate newCandidate( attr.nType, candidateAddr, candidateAddr );            
            newCandidate.m_nPriority = attr.nPriority;
            m_pICESession->AddPeerCandidate( newCandidate, attr.sFoundation.c_str() );
        }
    }
}

bool CConnectionTransportP2PICE_Valve::SendPacket( const void *pkt, int cbPkt )
{
    CSharedSocket *pSock = m_pICESession->GetSelectedSocket();
    if ( pSock == nullptr )
        return false;
    netadr_t destAdr;
    ConvertSteamNetworkingIPAddrToNetAdr_t( m_pICESession->GetSelectedDestination(), &destAdr );
    return pSock->BSendRawPacket( pkt, cbPkt, destAdr );
}
 
bool CConnectionTransportP2PICE_Valve::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
    CSharedSocket *pSock = m_pICESession->GetSelectedSocket();
    if ( pSock == nullptr )
        return false;

    return pSock->BSendRawPacketGather( nChunks, pChunks, m_pICESession->GetSelectedDestination() );
}

void CConnectionTransportP2PICE_Valve::OnLocalCandidateDiscovered( const CSteamNetworkingICESession::ICECandidate& candidate )
{
    char chBuffer[512];
    candidate.CalcCandidateAttribute( chBuffer, V_ARRAYSIZE( chBuffer ) - 1 );

    ConnectionScopeLock lock( Connection(), "OnLocalCandidateDiscovered");

    CMsgICECandidate c;
    c.set_candidate( chBuffer );
	LocalCandidateGathered( candidate.CalcType(), std::move( c ) );
}

void CConnectionTransportP2PICE_Valve::OnConnectionSelected( const CSteamNetworkingICESession::ICECandidate& localCandidate, const CSteamNetworkingICESession::ICECandidate& remoteCandidate )
{
    ConnectionScopeLock lock( Connection(), "CConnectionTransportP2PICE_Valve::OnConnectionSelected");

    m_currentRouteRemoteAddress = remoteCandidate.m_addr;
    if ( localCandidate.m_type == CSteamNetworkingICESession::kICECandidateType_Host && remoteCandidate.m_type == CSteamNetworkingICESession::kICECandidateType_Host ) 																						
    {
        m_eCurrentRouteKind = k_ESteamNetTransport_UDPProbablyLocal;
    }
    else
    {
        m_eCurrentRouteKind = k_ESteamNetTransport_UDP;
    }
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
