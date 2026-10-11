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

	set(XMPP_SERVER_TARGET xmpp_mock_server)
	set(XMPP_SERVER_OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/xmpp_mock_server.py)
	set(XMPP_SERVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/xmpp_mock_server.py)
	add_custom_command(
		OUTPUT ${XMPP_SERVER_OUTPUT}
		DEPENDS ${XMPP_SERVER_SRC}
		COMMENT "Publishing ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/xmpp_mock_server.py"
		COMMAND ${CMAKE_COMMAND} -E copy ${XMPP_SERVER_SRC} ${XMPP_SERVER_OUTPUT}
	)
	add_custom_target(${XMPP_SERVER_TARGET} ALL DEPENDS ${XMPP_SERVER_OUTPUT})

	add_library(
		xmpp_signaling_client STATIC
		xmpp_signaling_client.cpp
		xmpp_signaling_client.h)
	target_link_libraries(xmpp_signaling_client PUBLIC GameNetworkingSockets::shared)
	if( TARGET OpenSSL::SSL )
		target_link_libraries(xmpp_signaling_client PUBLIC OpenSSL::SSL)
		target_compile_definitions(xmpp_signaling_client PUBLIC XMPP_HAS_OPENSSL=1)
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
