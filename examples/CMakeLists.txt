cmake_minimum_required(VERSION 3.5)
project(gns_example C CXX)

add_executable(
	example_chat
	example_chat.cpp)

# If building the example as a standalone project, need to find GameNetworkingSockets
if(${CMAKE_PROJECT_NAME} STREQUAL "gns_example")
	find_package(GameNetworkingSockets CONFIG REQUIRED)
endif()

target_link_libraries(example_chat GameNetworkingSockets::GameNetworkingSockets)

if(COMMAND add_sanitizers)
	add_sanitizers(example_chat)
endif()

# vim: set ts=4 sts=4 sw=4 noet:
