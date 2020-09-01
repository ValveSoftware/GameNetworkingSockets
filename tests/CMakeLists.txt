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

file(COPY aesgcmtestvectors DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# NOTE: This happens when we generate the projects, and really we'd like to actually
# make a build rule to copy it.  I don't want to do that work right now.
file(COPY test_p2p.py DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
