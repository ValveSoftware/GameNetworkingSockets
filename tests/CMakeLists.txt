if(LIGHT_TESTS)
	add_definitions(-DLIGHT_TESTS)
endif()
if(SANITIZE)
	add_definitions(-DSANITIZER)
	set( GAMENETWORKINGSOCKETS_LIB GameNetworkingSockets_s )
else()
	set( GAMENETWORKINGSOCKETS_LIB GameNetworkingSockets )
endif()

add_executable(
	test_connection
	test_common.cpp
	test_connection.cpp)
set_target_common_gns_properties( test_connection )
target_link_libraries(test_connection ${GAMENETWORKINGSOCKETS_LIB})
add_sanitizers(test_connection)

add_executable(
	test_crypto
	test_crypto.cpp
	)
set_target_common_gns_properties( test_crypto )
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto GameNetworkingSockets_s)
add_sanitizers(test_crypto)

# Test data for the crypto test
file(COPY aesgcmtestvectors DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# P2P test
if(USE_STEAMWEBRTC)
	add_executable(
		test_p2p
		test_common.cpp
		test_p2p.cpp
		../examples/trivial_signaling_client.cpp)
	set_target_common_gns_properties( test_p2p )
	target_link_libraries(test_p2p ${GAMENETWORKINGSOCKETS_LIB})

	# NOTE: This happens when we generate the projects, and really we'd like to actually
	# make a build rule to copy it.  I don't want to do that work right now.
	file(COPY test_p2p.py DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()
