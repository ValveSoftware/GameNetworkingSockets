//====== Copyright 1996-2010, Valve Corporation, All rights reserved. =======
//
// Purpose: Code for dealing with OpenSSL library
//
//=============================================================================

#ifndef OPENSSLWRAPPER_H
#define OPENSSLWRAPPER_H
#ifdef _WIN32
#pragma once
#endif

// Locking structure for OpenSSL usage
struct CRYPTO_dynlock_value;


//-----------------------------------------------------------------------------
// Purpose: Wrapper for OpenSSL
//-----------------------------------------------------------------------------
class COpenSSLWrapper
{
public:
	static void Initialize();
	static void Shutdown();
	static bool BIsOpenSSLInitialized() { return m_nInstances > 0; }
	static int GetContextDataIndex() { return s_nContextDataIndex; }
	static int GetConnectionDataIndex() { return s_nConnectionDataIndex; }


	// OpenSSL callback functions for threading
	static void OpenSSLLockingCallback( int mode, int type, const char *file, int line );
	static unsigned long OpenSSLThreadIDCallback( void );
	static CRYPTO_dynlock_value* OpenSSLDynLockCreateCallback( const char* file, int line );
	static void OpenSSLDynLockDestroyCallback( CRYPTO_dynlock_value * l, const char *file, int line );
	static void OpenSSLDynLockLockCallback( int mode, CRYPTO_dynlock_value *l, const char* file, int line );

#ifdef _DEBUG
	// In debug we track OpenSSL memory usage via it's internal mechanism for this
	static void *OpenSSLMemLeakCallback( unsigned long order, const char *file, int line, int num_bytes, void * addr );
	static int m_nBytesLeaked;
#endif

#ifdef DBGFLAG_VALIDATE
	static void ValidateStatics( CValidator &validator, const char *pchName );
#endif


private:
	// Some statics to track how many context objects exist and to perform one time OpenSSL library initialization
	// when needed.
	static int m_nInstances;
	// VOID to avoid include on engine.h, as it can lead to some conflicts in places we need to include this header
	static void *s_pAESNIEngine;
	static int s_nContextDataIndex;
	static int s_nConnectionDataIndex;

};


#endif // OPENSSLWRAPPER_H

