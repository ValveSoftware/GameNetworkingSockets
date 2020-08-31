if(LIGHT_TESTS)
	add_definitions(-DLIGHT_TESTS)
endif()
if(SANITIZE)
	add_definitions(-DSANITIZER)
endif()

add_executable(
	test_connection
	test_common.cpp
	test_connection.cpp)
add_executable(
	test_p2p
	test_common.cpp
	test_p2p.cpp
	../examples/trivial_signaling_client.cpp)
if(SANITIZE)
	target_link_libraries(test_connection GameNetworkingSockets_s)
	target_link_libraries(test_p2p GameNetworkingSockets_s)
else()
	target_link_libraries(test_connection GameNetworkingSockets)
	target_link_libraries(test_p2p GameNetworkingSockets)
endif()
add_sanitizers(test_connection)

add_executable(
	test_crypto
	test_crypto.cpp
	)
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto GameNetworkingSockets_s)
add_sanitizers(test_crypto)

file(COPY aesgcmtestvectors DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# vim: set ts=4 sts=4 sw=4 noet:
