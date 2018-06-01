add_executable(
	test_connection
	test_connection.cpp)
add_executable(
	test_flat
	test_flat.c)
target_link_libraries(test_connection GameNetworkingSockets)
target_link_libraries(test_flat GameNetworkingSockets)
