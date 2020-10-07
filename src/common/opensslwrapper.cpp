//====== Copyright 1996-2010, Valve Corporation, All rights reserved. =======
//
// Purpose: Code for dealing with OpenSSL library
//
//=============================================================================

#include <tier0/dbg.h>
#include "opensslwrapper.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include "crypto.h"

#include <mutex>
#include <thread>

// Statics - all automatically zero-init
int COpenSSLWrapper::m_nInstances;
static std::recursive_mutex *s_pMutexArray;
int COpenSSLWrapper::s_nContextDataIndex;
int COpenSSLWrapper::s_nConnectionDataIndex;

#ifdef _DEBUG
int COpenSSLWrapper::m_nBytesLeaked = 0;
#endif

// Locking structure for OpenSSL usage
struct CRYPTO_dynlock_value
{
	//CThreadMutex m_Mutex;
	std::recursive_mutex m_Mutex;
};


#ifdef _WIN32
//-----------------------------------------------------------------------------
// Purpose: define a thin wrapper around RtlGenRandom (Windows XP and above)
// that OpenSSL can use instead of its own md_rand system, which seriously sucks
// on win32 as it gathers "entropy" from a toolhelp32 snapshot and GDI functions
// while keeping the entire random generator state in process memory. better to
// let the OS hide the PRNG state from our process and just give us the result.
// (RtlGenRandom is also faster, at less than 1 microsecond per 16 byte output.)
//-----------------------------------------------------------------------------
static int RAND_Win32CryptoGenRandom_bytes( unsigned char *buf, int num ) {
	CCrypto::GenerateRandomBlock( buf, num );
	return 1;
}
static int RAND_Win32CryptoGenRandom_status() { return 1; }
static const RAND_METHOD RAND_Win32CryptoGenRandom = 
{
	NULL,								// seed entropy
	RAND_Win32CryptoGenRandom_bytes,	// generate random
	NULL,								// cleanup
	NULL,								// add entropy
	RAND_Win32CryptoGenRandom_bytes,	// generate pseudo-random
	RAND_Win32CryptoGenRandom_status	// status
};
#endif

#ifdef ANDROID
// This isn't in the headers, but is implemented for Android 2.4 and newer
extern "C" int pthread_atfork(void (*prepare)(void), void (*parent)(void), void(*child)(void));
#endif

//-----------------------------------------------------------------------------
// Purpose: Initialize OpenSSL, may call multiple times and will ref count, 
// call COpenSSLWrapper::Shutdown() a matching number of times.
//-----------------------------------------------------------------------------
void COpenSSLWrapper::Initialize()
{
	int iStatus;

	// If this is the first instance then we need to do some one time initialization of the OpenSSL library
	if ( m_nInstances++ == 0 )
	{
		s_pMutexArray = new std::recursive_mutex[CRYPTO_num_locks()];
		CRYPTO_set_locking_callback( COpenSSLWrapper::OpenSSLLockingCallback );
		CRYPTO_set_id_callback( COpenSSLWrapper::OpenSSLThreadIDCallback );

		CRYPTO_set_dynlock_create_callback( COpenSSLWrapper::OpenSSLDynLockCreateCallback );
		CRYPTO_set_dynlock_destroy_callback( COpenSSLWrapper::OpenSSLDynLockDestroyCallback );
		CRYPTO_set_dynlock_lock_callback( COpenSSLWrapper::OpenSSLDynLockLockCallback );

#ifdef _WIN32
		RAND_set_rand_method( &RAND_Win32CryptoGenRandom );
#endif

#ifdef OSX
		pthread_atfork( NULL, NULL, []{ arc4random_stir(); RAND_poll(); } );
#elif defined(POSIX)
		pthread_atfork( NULL, NULL, []{ RAND_poll(); } );
#endif

		iStatus = RAND_status();
		AssertMsg( iStatus == 1, "OpenSSL random number system reports not enough entropy" );
	}
}


//-----------------------------------------------------------------------------
// Purpose: Shutdown OpenSSL, number of calls must match those to COpenSSLWrapper::Initialize()
//-----------------------------------------------------------------------------
void COpenSSLWrapper::Shutdown()
{
	// If this is the last instance, then we can do some one time cleanup of the library
	if ( m_nInstances-- == 1 )
	{
//SDR_PUBLIC		EVP_cleanup();
//SDR_PUBLIC
//SDR_PUBLIC		/* Don't call ERR_free_strings here; ERR_load_*_strings only
//SDR_PUBLIC		 * actually load the error strings once per process due to static
//SDR_PUBLIC		 * variable abuse in OpenSSL. */
//SDR_PUBLIC		ERR_free_strings();
//SDR_PUBLIC		ERR_remove_state(0);
		CRYPTO_cleanup_all_ex_data();

		CRYPTO_set_locking_callback( NULL );
		CRYPTO_set_id_callback( NULL );

		CRYPTO_set_dynlock_create_callback( NULL );
		CRYPTO_set_dynlock_destroy_callback( NULL );
		CRYPTO_set_dynlock_lock_callback( NULL );

		delete[] s_pMutexArray;
		s_pMutexArray = NULL;
	}
}


//-----------------------------------------------------------------------------
// Purpose: OpenSSL callback func needed for threading support
//-----------------------------------------------------------------------------
void COpenSSLWrapper::OpenSSLLockingCallback( int mode, int type, const char *file, int line )
{
	// We are shutting down if this is NULL, won't need locking as we only have the main thread then right?
	if ( s_pMutexArray == NULL )
		return;

	if ( mode & CRYPTO_LOCK )
	{
		s_pMutexArray[type].lock();
	}
	else
	{
		s_pMutexArray[type].unlock();
	}
}


//-----------------------------------------------------------------------------
// Purpose: OpenSSL callback func needed for threading support
//-----------------------------------------------------------------------------
unsigned long COpenSSLWrapper::OpenSSLThreadIDCallback( void )
{
	std::hash<std::thread::id> hash;
	return (unsigned long)hash( std::this_thread::get_id() );
}


//-----------------------------------------------------------------------------
// Purpose: OpenSSL callback func needed for threading support
//-----------------------------------------------------------------------------
CRYPTO_dynlock_value* COpenSSLWrapper::OpenSSLDynLockCreateCallback( const char* file, int line )
{
	return new CRYPTO_dynlock_value;
}


//-----------------------------------------------------------------------------
// Purpose: OpenSSL callback func needed for threading support
//-----------------------------------------------------------------------------
void COpenSSLWrapper::OpenSSLDynLockDestroyCallback( CRYPTO_dynlock_value * l, const char *file, int line )
{
	delete l;
}


//-----------------------------------------------------------------------------
// Purpose: OpenSSL callback func needed for threading support
//-----------------------------------------------------------------------------
void COpenSSLWrapper::OpenSSLDynLockLockCallback( int mode, CRYPTO_dynlock_value *l, const char* file, int line )
{
	if ( mode & CRYPTO_LOCK )
	{
		l->m_Mutex.lock();
	}
	else
	{
		l->m_Mutex.unlock();
	}
}


#ifdef _DEBUG
//-----------------------------------------------------------------------------
// Purpose: OpenSSL callback func, used to print/track memory leaks in OpenSSL
//-----------------------------------------------------------------------------
void *COpenSSLWrapper::OpenSSLMemLeakCallback( unsigned long order, const char *file, int line, int num_bytes, void * addr )
{
	//Plat_OutputDebugString( CFmtStr1024( "OpenSSL Leak: %16u %52s:%8d %16d 0x%8X\n", order, file, line, num_bytes, addr ).Access() );
	m_nBytesLeaked += num_bytes;
	return addr;
}
#endif



#ifdef DBGFLAG_VALIDATE
//-----------------------------------------------------------------------------
// Purpose: validates memory structures
//-----------------------------------------------------------------------------
void COpenSSLWrapper::ValidateStatics( CValidator &validator, const char *pchName )
{
	if ( COpenSSLWrapper::s_pMutexArray && !validator.IsClaimed( COpenSSLWrapper::s_pMutexArray ) )
	{
		validator.ClaimArrayMemory( COpenSSLWrapper::s_pMutexArray );
		for( int i=0; i < CRYPTO_num_locks(); ++i )
		{
			validator.ClaimMemory( s_pMutexArray[i] );
		}
	}
}
#endif

