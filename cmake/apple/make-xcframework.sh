#!/usr/bin/env bash
#
# Build GameNetworkingSockets for iOS device + iOS simulator (and optionally
# macOS) and package the results as a single GameNetworkingSockets.xcframework
# that can be dropped into an Xcode project.
#
# This assumes the dependencies for each slice have already been built by
# cmake/apple/build-apple-deps.sh; pass --with-deps to have this script run that for
# you.
#
# Usage:
#   cmake/apple/make-xcframework.sh [options]
#
#   --with-deps        build the OpenSSL/protobuf dependencies first
#   --min-version V    iOS deployment target                 (default: 15.0)
#   --macos            also include a macOS slice
#   --macos-version V  macOS deployment target               (default: 11.0)
#   --sim-archs "..."  simulator architectures    (default: "arm64 x86_64")
#   --no-bundle-deps   do NOT merge OpenSSL/protobuf/Abseil into the library
#                      (consumers then have to link those themselves)
#   --output PATH      where to write the xcframework
#                      (default: build/GameNetworkingSockets.xcframework)
#   --jobs N           parallel build jobs
#

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

MIN_VERSION=15.0
MACOS_VERSION=11.0
WITH_MACOS=0
WITH_DEPS=0
SIM_ARCHS="arm64 x86_64"
BUNDLE_DEPS=1
OUTPUT="$REPO_ROOT/build/GameNetworkingSockets.xcframework"
JOBS="$(sysctl -n hw.ncpu)"

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--with-deps)      WITH_DEPS=1; shift ;;
		--min-version)    MIN_VERSION="$2"; shift 2 ;;
		--macos)          WITH_MACOS=1; shift ;;
		--macos-version)  MACOS_VERSION="$2"; shift 2 ;;
		--sim-archs)      SIM_ARCHS="$2"; shift 2 ;;
		--no-bundle-deps) BUNDLE_DEPS=0; shift ;;
		--output)         OUTPUT="$2"; shift 2 ;;
		--jobs)           JOBS="$2"; shift 2 ;;
		-h|--help)        sed -n '2,28p' "$0"; exit 0 ;;
		*)                die "unknown argument '$1' (try --help)" ;;
	esac
done

command -v protoc >/dev/null || die "protoc not found; 'brew install protobuf'"

DEPS_DIR="$REPO_ROOT/build/apple-deps"
STAGE_DIR="$REPO_ROOT/build/xcframework-stage"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

#
# Build one (platform, arch) slice of the static library and echo the path to
# the resulting .a
#
build_slice() {
	local platform="$1" arch="$2" minver="$3" sysname="$4"
	local prefix="$DEPS_DIR/$platform-$arch"
	local builddir="$REPO_ROOT/build/$platform-$arch"

	if [ "$WITH_DEPS" = "1" ]; then
		"$REPO_ROOT/cmake/apple/build-apple-deps.sh" \
			--platform "$platform" --arch "$arch" \
			--min-version "$minver" --jobs "$JOBS" >&2
	fi
	[ -f "$prefix/lib/libcrypto.a" ] || die "no dependencies at $prefix -- run cmake/apple/build-apple-deps.sh --platform $platform --arch $arch (or pass --with-deps)"

	cmake -S "$REPO_ROOT" -B "$builddir" -G Ninja \
		-DCMAKE_SYSTEM_NAME="$sysname" \
		-DCMAKE_OSX_SYSROOT="$platform" \
		-DCMAKE_OSX_ARCHITECTURES="$arch" \
		-DCMAKE_OSX_DEPLOYMENT_TARGET="$minver" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_PREFIX_PATH="$prefix" \
		-DCMAKE_FIND_ROOT_PATH="$prefix" \
		-DOPENSSL_ROOT_DIR="$prefix" \
		-DBUILD_STATIC_LIB=ON \
		-DBUILD_SHARED_LIB=OFF >&2
	cmake --build "$builddir" -j "$JOBS" >&2

	local lib="$builddir/src/libGameNetworkingSockets_s.a"
	[ -f "$lib" ] || die "expected $lib to exist after the build"

	if [ "$BUNDLE_DEPS" = "1" ]; then
		merge_archives "$platform-$arch" "$lib" "$prefix/lib"
	else
		echo "$lib"
	fi
}

#
# Fold OpenSSL, protobuf and Abseil into one archive so that the xcframework is
# self-contained.  Without this a consumer has to track down and link ~90
# separate archives by hand.
#
# Handing all of those archives straight to 'libtool -static' does not work:
# several of them contain members with the same name (libprotobuf.a and
# libprotobuf-lite.a both have message_lite.cc.o, for instance), and the
# linker resolves archive members by name, so it silently picks whichever one
# came first and you get undefined symbols at link time.  Explode everything
# into uniquely named object files first.
#
merge_archives() {
	local tag="$1" mainlib="$2" depdir="$3"
	local merged="$STAGE_DIR/$tag-merged.a"
	local objdir="$STAGE_DIR/$tag-objs"
	local filelist="$STAGE_DIR/$tag-objs.txt"

	rm -rf "$objdir"; mkdir -p "$objdir"

	local archive base sub
	while IFS= read -r archive; do
		base="$(basename "$archive" .a)"
		sub="$objdir/$base"
		mkdir -p "$sub"
		( cd "$sub" && ar -x "$archive" )
		# Prefix every object with the archive it came from so that no two
		# members of the merged archive share a name.
		local obj
		for obj in "$sub"/*.o; do
			[ -e "$obj" ] || continue
			mv "$obj" "$objdir/${base}__$(basename "$obj")"
		done
		rmdir "$sub" 2>/dev/null || true
	done < <( { echo "$mainlib"; find "$depdir" -name '*.a'; } | sort -u )

	find "$objdir" -maxdepth 1 -name '*.o' | sort > "$filelist"
	libtool -static -no_warning_for_no_symbols -o "$merged" -filelist "$filelist"
	rm -rf "$objdir" "$filelist"
	echo "$merged"
}

# Combine per-arch archives into one fat archive
lipo_slices() {
	local out="$1"; shift
	if [ "$#" -eq 1 ]; then
		cp "$1" "$out"
	else
		lipo -create "$@" -output "$out"
	fi
	echo "$out"
}

XCFRAMEWORK_ARGS=()

echo "==> iOS device (arm64)"
IOS_LIB="$(build_slice iphoneos arm64 "$MIN_VERSION" iOS)"
cp "$IOS_LIB" "$STAGE_DIR/ios-device.a"
XCFRAMEWORK_ARGS+=(-library "$STAGE_DIR/ios-device.a" -headers "$REPO_ROOT/include")

echo "==> iOS simulator ($SIM_ARCHS)"
SIM_LIBS=()
for arch in $SIM_ARCHS; do
	SIM_LIBS+=("$(build_slice iphonesimulator "$arch" "$MIN_VERSION" iOS)")
done
lipo_slices "$STAGE_DIR/ios-simulator.a" "${SIM_LIBS[@]}" >/dev/null
XCFRAMEWORK_ARGS+=(-library "$STAGE_DIR/ios-simulator.a" -headers "$REPO_ROOT/include")

if [ "$WITH_MACOS" = "1" ]; then
	echo "==> macOS (arm64 x86_64)"
	MAC_LIBS=()
	for arch in arm64 x86_64; do
		MAC_LIBS+=("$(build_slice macosx "$arch" "$MACOS_VERSION" Darwin)")
	done
	lipo_slices "$STAGE_DIR/macos.a" "${MAC_LIBS[@]}" >/dev/null
	XCFRAMEWORK_ARGS+=(-library "$STAGE_DIR/macos.a" -headers "$REPO_ROOT/include")
fi

echo "==> packaging $OUTPUT"
rm -rf "$OUTPUT"
mkdir -p "$(dirname "$OUTPUT")"
xcodebuild -create-xcframework "${XCFRAMEWORK_ARGS[@]}" -output "$OUTPUT"

echo
echo "==> done: $OUTPUT"
if [ "$BUNDLE_DEPS" = "1" ]; then
	echo "    OpenSSL, protobuf and Abseil are bundled in.  Consumers only need"
	echo "    to add the xcframework itself, plus -lc++ (C++ standard library)."
else
	echo "    Dependencies are NOT bundled; consumers must also link libcrypto,"
	echo "    libprotobuf and Abseil for the matching platform."
fi
