find_package(JSONCPP REQUIRED)
find_package(JSONRPCCPP REQUIRED)

add_executable(
	gateway
	gateway.cpp)
target_include_directories(gateway PRIVATE ../src ../src/public ../src/common)
target_link_libraries(gateway GameNetworkingSockets_static jsoncpp jsonrpccpp-common jsonrpccpp-client)
add_sanitizers(gateway)

# vim: set ts=4 sts=4 sw=4 noet:
