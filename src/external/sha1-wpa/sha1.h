/*
 * SHA1 hash implementation and interface functions
 * Copyright (c) 2003-2009, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef SHA1_H
#define SHA1_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define os_memcpy	memcpy
#define os_memset	memset

#define SHA1_MAC_LEN 20

typedef uint8_t u8;
typedef uint32_t u32;


int hmac_sha1_vector(const u8 *key, size_t key_len, size_t num_elem,
		     const u8 *addr[], const size_t *len, u8 *mac);
int hmac_sha1(const u8 *key, size_t key_len, const u8 *data, size_t data_len,
	       u8 *mac);

/**
 * sha1_vector - SHA-1 hash for data vector
 * @num_elem: Number of elements in the data vector
 * @addr: Pointers to the data areas
 * @len: Lengths of the data blocks
 * @mac: Buffer for the hash
 * Returns: 0 on success, -1 on failure
 */
int sha1_vector(size_t num_elem, const u8 *addr[], const size_t *len,
		u8 *mac);

#endif /* SHA1_H */
