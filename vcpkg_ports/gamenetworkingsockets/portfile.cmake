set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")

if(openssl IN_LIST FEATURES)
    set(CRYPTO_BACKEND OpenSSL)
endif()

if(libsodium IN_LIST FEATURES)
    set(CRYPTO_BACKEND libsodium)
endif()

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
      webrtc USE_STEAMWEBRTC
)

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}
    PREFER_NINJA
    OPTIONS
    -DGAMENETWORKINGSOCKETS_BUILD_TESTS=OFF
    -DGAMENETWORKINGSOCKETS_BUILD_EXAMPLES=OFF
    -DUSE_CRYPTO=${CRYPTO_BACKEND}
    -DUSE_CRYPTO25519=${CRYPTO_BACKEND}
    ${FEATURE_OPTIONS}
)

vcpkg_install_cmake()
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

vcpkg_fixup_cmake_targets(CONFIG_PATH "lib/cmake/GameNetworkingSockets" TARGET_PATH "share/GameNetworkingSockets")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")


vcpkg_copy_pdbs()