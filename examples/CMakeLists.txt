#
# Copy trivial signaling server python script into the runtime dir
#
if( ENABLE_ICE AND ( BUILD_EXAMPLES OR BUILD_TESTS ) )
	set(SIGNAL_SERVER_TARGET trivial_signaling_server)
	set(SIGNAL_SERVER_OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/trivial_signaling_server.py)
	set(SIGNAL_SERVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/trivial_signaling_server.py)
	add_custom_command(
		OUTPUT ${SIGNAL_SERVER_OUTPUT}
		DEPENDS ${SIGNAL_SERVER_SRC}
		COMMENT "Publishing ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/trivial_signaling_server.py"
		COMMAND ${CMAKE_COMMAND} -E copy ${SIGNAL_SERVER_SRC} ${SIGNAL_SERVER_OUTPUT}
	)
	add_custom_target(${SIGNAL_SERVER_TARGET} ALL DEPENDS ${SIGNAL_SERVER_OUTPUT})
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
