#pragma once

// Expat's configuration, in place of the one its CMake writes. getentropy
// seeds its hash salt on macOS and glibc alike. DTDs and the general entities
// they declare are on, as in Expat's default build, bounded by its protection
// against amplification such as billion laughs.
#define HAVE_GETENTROPY 1
#define XML_CONTEXT_BYTES 1024
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1
