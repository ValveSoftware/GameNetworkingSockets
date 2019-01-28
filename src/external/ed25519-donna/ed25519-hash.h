#if defined(ED25519_REFHASH)
#include "ed25519-hash-ref.h"
#elif defined(ED25519_CUSTOMHASH)
#include "ed25519-hash-custom.h"
#else
#include "ed25519-hash-openssl.h"
#endif
