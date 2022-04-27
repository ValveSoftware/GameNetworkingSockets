if(SANITIZE)
	add_definitions(-DSANITIZER)
endif()

# Let's always link all tests statically with the lib
# To keep things simple.  (Since some of the tests MUST
# link statically)
set( GAMENETWORKINGSOCKETS_LIB "GameNetworkingSockets::static" )

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
target_link_libraries(test_crypto GameNetworkingSockets::static)
add_sanitizers(test_crypto)

# Test data for the crypto test when the project is built
file(COPY aesgcmtestvectors DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# P2P test
if(ENABLE_ICE)
	add_executable(
		test_p2p
		test_common.cpp
		test_p2p.cpp
		../examples/trivial_signaling_client.cpp)
	set_target_common_gns_properties( test_p2p )
	target_link_libraries(test_p2p ${GAMENETWORKINGSOCKETS_LIB})

	# Publish the test script
	set( P2P_TEST_SCRIPT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_p2p.py )
	set( P2P_TEST_SCRIPT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_p2p.py )
	add_custom_command( 
		OUTPUT ${P2P_TEST_SCRIPT} 
		COMMAND ${CMAKE_COMMAND} -E copy ${P2P_TEST_SCRIPT_SRC} ${P2P_TEST_SCRIPT}
		DEPENDS ${P2P_TEST_SCRIPT_SRC} )
	add_custom_target( publish_test_script ALL DEPENDS ${P2P_TEST_SCRIPT} )
endif()
