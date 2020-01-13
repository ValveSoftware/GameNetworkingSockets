add_executable(
	gateway
	gateway.cpp)
target_link_libraries(gateway GameNetworkingSockets)
add_sanitizers(gateway)

# vim: set ts=4 sts=4 sw=4 noet:
