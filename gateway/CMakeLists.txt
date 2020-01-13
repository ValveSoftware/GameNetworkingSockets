find_package(JSONCPP REQUIRED)
find_package(JSONRPCCPP REQUIRED)

if(CMAKE_SYSTEM_NAME MATCHES Linux)
	target_compile_definitions(gateway PUBLIC POSIX LINUX)
elseif(CMAKE_SYSTEM_NAME MATCHES Darwin)
	target_compile_definitions(gateway PUBLIC POSIX OSX)
elseif(CMAKE_SYSTEM_NAME MATCHES Windows)
	target_compile_definitions(gateway PUBLIC WIN32)
	if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
		if(NOT Protobuf_USE_STATIC_LIBS)
			target_compile_definitions(gateway PRIVATE
				PROTOBUF_USE_DLLS)
		endif()
		get_target_property(TARGET_TYPE gateway TYPE)
		target_compile_options(gateway PRIVATE
			/EHs-c-   # Disable C++ exceptions
			/GR-      # Disable RTTI

			# Below are warnings we can't fix and don't want to see (mostly from protobuf, some from MSVC standard library)
			/wd4146   # include/google/protobuf/wire_format_lite.h(863): warning C4146: unary minus operator applied to unsigned type, result still unsigned
			/wd4530   # .../xlocale(319): warning C4530: C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc
			/wd4244   # google/protobuf/wire_format_lite.h(935): warning C4244: 'argument': conversion from 'google::protobuf::uint64' to 'google::protobuf::uint32', possible loss of data
			/wd4251   # 'google::protobuf::io::CodedOutputStream::default_serialization_deterministic_': struct 'std::atomic<bool>' needs to have dll-interface to be used by clients of class 
			)
		if(NOT TARGET_TYPE STREQUAL STATIC_LIBRARY)
			target_compile_options(gateway PRIVATE
				/GL       # Enable link-time code generation
				)
			set_target_properties(gateway PROPERTIES LINK_FLAGS "/LTCG /SUBSYSTEM:WINDOWS")
		endif()
	else()
		target_compile_definitions(gateway PRIVATE
			__STDC_FORMAT_MACROS=1
			__USE_MINGW_ANSI_STDIO=0
			)
		target_compile_options(gateway PRIVATE -fno-stack-protector)
	endif()
	target_link_libraries(gateway PUBLIC ws2_32 crypt32)
	if(USE_BCRYPT)
		target_link_libraries(gateway PUBLIC bcrypt)
	endif()
else()
	message(FATAL_ERROR "Could not identify your target operating system")
endif()

add_executable(
	gateway
	gateway.cpp)
target_include_directories(gateway PUBLIC ../src ../src/public ../src/common)
target_link_libraries(gateway GameNetworkingSockets_static jsoncpp jsonrpccpp-common jsonrpccpp-client)
add_sanitizers(gateway)

# vim: set ts=4 sts=4 sw=4 noet:
