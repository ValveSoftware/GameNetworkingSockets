// Microbenchmarks for SteamNetworkingMessage_t allocation and the
// send/receive path of a pipe (CreateSocketPair without network loopback),
// where the message objects themselves are the main per-message cost.
//
// Usage: bench_messages [iterations]

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static double NsPerOp( Clock::time_point start, Clock::time_point end, long long ops )
{
	return std::chrono::duration<double, std::nano>( end - start ).count() / (double)ops;
}

// AllocateMessage + Release in a tight loop on one thread
static double BenchAllocRelease( long long nIters, int cbPayload )
{
	ISteamNetworkingUtils *pUtils = SteamNetworkingUtils();
	auto start = Clock::now();
	for ( long long i = 0 ; i < nIters ; ++i )
	{
		SteamNetworkingMessage_t *pMsg = pUtils->AllocateMessage( cbPayload );
		pMsg->Release();
	}
	return NsPerOp( start, Clock::now(), nIters );
}

// Allocate a burst of messages, then release them all (more realistic
// working set than alloc/free of a single object)
static double BenchAllocReleaseBurst( long long nIters, int cbPayload, int nBurst )
{
	ISteamNetworkingUtils *pUtils = SteamNetworkingUtils();
	std::vector<SteamNetworkingMessage_t *> vec( nBurst );
	long long nRounds = nIters / nBurst;
	auto start = Clock::now();
	for ( long long r = 0 ; r < nRounds ; ++r )
	{
		for ( int i = 0 ; i < nBurst ; ++i )
			vec[i] = pUtils->AllocateMessage( cbPayload );
		for ( int i = 0 ; i < nBurst ; ++i )
			vec[i]->Release();
	}
	return NsPerOp( start, Clock::now(), nRounds * nBurst );
}

// Send batches of small reliable messages over a pipe and receive them on the
// same thread.
static double BenchPipeSameThread( long long nIters, int cbPayload, int nBatch )
{
	HSteamNetConnection hA, hB;
	bool ok = SteamNetworkingSockets()->CreateSocketPair( &hA, &hB, false, nullptr, nullptr );
	assert( ok ); (void)ok;
	ISteamNetworkingUtils *pUtils = SteamNetworkingUtils();
	std::vector<SteamNetworkingMessage_t *> vecSend( nBatch ), vecRecv( nBatch );
	long long nRounds = nIters / nBatch;
	auto start = Clock::now();
	for ( long long r = 0 ; r < nRounds ; ++r )
	{
		for ( int i = 0 ; i < nBatch ; ++i )
		{
			SteamNetworkingMessage_t *pMsg = pUtils->AllocateMessage( cbPayload );
			pMsg->m_conn = hA;
			pMsg->m_nFlags = k_nSteamNetworkingSend_Reliable;
			vecSend[i] = pMsg;
		}
		SteamNetworkingSockets()->SendMessages( nBatch, vecSend.data(), nullptr, true );
		int nGot = 0;
		while ( nGot < nBatch )
		{
			int n = SteamNetworkingSockets()->ReceiveMessagesOnConnection( hB, vecRecv.data(), nBatch );
			for ( int i = 0 ; i < n ; ++i )
				vecRecv[i]->Release();
			nGot += n;
		}
	}
	double ns = NsPerOp( start, Clock::now(), nRounds * nBatch );
	SteamNetworkingSockets()->CloseConnection( hA, 0, nullptr, false );
	SteamNetworkingSockets()->CloseConnection( hB, 0, nullptr, false );
	return ns;
}

// Producer thread allocates and sends; consumer thread receives and releases.
// Messages are allocated on one thread and freed on another.
static double BenchPipeTwoThreads( long long nIters, int cbPayload, int nBatch )
{
	HSteamNetConnection hA, hB;
	bool ok = SteamNetworkingSockets()->CreateSocketPair( &hA, &hB, false, nullptr, nullptr );
	assert( ok ); (void)ok;
	long long nRounds = nIters / nBatch;
	long long nTotal = nRounds * nBatch;
	std::atomic<bool> bGo{ false };
	auto start = Clock::now();
	std::thread consumer( [&]() {
		std::vector<SteamNetworkingMessage_t *> vecRecv( nBatch );
		long long nGot = 0;
		while ( !bGo.load() ) {}
		while ( nGot < nTotal )
		{
			int n = SteamNetworkingSockets()->ReceiveMessagesOnConnection( hB, vecRecv.data(), nBatch );
			for ( int i = 0 ; i < n ; ++i )
				vecRecv[i]->Release();
			nGot += n;
		}
	} );
	{
		ISteamNetworkingUtils *pUtils = SteamNetworkingUtils();
		std::vector<SteamNetworkingMessage_t *> vecSend( nBatch );
		start = Clock::now();
		bGo = true;
		for ( long long r = 0 ; r < nRounds ; ++r )
		{
			for ( int i = 0 ; i < nBatch ; ++i )
			{
				SteamNetworkingMessage_t *pMsg = pUtils->AllocateMessage( cbPayload );
				pMsg->m_conn = hA;
				pMsg->m_nFlags = k_nSteamNetworkingSend_Reliable;
				vecSend[i] = pMsg;
			}
			SteamNetworkingSockets()->SendMessages( nBatch, vecSend.data(), nullptr, true );
		}
	}
	consumer.join();
	double ns = NsPerOp( start, Clock::now(), nTotal );
	SteamNetworkingSockets()->CloseConnection( hA, 0, nullptr, false );
	SteamNetworkingSockets()->CloseConnection( hB, 0, nullptr, false );
	return ns;
}

static double Median( std::vector<double> v )
{
	std::sort( v.begin(), v.end() );
	return v[ v.size() / 2 ];
}

int main( int argc, const char **argv )
{
	long long nIters = argc > 1 ? atoll( argv[1] ) : 2000000;
	const int nRuns = 7;

	SteamDatagramErrMsg errMsg;
	if ( !GameNetworkingSockets_Init( nullptr, errMsg ) )
	{
		fprintf( stderr, "GameNetworkingSockets_Init failed.  %s\n", errMsg );
		return 1;
	}
	// Don't let the send rate limit or buffer size throttle the pipe benchmarks
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_SendBufferSize, 64*1024*1024 );

	struct Result { const char *name; std::vector<double> ns; };
	std::vector<Result> results = {
		{ "alloc+release, 0 B payload", {} },
		{ "alloc+release, 64 B payload", {} },
		{ "alloc+release burst of 256, 64 B", {} },
		{ "pipe send+recv, 1 thread, 64 B, batch 64", {} },
		{ "pipe send+recv, 2 threads, 64 B, batch 64", {} },
	};
	for ( int run = 0 ; run < nRuns ; ++run )
	{
		results[0].ns.push_back( BenchAllocRelease( nIters, 0 ) );
		results[1].ns.push_back( BenchAllocRelease( nIters, 64 ) );
		results[2].ns.push_back( BenchAllocReleaseBurst( nIters, 64, 256 ) );
		results[3].ns.push_back( BenchPipeSameThread( nIters / 4, 64, 64 ) );
		results[4].ns.push_back( BenchPipeTwoThreads( nIters / 4, 64, 64 ) );
	}
	printf( "%-45s %12s %12s %12s\n", "benchmark (median of 7 runs)", "ns/msg", "min", "max" );
	for ( const Result &r: results )
		printf( "%-45s %12.1f %12.1f %12.1f\n", r.name, Median( r.ns ),
			*std::min_element( r.ns.begin(), r.ns.end() ), *std::max_element( r.ns.begin(), r.ns.end() ) );

	GameNetworkingSockets_Kill();
	return 0;
}
