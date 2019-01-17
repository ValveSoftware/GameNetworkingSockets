if(LIGHT_TESTS)
    add_definitions(-DLIGHT_TESTS)
endif()
if(SANITIZE)
	add_definitions(-DSANITIZER)
endif()

add_executable(
    test_connection
    test_connection.cpp)
if(SANITIZE)
	target_link_libraries(test_connection GameNetworkingSockets_s)
else()
	target_link_libraries(test_connection GameNetworkingSockets)
endif()
add_sanitizers(test_connection)

add_executable(
       test_crypto
       test_crypto.cpp
       )
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto GameNetworkingSockets_s)
add_sanitizers(test_crypto)
