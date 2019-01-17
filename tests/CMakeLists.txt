if(LIGHT_TESTS)
    add_definitions(-DLIGHT_TESTS)
endif()
if(SANITIZE_ADDRESS OR SANITIZE_THREAD OR SANITIZE_MEMORY OR SANITIZE_UNDEFINED)
	add_definitions(-DSANITIZER)
endif()

add_executable(
    test_connection
    test_connection.cpp)
target_link_libraries(test_connection GameNetworkingSockets)
add_sanitizers(test_connection)

add_executable(
       test_crypto
       test_crypto.cpp
       )
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto GameNetworkingSockets_s)
add_sanitizers(test_crypto)
