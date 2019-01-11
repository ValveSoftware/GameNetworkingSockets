find_package(OpenSSL REQUIRED)
if (USE_LIBSODIUM)
	find_package(sodium REQUIRED)
endif()

add_executable(
	test_connection
	test_connection.cpp)
target_link_libraries(test_connection GameNetworkingSockets)


set(TEST_CRYPTO_SRC
   "test_crypto.cpp"
   "../src/tier0/dbg.cpp"
   "../src/common/crypto.cpp"
   "../src/common/crypto_25519.cpp"
   "../src/common/crypto_25519_libsodium.cpp"
   "../src/common/crypto_textencode.cpp"
   "../src/common/keypair.cpp"
   "../src/common/opensslwrapper.cpp"
   "../src/tier0/cpu.cpp"
   "../src/tier0/platformtime.cpp"
   "../src/tier1/utlbuffer.cpp"
   "../src/tier1/utlmemory.cpp"
   "../src/vstdlib/strtools.cpp")

if (NOT USE_LIBSODIUM)
	set(TEST_CRYPTO_SRC ${TEST_CRYPTO_SRC}
	   "../src/external/curve25519-donna/curve25519.c"
	   "../src/external/curve25519-donna/curve25519_VALVE_sse2.c"
	   "../src/external/ed25519-donna/ed25519_VALVE.c"
	   "../src/external/ed25519-donna/ed25519_VALVE_sse2.c")
endif()

add_executable(
       test_crypto
	   ${TEST_CRYPTO_SRC}
       )
target_compile_definitions(test_crypto PRIVATE POSIX LINUX GNUC)
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto OpenSSL::Crypto)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU"
OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	target_compile_definitions(test_crypto PRIVATE GNUC GNU_COMPILER)
endif()
if(CMAKE_SYSTEM_NAME MATCHES Linux)
	target_compile_definitions(test_crypto PRIVATE POSIX LINUX)
elseif(CMAKE_SYSTEM_NAME MATCHES Darwin)
	target_compile_definitions(test_crypto PRIVATE POSIX OSX)
elseif(CMAKE_SYSTEM_NAME MATCHES Windows)
	target_compile_definitions(test_crypto PRIVATE WIN32)
endif()
if(USE_LIBSODIUM)
	target_compile_definitions(test_crypto PRIVATE USE_LIBSODIUM)
	target_include_directories(test_crypto PRIVATE ${sodium_INCLUDE_DIR})
	target_link_libraries(test_crypto ${sodium_LIBRARY_RELEASE})
endif(USE_LIBSODIUM)

#add_executable(
#	test_flat
#	test_flat.c)
#target_link_libraries(test_flat GameNetworkingSockets)
