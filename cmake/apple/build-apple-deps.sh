#!/usr/bin/env bash
#
# Build the GameNetworkingSockets third-party dependencies (OpenSSL and
# protobuf, which drags in Abseil) for an Apple platform, into a prefix that
# the main CMake build can be pointed at.
#
# You only need this for the platforms Homebrew does not cover -- that is,
# anything that isn't the Mac you are sitting at.  For a native macOS build
# just use the Homebrew packages; see BUILDING.md.
#
# Usage:
#   cmake/apple/build-apple-deps.sh [--platform P] [--arch A] [--min-version V] [options]
#
#   --platform    iphoneos | iphonesimulator | macosx    (default: iphoneos)
#   --arch        arm64 | x86_64                         (default: arm64)
#   --min-version deployment target                      (default: 15.0 iOS, 11.0 macOS)
#   --prefix      install prefix     (default: build/apple-deps/<platform>-<arch>)
#   --jobs N      parallel build jobs                    (default: sysctl hw.ncpu)
#   --clean       wipe the build and install trees for this platform/arch first
#
# The protobuf version defaults to whatever `protoc --version` reports, because
# the runtime we build here has to match the host protoc that generates the
# .pb.cc sources.  Override with PROTOBUF_VERSION= if you know what you're doing.
#

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

PLATFORM=iphoneos
ARCH=arm64
MIN_VERSION=
PREFIX=
JOBS="$(sysctl -n hw.ncpu)"
CLEAN=0

OPENSSL_VERSION="${OPENSSL_VERSION:-3.6.2}"
PROTOBUF_VERSION="${PROTOBUF_VERSION:-}"

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--platform)    PLATFORM="$2"; shift 2 ;;
		--arch)        ARCH="$2"; shift 2 ;;
		--min-version) MIN_VERSION="$2"; shift 2 ;;
		--prefix)      PREFIX="$2"; shift 2 ;;
		--jobs)        JOBS="$2"; shift 2 ;;
		--clean)       CLEAN=1; shift ;;
		-h|--help)     sed -n '2,30p' "$0"; exit 0 ;;
		*)             die "unknown argument '$1' (try --help)" ;;
	esac
done

#
# Translate platform/arch into the three different spellings the three
# different build systems want.
#
case "$PLATFORM" in
	iphoneos)
		CMAKE_SYSTEM_NAME=iOS
		SDK=iphoneos
		: "${MIN_VERSION:=15.0}"
		MIN_FLAG="-mios-version-min=$MIN_VERSION"
		[ "$ARCH" = "arm64" ] || die "iphoneos only supports --arch arm64"
		OPENSSL_TARGET=ios64-xcrun
		;;
	iphonesimulator)
		CMAKE_SYSTEM_NAME=iOS
		SDK=iphonesimulator
		: "${MIN_VERSION:=15.0}"
		MIN_FLAG="-mios-simulator-version-min=$MIN_VERSION"
		case "$ARCH" in
			arm64)  OPENSSL_TARGET=iossimulator-arm64-xcrun ;;
			x86_64) OPENSSL_TARGET=iossimulator-x86_64-xcrun ;;
			*)      die "iphonesimulator supports --arch arm64 or x86_64" ;;
		esac
		;;
	macosx)
		CMAKE_SYSTEM_NAME=Darwin
		SDK=macosx
		: "${MIN_VERSION:=11.0}"
		MIN_FLAG="-mmacosx-version-min=$MIN_VERSION"
		case "$ARCH" in
			arm64)  OPENSSL_TARGET=darwin64-arm64-cc ;;
			x86_64) OPENSSL_TARGET=darwin64-x86_64-cc ;;
			*)      die "macosx supports --arch arm64 or x86_64" ;;
		esac
		;;
	*)
		die "unknown --platform '$PLATFORM' (iphoneos, iphonesimulator, macosx)"
		;;
esac

command -v cmake  >/dev/null || die "cmake not found; 'brew install cmake'"
command -v ninja  >/dev/null || die "ninja not found; 'brew install ninja'"
command -v xcrun  >/dev/null || die "xcrun not found; install Xcode and run 'sudo xcode-select -s /Applications/Xcode.app'"
xcrun --sdk "$SDK" --show-sdk-path >/dev/null 2>&1 || die "the '$SDK' SDK is not installed in this Xcode"

if [ -z "$PROTOBUF_VERSION" ]; then
	command -v protoc >/dev/null || die "protoc not found; 'brew install protobuf' (or set PROTOBUF_VERSION=)"
	# "libprotoc 34.1" -> "34.1"
	PROTOBUF_VERSION="$(protoc --version | awk '{print $2}')"
	[ -n "$PROTOBUF_VERSION" ] || die "could not parse 'protoc --version' output"
fi

DEPS_DIR="$REPO_ROOT/build/apple-deps"
SRC_DIR="$DEPS_DIR/src"
WORK_DIR="$DEPS_DIR/work/$PLATFORM-$ARCH"
: "${PREFIX:=$DEPS_DIR/$PLATFORM-$ARCH}"

if [ "$CLEAN" = "1" ]; then
	rm -rf "$WORK_DIR" "$PREFIX"
fi
mkdir -p "$SRC_DIR" "$WORK_DIR" "$PREFIX"

echo "==> platform     : $PLATFORM / $ARCH (min $MIN_VERSION)"
echo "==> openssl      : $OPENSSL_VERSION ($OPENSSL_TARGET)"
echo "==> protobuf     : v$PROTOBUF_VERSION (matching host protoc)"
echo "==> prefix       : $PREFIX"

###############################################################################
# OpenSSL
###############################################################################

OPENSSL_SRC="$SRC_DIR/openssl-$OPENSSL_VERSION"
if [ ! -d "$OPENSSL_SRC" ]; then
	echo "==> downloading OpenSSL $OPENSSL_VERSION"
	curl -fL --retry 3 -o "$SRC_DIR/openssl-$OPENSSL_VERSION.tar.gz" \
		"https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
	tar -xf "$SRC_DIR/openssl-$OPENSSL_VERSION.tar.gz" -C "$SRC_DIR"
fi

if [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
	echo "==> building OpenSSL"
	OPENSSL_BUILD="$WORK_DIR/openssl"
	rm -rf "$OPENSSL_BUILD"
	mkdir -p "$OPENSSL_BUILD"
	(
		cd "$OPENSSL_BUILD"
		# We only use libcrypto, and only from a statically linked library.
		# no-shared keeps us from having to sign and embed a dylib; the rest
		# just trims build time and binary size.
		"$OPENSSL_SRC/Configure" "$OPENSSL_TARGET" \
			--prefix="$PREFIX" \
			--openssldir="$PREFIX/ssl" \
			--libdir=lib \
			no-shared \
			no-dso \
			no-tests \
			no-legacy \
			no-docs \
			"$MIN_FLAG"
		make -j"$JOBS" build_libs
		make install_dev
	)
else
	echo "==> OpenSSL already built, skipping (--clean to rebuild)"
fi

###############################################################################
# protobuf (+ Abseil, which protobuf fetches and installs alongside itself)
###############################################################################

PROTOBUF_SRC="$SRC_DIR/protobuf-$PROTOBUF_VERSION"
if [ ! -d "$PROTOBUF_SRC" ]; then
	echo "==> cloning protobuf v$PROTOBUF_VERSION"
	git clone --depth 1 --branch "v$PROTOBUF_VERSION" \
		--recurse-submodules --shallow-submodules \
		https://github.com/protocolbuffers/protobuf.git "$PROTOBUF_SRC"
fi

if [ ! -f "$PREFIX/lib/libprotobuf.a" ]; then
	echo "==> building protobuf"
	PROTOBUF_BUILD="$WORK_DIR/protobuf"
	rm -rf "$PROTOBUF_BUILD"
	cmake -S "$PROTOBUF_SRC" -B "$PROTOBUF_BUILD" -G Ninja \
		-DCMAKE_SYSTEM_NAME="$CMAKE_SYSTEM_NAME" \
		-DCMAKE_OSX_SYSROOT="$SDK" \
		-DCMAKE_OSX_ARCHITECTURES="$ARCH" \
		-DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_VERSION" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		`# Abseil bakes the resolved C++ standard into the absl/base/options.h` \
		`# it installs.  If it is configured below C++17 it installs headers` \
		`# saying absl::string_view is Abseil's own class, while protobuf's own` \
		`# objects were compiled against std::string_view -- and every protobuf` \
		`# entry point taking a string_view then fails to link.  Pin both.` \
		-DCMAKE_CXX_STANDARD=17 \
		-DCMAKE_CXX_STANDARD_REQUIRED=ON \
		-DABSL_PROPAGATE_CXX_STD=ON \
		-DBUILD_SHARED_LIBS=OFF \
		-Dprotobuf_BUILD_SHARED_LIBS=OFF \
		-Dprotobuf_BUILD_TESTS=OFF \
		-Dprotobuf_BUILD_EXAMPLES=OFF \
		-Dprotobuf_BUILD_CONFORMANCE=OFF \
		-Dprotobuf_BUILD_LIBUPB=OFF \
		-Dprotobuf_INSTALL=ON \
		-Dprotobuf_WITH_ZLIB=OFF \
		`# No host-runnable binaries out of a cross build; the main GNS build` \
		`# uses the host protoc instead.` \
		-Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
		-Dprotobuf_BUILD_LIBPROTOC=OFF
	cmake --build "$PROTOBUF_BUILD" -j "$JOBS"
	cmake --install "$PROTOBUF_BUILD"
else
	echo "==> protobuf already built, skipping (--clean to rebuild)"
fi

echo
echo "==> done.  Configure GameNetworkingSockets with:"
echo
echo "    cmake -S . -B build/$PLATFORM-$ARCH -G Ninja \\"
echo "        -DCMAKE_SYSTEM_NAME=$CMAKE_SYSTEM_NAME \\"
echo "        -DCMAKE_OSX_SYSROOT=$SDK \\"
echo "        -DCMAKE_OSX_ARCHITECTURES=$ARCH \\"
echo "        -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_VERSION \\"
echo "        -DCMAKE_PREFIX_PATH=$PREFIX \\"
echo "        -DCMAKE_FIND_ROOT_PATH=$PREFIX \\"
echo "        -DOPENSSL_ROOT_DIR=$PREFIX"
echo
