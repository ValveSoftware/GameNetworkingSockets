//========= Copyright Valve LLC, All rights reserved. =========================

#ifndef CRYPTO_CONSTANTS_H
#define CRYPTO_CONSTANTS_H
#pragma once

#include <steam/steamtypes.h>

const int k_nSymmetricBlockSize = 16;					// AES block size (128 bits)
const int k_nSymmetricIVSize = 12;						// length of the IV (must be 12 for BCryptEncrypt/BCryptDecrypt, but OpenSSL is more flexible)
const int k_nSymmetricGCMTagSize = 16;					// length in GCM tag (must be 16 for BCryptEncrypt/BCryptDecrypt, but OpenSSL is more flexible)
const int k_nSymmetricKeyLen = 32;						// length in bytes of keys used for symmetric encryption
const int k_nSymmetricKeyLen128 = 16;					// 128 bits, in bytes
const int k_nSymmetricKeyLen256 = 32;					// 256 bits, in bytes

const int k_nRSAKeyLenMax = 1024;						// max length in bytes of keys used for RSA encryption (includes DER encoding)
const int k_nRSAKeyLenMaxEncoded = k_nRSAKeyLenMax*2;	// max length in bytes of hex-encoded key (hex encoding exactly doubles size)
const int k_nRSAKeyBits = 1024;							// length in bits of keys used for RSA encryption
const int k_cubRSAEncryptedBlockSize	= 128;
const int k_cubRSAPlaintextBlockSize	= 86 + 1;		// assume plaintext is text, so add a byte for the trailing \0
const uint32 k_cubRSASignature = k_cubRSAEncryptedBlockSize;

const uint32 k_cubRSA2048Signature = 256;				// Signature length when using a 2048-bit RSA key

const int k_nAESKeyLenMax = k_nSymmetricKeyLen;
const int k_nAESKeyLenMaxEncoded = k_nAESKeyLenMax*2; 
const int k_nAESKeyBits = k_nSymmetricKeyLen*8;	// normal size of AES keys
const int k_cchMaxPassphrase = 128;				// maximum size of a license passphrase

const int k_nSHAHashStringLen = 40;				// length of SHA1 hash when stored as a hex string

const unsigned int k_cubMD5Hash = 16;
typedef	unsigned char MD5Digest_t[k_cubMD5Hash];

const unsigned int k_cubSHA256Hash = 32;
typedef	unsigned char SHA256Digest_t[ k_cubSHA256Hash ];

#endif // CRYPTO_CONSTANTS_H
