cmake_minimum_required(VERSION 3.5)
project(gns_example C CXX)

find_library(golang REQUIRED)

#
# Trivial signaling server, written in go
#
set(SIGNAL_SERVER_TARGET trivial_signaling_server)
set(SIGNAL_SERVER_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/trivial_signaling_server)
set(SIGNAL_SERVER_SRCS
  trivial_signaling_server.go
)
add_custom_command(
    OUTPUT ${SIGNAL_SERVER_OUTPUT}
    DEPENDS ${SIGNAL_SERVER_SRCS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building GO Trivial signaling server"
    COMMAND env go build -o "${SIGNAL_SERVER_OUTPUT}" ${CMAKE_GO_FLAGS} ${SIGNAL_SERVER_SRCS}
)
add_custom_target(${SIGNAL_SERVER_TARGET}_all ALL DEPENDS ${SIGNAL_SERVER_OUTPUT})
#add_custom_target(${SIGNAL_SERVER_TARGET} ALL)
#add_custom_target(${SIGNAL_SERVER_OUTPUT} ALL)

#
# Really simple client/server chat
#

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
