/* AES-GCM implmented using Windows Crypto API: Next Generation (CNG) functions */
#cmakedefine GNS_CRYPTO_AES_BCRYPT

/* AES-GCM implemented using OpenSSL */
#cmakedefine GNS_CRYPTO_AES_OPENSSL

/* ed25519/curve25519 using reference C implementation */
#cmakedefine GNS_CRYPTO_25519_REF

/* ed25519/curve25519 using OpenSSL implementation */
#cmakedefine GNS_CRYPTO_25519_OPENSSL