#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <random>
#include <chrono>
#include <thread>

#include "test_common.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>


int main(  )
{

	// Create client and server sockets
	TEST_Init( nullptr );
	//SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( OnSteamNetConnectionStatusChanged );

	// Run the test
	//RunSteamDatagramConnectionTest();

	TEST_Kill();	
	return 0;
}
