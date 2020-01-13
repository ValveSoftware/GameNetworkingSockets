find_package(JSONCPP REQUIRED)
find_package(JSONRPCCPP REQUIRED)

if(CMAKE_SYSTEM_NAME MATCHES Linux)
	add_definitions(-DPOSIX)
	add_definitions(-DLINUX)
elseif(CMAKE_SYSTEM_NAME MATCHES Darwin)
	add_definitions(-DPOSIX)
	add_definitions(-DOSX)
else()
	message(FATAL_ERROR "Could not identify your target operating system")
endif()

add_executable(
	gateway
	gateway.cpp)
target_include_directories(gateway PUBLIC ../include ../src ../src/public ../src/common)
target_link_libraries(gateway GameNetworkingSockets_s jsoncpp jsonrpccpp-common jsonrpccpp-client libzmq3)
add_sanitizers(gateway)

# vim: set ts=4 sts=4 sw=4 noet:
