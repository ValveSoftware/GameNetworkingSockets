#!/usr/bin/env bash
#
# Build tests/ios_smoke_test.cpp against the iOS simulator slice of the
# xcframework and run it inside an iOS Simulator.  It stands up a listen socket
# on loopback, connects to it, and round-trips a reliable message -- which
# exercises the crypto backend, protobuf, the socket thread and SNP, i.e. all
# the parts of the port that could plausibly be broken.
#
# Run cmake/apple/make-xcframework.sh first.
#
# Usage:
#   cmake/apple/run-ios-smoke-test.sh [--xcframework PATH] [--device NAME_OR_UDID]
#

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

XCFRAMEWORK="$REPO_ROOT/build/GameNetworkingSockets.xcframework"
DEVICE=
MIN_VERSION=15.0

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--xcframework) XCFRAMEWORK="$2"; shift 2 ;;
		--device)      DEVICE="$2"; shift 2 ;;
		--min-version) MIN_VERSION="$2"; shift 2 ;;
		-h|--help)     sed -n '2,14p' "$0"; exit 0 ;;
		*)             die "unknown argument '$1' (try --help)" ;;
	esac
done

SLICE="$XCFRAMEWORK/ios-arm64_x86_64-simulator/ios-simulator.a"
[ -f "$SLICE" ] || die "no simulator slice at $SLICE -- run cmake/apple/make-xcframework.sh first"

HOST_ARCH="$(uname -m)"
SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"
BIN="$(mktemp -d)/ios_smoke_test"

echo "==> building the test for $HOST_ARCH-apple-ios$MIN_VERSION-simulator"
xcrun --sdk iphonesimulator clang++ -std=c++17 \
	-target "$HOST_ARCH-apple-ios$MIN_VERSION-simulator" \
	-isysroot "$SDK_PATH" \
	-I"$REPO_ROOT/include" \
	-DSTEAMNETWORKINGSOCKETS_STATIC_LINK \
	"$REPO_ROOT/tests/ios_smoke_test.cpp" \
	"$SLICE" \
	-framework CoreFoundation \
	-o "$BIN"

#
# Find something to run it on.  Boot it if it isn't already; leave it booted if
# it was, so we don't yank a simulator out from under the user.
#
BOOTED_BY_US=0
if [ -z "$DEVICE" ]; then
	# '|| true' matters: with 'set -e', an assignment from a command
	# substitution that exits nonzero (grep finding nothing) kills the script.
	DEVICE="$(xcrun simctl list devices booted | grep -oE '\(([0-9A-F-]{36})\)' | head -1 | tr -d '()' || true)"
	if [ -z "$DEVICE" ]; then
		DEVICE="$(xcrun simctl list devices available | grep -E 'iPhone' | head -1 | grep -oE '\(([0-9A-F-]{36})\)' | head -1 | tr -d '()' || true)"
		[ -n "$DEVICE" ] || die "no iPhone simulator available; create one in Xcode"
		echo "==> booting simulator $DEVICE"
		xcrun simctl boot "$DEVICE"
		BOOTED_BY_US=1
		# simctl boot returns before the device is usable
		xcrun simctl bootstatus "$DEVICE" >/dev/null 2>&1 || true
	fi
fi

echo "==> running on simulator $DEVICE"
set +e
xcrun simctl spawn "$DEVICE" "$BIN"
rc=$?
set -e

if [ "$BOOTED_BY_US" = "1" ]; then
	xcrun simctl shutdown "$DEVICE" >/dev/null 2>&1 || true
fi

rm -rf "$(dirname "$BIN")"

if [ "$rc" -ne 0 ]; then
	echo "==> FAILED (exit $rc)"
	exit "$rc"
fi
echo "==> smoke test passed"
