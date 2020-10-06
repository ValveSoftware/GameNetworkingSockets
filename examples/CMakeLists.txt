cmake_minimum_required(VERSION 3.9)
project(gns_example C CXX)

#
# Trivial signaling server, written in go
#
if( NOT WIN32 AND USE_STEAMWEBRTC )
	find_program( GO go )
	if ( NOT GO )
		message(WARNING "Could not find 'go' binary, will not build signaling server example program")
	else()
		set(SIGNAL_SERVER_TARGET trivial_signaling_server)

		set(SIGNAL_SERVER_OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/trivial_signaling_server)
		set(SIGNAL_SERVER_SRCS
			trivial_signaling_server.go
			)
		add_custom_command(
			OUTPUT ${SIGNAL_SERVER_OUTPUT}
			DEPENDS ${SIGNAL_SERVER_SRCS}
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
			COMMENT "Building GO Trivial signaling server"
			COMMAND ${GO} build -o "${SIGNAL_SERVER_OUTPUT}" ${CMAKE_GO_FLAGS} ${SIGNAL_SERVER_SRCS}
			)
		add_custom_target(${SIGNAL_SERVER_TARGET} ALL DEPENDS ${SIGNAL_SERVER_OUTPUT})
	endif()
endif()

#
# Really simple client/server chat
#

add_executable(
	example_chat
	example_chat.cpp)

if(COMMAND set_target_common_gns_properties)
	set_target_common_gns_properties( example_chat )
endif()

# If building the example as a standalone project, need to find GameNetworkingSockets
if(${CMAKE_PROJECT_NAME} STREQUAL "gns_example")
	find_package(GameNetworkingSockets CONFIG REQUIRED)
endif()

target_link_libraries(example_chat GameNetworkingSockets::GameNetworkingSockets)

if(COMMAND add_sanitizers)
	add_sanitizers(example_chat)
endif()
