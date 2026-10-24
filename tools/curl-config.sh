#!/bin/sh
# Generates src/net/curl/<os>/curl_config.h, curl's configuration for this
# system: HTTP/1.1 and HTTP/2 over HTTPS, with gzip and deflate responses,
# mail over SMTPS or STARTTLS with MIME messages, and files over FTPS, on the
# mbedTLS, nghttp2 and zlib in deps/. Run it from the repository root with an
# unpacked curl release of the version in deps.lock, on macOS for macos and
# in ubuntu:24.04 (glibc 2.39) for linux:
#
#     tools/curl-config.sh ~/Downloads/curl-8.23.0 macos
#
# It needs cmake and a C compiler. Every feature libmc does not use is off.
set -eu
release="$1"
os="$2"
root="$(pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# curl's CMake links a test program against mbedTLS, so it needs a library to find.
mb="$root/deps/mbedtls"
for f in "$mb"/library/*.c "$mb"/tf-psa-crypto/core/*.c "$mb"/tf-psa-crypto/drivers/builtin/src/*.c \
         "$mb"/tf-psa-crypto/extras/*.c "$mb"/tf-psa-crypto/platform/*.c "$mb"/tf-psa-crypto/utilities/*.c; do
  cc -std=c11 -w -O1 -I"$mb/include" -I"$mb/tf-psa-crypto/include" -I"$mb/tf-psa-crypto/drivers/builtin/include" \
     -I"$mb/library" -I"$mb/tf-psa-crypto/core" -I"$mb/tf-psa-crypto/dispatch" -I"$mb/tf-psa-crypto/extras" \
     -I"$mb/tf-psa-crypto/platform" -I"$mb/tf-psa-crypto/utilities" -I"$mb/tf-psa-crypto/drivers/builtin/src" \
     -c "$f" -o "$work/$(echo "$f" | tr / _).o"
done
ar rcs "$work/libmbedtls.a" "$work"/*.o

# It finds nghttp2 and zlib as libraries too, built with tools/build.c's flags.
mkdir "$work/nghttp2" "$work/zlib"
for f in "$root"/deps/nghttp2/lib/*.c; do
  cc -std=gnu11 -w -I"$root/deps/nghttp2/lib/includes" -DBUILDING_NGHTTP2 -DNGHTTP2_STATICLIB -DHAVE_ARPA_INET_H \
     -DHAVE_NETINET_IN_H -DHAVE_CLOCK_GETTIME -DHAVE_DECL_CLOCK_MONOTONIC=1 -c "$f" -o "$work/nghttp2/$(basename "$f").o"
done
ar rcs "$work/libnghttp2.a" "$work"/nghttp2/*.o
for f in "$root"/deps/zlib/*.c; do
  cc -std=c11 -w -DHAVE_UNISTD_H=1 -c "$f" -o "$work/zlib/$(basename "$f").o"
done
ar rcs "$work/libz.a" "$work"/zlib/*.o

off=""
for feature in DICT FILE GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP TELNET TFTP IPFS WEBSOCKETS \
               NETRC KERBEROS_AUTH NEGOTIATE_AUTH AWS DOH PROXY BINDLOCAL CA_SEARCH FORM_API \
               COOKIES ALTSVC HSTS HTTPSIG; do
  off="$off -DCURL_DISABLE_$feature=ON"
done
mkdir "$work/build"
cd "$work/build"
cmake "$release" -DCMAKE_C_FLAGS="-I$mb/tf-psa-crypto/include -I$mb/tf-psa-crypto/drivers/builtin/include" $off \
  -DCURL_ENABLE_SMB=OFF -DCURL_ENABLE_NTLM=OFF \
  -DCURL_USE_MBEDTLS=ON -DCURL_USE_LIBPSL=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
  -DCURL_ZLIB=ON -DZLIB_INCLUDE_DIR="$root/deps/zlib" -DZLIB_LIBRARY="$work/libz.a" \
  -DUSE_NGHTTP2=ON -DNGHTTP2_INCLUDE_DIR="$root/deps/nghttp2/lib/includes" -DNGHTTP2_LIBRARY="$work/libnghttp2.a" \
  -DUSE_LIBIDN2=OFF -DUSE_APPLE_IDN=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
  -DENABLE_THREADED_RESOLVER=ON -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DBUILD_TESTING=OFF \
  -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none \
  -DMBEDTLS_INCLUDE_DIR="$mb/include" -DMBEDTLS_LIBRARY="$work/libmbedtls.a" \
  -DMBEDX509_LIBRARY="$work/libmbedtls.a" -DMBEDCRYPTO_LIBRARY="$work/libmbedtls.a" > cmake.log
mkdir -p "$root/src/net/curl/$os"
cp lib/curl_config.h "$root/src/net/curl/$os/curl_config.h"
echo "wrote src/net/curl/$os/curl_config.h"
