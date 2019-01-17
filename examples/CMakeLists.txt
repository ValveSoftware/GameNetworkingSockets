add_executable(
	example_chat
	example_chat.cpp)
target_link_libraries(example_chat GameNetworkingSockets)
add_sanitizers(example_chat)

# vim: set ts=4 sts=4 sw=4 noet:
