#!/bin/sh
# Generates src/net/c-ares/<os>/ares_config.h, c-ares's configuration for
# this system, and deps/c-ares/include/ares_build.h, which comes out the same
# on both and is published with c-ares's source. Threads are on so a channel
# can run its own event thread. Run it from the repository root with an
# unpacked c-ares release of the version in deps.lock, on macOS for macos and
# in ubuntu:24.04 (glibc 2.39) for linux:
#
#     tools/c-ares-config.sh ~/Downloads/c-ares-1.34.9 macos
#
# It needs cmake and a C compiler.
set -eu
release="$1"
os="$2"
root="$(pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cd "$work"
cmake "$release" -DCARES_STATIC=ON -DCARES_SHARED=OFF -DCARES_THREADS=ON -DCARES_BUILD_TOOLS=OFF \
  -DCARES_BUILD_TESTS=OFF -DCARES_INSTALL=OFF > cmake.log
mkdir -p "$root/src/net/c-ares/$os"
cp ares_config.h "$root/src/net/c-ares/$os/"
cp ares_build.h "$root/deps/c-ares/include/"
echo "wrote src/net/c-ares/$os/ares_config.h and deps/c-ares/include/ares_build.h"
