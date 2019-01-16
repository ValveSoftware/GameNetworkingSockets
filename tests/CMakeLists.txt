if(LIGHT_TESTS)
    add_compile_definitions(LIGHT_TESTS)
endif()

add_executable(
	test_connection
	test_connection.cpp)
target_link_libraries(test_connection GameNetworkingSockets)

add_executable(
       test_crypto
	   test_crypto.cpp
       )
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto GameNetworkingSockets_s)
