#
# Trivial signaling server, written in go
#
if( ENABLE_ICE AND ( BUILD_EXAMPLES OR BUILD_TESTS ) )
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
# Examples
#
if( BUILD_EXAMPLES )

	#
	# Really simple client/server chat
	#

	add_executable(
		example_chat
		example_chat.cpp)

	target_link_libraries(example_chat GameNetworkingSockets::shared)

	add_sanitizers(example_chat)

endif()
