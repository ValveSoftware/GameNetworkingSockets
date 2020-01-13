find_package(JSONCPP REQUIRED)
find_package(JSONRPCCPP REQUIRED)
find_package(curl REQUIRED)

add_executable(
	gateway
	gateway.cpp)
target_include_directories(../src ../src/public ../src/common)
target_link_libraries(gateway GameNetworkingSockets jsoncpp jsonrpccpp-common jsonrpccpp-client)
add_sanitizers(gateway)

# vim: set ts=4 sts=4 sw=4 noet:
